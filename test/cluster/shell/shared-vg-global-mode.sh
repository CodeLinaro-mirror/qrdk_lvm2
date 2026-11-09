#!/bin/bash
# Test vgchange --locksettings global_mode=1|2
#
# The global mode of a shared VG is recorded as tags on its lvmlock LV
# (LOCKD_GLOBAL_MODE_1/2, LOCKD_GLOBAL_LOCK_1/2) and in the lock_args
# version.  vgchange --locksettings changes it, and repairs the tags.
#
# - with persistent_reservations=none, new VGs bootstrap global
#   mode 1, the first one holds the LOCKD_GLOBAL_LOCK_1 tag
# - with the default persistent_reservations=auto, new VGs bootstrap
#   global mode 2, the first one holds the LOCKD_GLOBAL_LOCK_2 tag
# - an explicit lvmlockd_global=global_mode_2 conflicts with VGs using
#   global mode 1 on disk, and persistent_reservations=none conflicts
#   with VGs using global mode 2 on disk
# - global_mode=2 on a VG already in mode 2 is a no-op
# - converting one VG leaves a mix of modes, in which new lvm refuses
#   global lock requests until all VGs are converted
# - the GLLK is disabled in every VG using global mode 2, so old lvm versions
#   find no global lock to acquire
# - global_mode=1 enables the GLLK and sets LOCKD_GLOBAL_LOCK_1 on the first
#   converted VG, and leaves the GLLK disabled in the others
# - global_mode=2 repairs the missing LOCKD_GLOBAL_LOCK_2 after the VG
#   holding it is removed
# - conversion requires the lockspace stopped on the other hosts

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

# has_tag <vg> <tag>: the lvmlock LV of the VG has the tag
has_tag() {
    noden 1 "lvs -a --noheadings -o lv_tags $1/lvmlock" > out
    grep -q "$2" out
}

check_tags() {
    local vg=$1 mode1=$2 mode2=$3 lock1=$4 lock2=$5

    for t in "MODE_1 $mode1" "MODE_2 $mode2" "LOCK_1 $lock1" "LOCK_2 $lock2"; do
        set -- $t
        if has_tag $vg LOCKD_GLOBAL_$1; then
            if [ "$2" != "1" ]; then
                echo "ERROR: $vg has unexpected tag LOCKD_GLOBAL_$1"
                exit 1
            fi
        else
            if [ "$2" != "0" ]; then
                echo "ERROR: $vg is missing tag LOCKD_GLOBAL_$1"
                exit 1
            fi
        fi
    done
}

# Other hosts that just stopped the lockspace may still have an unknown host
# state, so retry for a while.
retry_node1() {
    local i
    for i in $(seq 1 24); do
        if noden 1 "$1 2>&1" > out; then
            cat out
            return 0
        fi
        cat out
        grep -q "unknown host state" out || return 1
        sleep 5
    done
    return 1
}

# check_gllk <vg> enabled|disabled: the GLLK name on disk (read through the
# lvmlock LV, so the lockspace must be started on node1.  The GLLK is at lease
# 65, which is 68157440 bytes with 512 byte sectors and 1MiB align.)
check_gllk() {
    noden 1 "sanlock direct dump /dev/mapper/$1-lvmlock:68157440:1048576" > out
    cat out
    if [ "$2" = "enabled" ]; then
        grep -qw GLLK out
    else
        grep -q _GLLK_disabled out
    fi
}

locksettings() {
    retry_node1 "vgchange --locksettings $1 $2"
}

check_lock_args() {
    noden 1 "vgs --noheadings -o lock_args $1" > out
    cat out
    grep -q "$2" out
}

echo "== Test 1: new VGs use global mode 1 with persistent_reservations=none =="

# With persistent_reservations=none and lvmlockd_global=auto, the
# first shared VG bootstraps global mode 1: it enables the GLLK and holds
# the LOCKD_GLOBAL_LOCK_1 tag.
node1 "printf 'global {\n\tpersistent_reservations = none\n}\n' >> /etc/lvm/lvm.conf"
node1 "lvmconfig --mergedconfig global/persistent_reservations | grep -q 'persistent_reservations=\"none\"'"

node1 vgcreate --shared dvg $dev3

#            mode1 mode2 lock1 lock2
check_tags dvg      1 0 1 0
check_lock_args dvg 1.1.0

retry_node1 "vgremove -y dvg"

# With the default policy (auto), the first shared VG bootstraps global
# mode 2 instead: PR is used in place of the global lock without any
# user configuration.
node1 "sed -i -e '/persistent_reservations = none/d' /etc/lvm/lvm.conf"
node1 "lvmconfig --type default global/persistent_reservations | grep -q 'persistent_reservations=\[\"auto\"\]'"

echo "== Test 2: new VGs use global mode 2 by default (persistent_reservations=auto) =="

node1 vgcreate --shared testvg $dev1
node1 vgcreate --shared testvg2 $dev2

#            mode1 mode2 lock1 lock2
check_tags testvg   0 1 0 1
check_tags testvg2  0 1 0 0
check_lock_args testvg 1.1.0
check_lock_args testvg2 1.1.0

echo "== Test 3: invalid and no-op settings =="

node1 not vgchange --locksettings global_mode=3 testvg
node1 not vgchange --locksettings foo=1 testvg

# invalid lvmlockd_global values are rejected at command start
node1 "not vgs --config 'global { lvmlockd_global = \"bogus\" }' 2>&1" > out
grep -q "Invalid global/lvmlockd_global value: bogus" out

node1 "not vgs --config 'global { lvmlockd_global = [\"global_mode_1\", \"global_mode_2\"] }' 2>&1" > out
grep -q "conflicts with an earlier global mode setting" out

noden 1 "vgchange --locksettings global_mode=2 testvg 2>&1" > out
cat out
grep -q "already using global mode 2" out
check_tags testvg   0 1 0 1

echo "== Test 4: conversion requires other hosts to stop the lockspace =="

nodep vgchange --lockstart testvg2

node1 not vgchange --locksettings global_mode=1 testvg2
check_tags testvg2  0 1 0 0

nodep vgchange --lockstop testvg2
node1 vgchange --lockstart testvg2

# the GLLK is disabled in global mode 2
check_gllk testvg2 disabled

echo "== Test 5: convert one VG to global mode 1, the VGs are mixed =="

locksettings global_mode=1 testvg2

# no other VG has lock 1 and testvg is in global mode 2, so the GLLK is
# enabled in testvg2
check_tags testvg2  1 0 1 0
check_tags testvg   0 1 0 1
check_lock_args testvg2 1.1.0

# the lockspace is stopped by the command, starting it is allowed, but
# commands needing the global lock refuse to run in the mix of global modes
node1 vgchange --lockstart testvg2
check_gllk testvg2 enabled
node1 not pvcreate -y $dev3

echo "== Test 6: convert the other VG to global mode 1 =="

locksettings global_mode=1 testvg

# testvg2 already has lock 1, so the GLLK stays disabled in testvg
check_tags testvg   1 0 0 0
check_tags testvg2  1 0 1 0

nodep vgchange --lockstart testvg testvg2
node1 vgs testvg testvg2
node2 vgs testvg testvg2
check_gllk testvg disabled
check_gllk testvg2 enabled

node1 lvcreate -y -n lv1 -L 12m testvg
node2 lvs testvg
node1 lvremove -y testvg/lv1

echo "== Test 7: lvmlockd_global=global_mode_2 conflicts with disk global mode 1 =="

# All shared VGs use global mode 1 on disk; an explicit global_mode_2
# demands global mode 2, so commands needing global protection fail on
# the conflict.
node1 "printf 'global {\n\tlvmlockd_global = [\"global_mode_2\"]\n}\n' >> /etc/lvm/lvm.conf"

node1 "not pvcreate -y $dev3 2>&1" > out
grep -q "Configured global mode 2 conflicts with disk global mode 1" out

node1 "sed -i -e '/lvmlockd_global = /d' /etc/lvm/lvm.conf"

echo "== Test 8: convert the VGs back to global mode 2 =="

nodep vgchange --lockstop testvg testvg2
node1 vgchange --lockstart testvg testvg2

locksettings global_mode=2 testvg
check_tags testvg   0 1 0 1

locksettings global_mode=2 testvg2
# testvg already holds the lock 2 tag
check_tags testvg2  0 1 0 0

nodep vgchange --lockstart testvg testvg2
node1 vgs testvg testvg2
node2 vgs testvg testvg2
check_gllk testvg disabled
check_gllk testvg2 disabled

echo "== Test 8b: persistent_reservations=none conflicts with disk global mode 2 =="

# All shared VGs use global mode 2 on disk; none demands global
# mode 1, so commands needing global protection fail on the conflict.
node1 "printf 'global {\n\tpersistent_reservations = none\n}\n' >> /etc/lvm/lvm.conf"

node1 "not pvcreate -y $dev3 2>&1" > out
grep -q "Configured global mode 1 conflicts with disk global mode 2" out

node1 "sed -i -e '/persistent_reservations = none/d' /etc/lvm/lvm.conf"

echo "== Test 9: repair the missing lock 2 tag after its VG is removed =="

nodep vgchange --lockstop testvg testvg2
node1 vgchange --lockstart testvg testvg2
retry_node1 "vgremove -y testvg"

# no VG has the lock 2 tag, assign it to testvg2
check_tags testvg2  0 1 0 0
locksettings global_mode=2 testvg2
check_tags testvg2  0 1 0 1

nodep vgchange --lockstart testvg2
node2 vgs testvg2
check_gllk testvg2 disabled

nodep vgchange --lockstop testvg2
node1 vgchange --lockstart testvg2
retry_node1 "vgremove -y testvg2"

echo "== Test 10: convert an old VG (lock_args 1.0.0, no tags) =="

# Fake a VG created by an old version of lvm: old lock_args version and no
# global mode tags on the lvmlock LV.
node1 vgcreate --shared oldvg $dev1
node1 vgchange --lockstop oldvg
noden 1 "vgcfgbackup -f /tmp/oldvg.txt oldvg"
noden 1 "sed -i -e 's/1\\.1\\.0:lvmlock/1.0.0:lvmlock/' -e '/LOCKD_GLOBAL_/d' /tmp/oldvg.txt"
noden 1 "vgcfgrestore -f /tmp/oldvg.txt oldvg"
check_lock_args oldvg 1.0.0
check_tags oldvg    0 0 0 0

# the lvmlock LV may not be active yet right after the restore
noden 1 "vgchange --lockstart oldvg" || true
sleep 2
node1 vgchange --lockstart oldvg

# an old VG has an enabled GLLK
node1 lvmlockctl --gl-enable oldvg
check_gllk oldvg enabled

locksettings global_mode=2 oldvg
check_lock_args oldvg 1.1.0
check_tags oldvg    0 1 0 1

node1 vgchange --lockstart oldvg
check_gllk oldvg disabled
retry_node1 "vgremove -y oldvg"

echo "== Test passed =="
