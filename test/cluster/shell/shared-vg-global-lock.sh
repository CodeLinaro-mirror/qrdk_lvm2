#!/bin/bash
# Test vgchange --locksettings global_lock=enable|disable
#
# The VG holding the global lock is tracked by the LOCKD_GLOBAL_LOCK_1/2
# tags on the lvmlock LV.  vgchange --locksettings global_lock changes
# the tags, and in global mode 1 it also enables/disables the GLLK lease
# on disk.
#
# - the global lock can only be enabled in one VG at a time, so enabling
#   it in a second VG fails until it is disabled in the first
# - disabling it in every VG leaves no global lock, and commands needing
#   global protection fail
# - in global mode 1 the GLLK lease is enabled/disabled on disk, and
#   moving the global lock to another VG moves the enabled GLLK with it
# - in global mode 2 only the tags change (the GLLK stays disabled)
# - the lockspace must be stopped on other hosts, and the command stops
#   the lockspace on the host running it

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

echo "== Test 1: new VGs use global mode 2, the first holds the lock 2 tag =="

node1 vgcreate --shared testvg $dev1
node1 vgcreate --shared testvg2 $dev2

#            mode1 mode2 lock1 lock2
check_tags testvg   0 1 0 1
check_tags testvg2  0 1 0 0

echo "== Test 2: invalid settings =="

node1 not vgchange --locksettings global_lock=foo testvg
node1 not vgchange --locksettings global_lock testvg
node1 not vgchange --locksettings foo=1 testvg

echo "== Test 3: enabling the global lock in a second VG fails =="

# testvg holds the lock 2 tag, so it must be disabled there first
node1 "not vgchange --locksettings global_lock=enable testvg2 2>&1" > out
grep -q "disable it there first" out
check_tags testvg2 0 1 0 0

echo "== Test 4: disabling requires the lockspace stopped on other hosts =="

nodep vgchange --lockstart testvg

node1 "not vgchange --locksettings global_lock=disable testvg 2>&1" > out
grep -q "not stopped on other hosts" out || grep -q "unknown host state" out

nodep vgchange --lockstop testvg
node1 vgchange --lockstart testvg

echo "== Test 5: disable the global lock in testvg, no VG holds it =="

locksettings global_lock=disable testvg
grep -q "No VG has the global lock enabled" out

#            mode1 mode2 lock1 lock2
check_tags testvg   0 1 0 0
check_tags testvg2  0 1 0 0

node1 vgchange --lockstart testvg

# in global mode 2 the GLLK stays disabled
check_gllk testvg disabled

echo "== Test 6: enable the global lock in testvg2 =="

locksettings global_lock=enable testvg2

#            mode1 mode2 lock1 lock2
check_tags testvg   0 1 0 0
check_tags testvg2  0 1 0 1

node1 vgchange --lockstart testvg2
check_gllk testvg2 disabled

echo "== Test 7: move the global lock back to testvg =="

locksettings global_lock=disable testvg2
check_tags testvg2 0 1 0 0

locksettings global_lock=enable testvg
check_tags testvg   0 1 0 1

node1 vgchange --lockstart testvg
check_gllk testvg disabled

nodep vgchange --lockstop testvg testvg2
node1 vgchange --lockstart testvg testvg2
retry_node1 "vgremove -y testvg"
retry_node1 "vgremove -y testvg2"

echo "== Test 8: new VGs use global mode 1 with persistent_reservations=none =="

# With persistent_reservations=none, new VGs bootstrap global mode 1: the
# first one enables the GLLK and holds the LOCKD_GLOBAL_LOCK_1 tag.
node1 "printf 'global {\n\tpersistent_reservations = none\n}\n' >> /etc/lvm/lvm.conf"
node1 "lvmconfig --mergedconfig global/persistent_reservations | grep -q 'persistent_reservations=\"none\"'"

node1 vgcreate --shared gmvg $dev1
node1 vgcreate --shared gmvg2 $dev2

#            mode1 mode2 lock1 lock2
check_tags gmvg   1 0 1 0
check_tags gmvg2  1 0 0 0

check_gllk gmvg enabled

echo "== Test 9: disable the global lock in gmvg, no VG holds it =="

# gmvg holds the lock 1 tag, so enabling in gmvg2 fails
node1 "not vgchange --locksettings global_lock=enable gmvg2 2>&1" > out
grep -q "disable it there first" out

locksettings global_lock=disable gmvg
grep -q "No VG has the global lock enabled" out

#            mode1 mode2 lock1 lock2
check_tags gmvg   1 0 0 0
check_tags gmvg2  1 0 0 0

node1 vgchange --lockstart gmvg

# the GLLK lease is disabled on disk
check_gllk gmvg disabled

# with no global lock, commands needing global protection fail
node1 not pvcreate -y $dev3

echo "== Test 10: enable the global lock in gmvg2 =="

locksettings global_lock=enable gmvg2

#            mode1 mode2 lock1 lock2
check_tags gmvg   1 0 0 0
check_tags gmvg2  1 0 1 0

node1 vgchange --lockstart gmvg2

# the GLLK lease is enabled on disk
check_gllk gmvg2 enabled

# the global lock is available again
node1 pvcreate -y $dev3
node1 pvremove -y $dev3

node1 "sed -i -e '/persistent_reservations = none/d' /etc/lvm/lvm.conf"

nodep vgchange --lockstop gmvg gmvg2
node1 vgchange --lockstart gmvg gmvg2
retry_node1 "vgremove -y gmvg"
retry_node1 "vgremove -y gmvg2"

echo "== Test passed =="
