#!/bin/bash
# Regression tests for global protection fixes, sanlock leases and the
# lvmlockd global lock (see GLOBAL_PROTECT_ANALYSIS_GLM53_20261109.md):
#
# H2: vgrename of a VG created before the global mode feature
#     (lock_args 1.0.0, no GL2 lease ever written) succeeds.
# H3: vgchange --setlockargs on such a VG creates the GL2 lease when it
#     changes the lock_args version to minor 1.
# H6: the exclusive PR taken by vgchange --setlockargs is released when
#     the command fails after acquiring it.
# H7: the gl lock is adopted after an lvmlockd restart using the global
#     mode saved in the adopt file.

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

LOCKD_SERVICE="lvmlockd"
SYSCONFIG="/etc/sysconfig/lvmlockd"
ADOPT_FILE="/run/lvm/lvmlockd.adopt"

# Other hosts that just stopped the lockspace may still have an unknown
# host state, so retry for a while.  A lockspace start may also fail
# shortly after the lease areas were modified outside lvmlockd.
retry_node1() {
    local i
    for i in $(seq 1 24); do
        if noden 1 "$1 2>&1" > out; then
            cat out
            return 0
        fi
        cat out
        grep -qE "unknown host state|error -214|Lock start failed" out || return 1
        sleep 5
    done
    return 1
}

# check_gl2 <vg>: the GL2 lease exists at offset 64 (64MiB with 1MiB align)
check_gl2() {
    noden 1 "sanlock direct dump /dev/mapper/$1-lvmlock:67108864:1048576" > out
    cat out
    grep -q GL2LK out
}

# These tests use global mode 2 (PR instead of the global lock for global
# state, and the GL2LK); the default policy (persistent_reservations=auto)
# bootstraps global mode 2 for the first shared VG in a new environment.
# (The nodes have a host_id from the cluster setup, required by GM2.)
node1 "lvmconfig --type default global/persistent_reservations | grep -q 'persistent_reservations=\[\"auto\"\]'"

echo "== Test 1 (H2): vgrename of an old VG with no GL2 lease =="

# Fake a VG created by an old version of lvm: old lock_args version, no
# global mode tags, and the GL2 lease area (offset align_size * 64)
# never written.  (oflag=direct so the zeroed area is immediately
# visible to sanlock's direct reads.)
node1 vgcreate --shared oldvg $dev1
node1 vgchange --lockstop oldvg
noden 1 "vgcfgbackup -f /tmp/oldvg.txt oldvg" > out
noden 1 "sed -i -e 's/1\\.1\\.0:lvmlock/1.0.0:lvmlock/' -e '/LOCKD_GLOBAL_/d' /tmp/oldvg.txt" > out
node1 vgcfgrestore -f /tmp/oldvg.txt oldvg
node1 "vgs --noheadings -o lock_args oldvg" | grep -q "1.0.0"
# vgcfgrestore replays udev events for the LVs; wait for the device
# nodes to be relabeled before sanlock opens them
node1 udevadm settle

node1 "dd if=/dev/zero of=/dev/mapper/oldvg-lvmlock bs=1M seek=64 count=1 conv=notrunc oflag=direct"
sleep 2
node1 vgchange --lockstart oldvg || true
sleep 5
retry_node1 "vgchange --lockstart oldvg"

# vgrename must succeed.  (Before the fix it unconditionally read the
# GL2 lease, which fails on the never-written area after the lockspace
# has already been renamed, leaving the VG's leases inconsistent with
# the lockspace name.)
retry_node1 "vgrename oldvg oldvg2"
node1 "vgs --noheadings -o vg_name oldvg2" | grep -q oldvg2

# the VG lease was renamed with the lockspace
node1 "sanlock direct dump /dev/mapper/oldvg2-lvmlock:69206016:1048576" > out
cat out
grep -q "lvm_oldvg2.*VGLK" out

echo "== Test 2 (H3): setlockargs on the old VG creates the GL2 lease =="

node1 vgchange --lockstop oldvg2 || true
sleep 2
retry_node1 "vgchange --lockstart oldvg2"

retry_node1 "vgchange --setlockargs notimeout oldvg2"
node1 "vgs --noheadings -o lock_args oldvg2" | grep -q "2.1.0"

node1 vgchange --lockstart oldvg2 || true
sleep 3
node1 vgchange --lockstart oldvg2
check_gl2 oldvg2

# the GL2 lease allows converting the old VG to global mode 2
retry_node1 "vgchange --locksettings global_mode=2 oldvg2"
node1 "vgs --noheadings -o lock_args oldvg2" | grep -q "2.1.0"

node1 vgchange --lockstart oldvg2 || true
sleep 3
retry_node1 "vgchange --lockstart oldvg2"
retry_node1 "vgremove -y oldvg2"
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"

echo "== Test 3 (H6): the ex PR is released when setlockargs fails =="

# The VG uses PR, so vgchange --setlockargs takes an exclusive PR
# (persist_upgrade_ex).  Make the command fail after that by setting
# one of the VG's PVs read-only (the lease rewrite or the metadata
# write fails).  The ex PR must be released on exit; before the fix it
# remained registered and blocked all hosts from using the VG.

node1 "sed -i -e '/pr_key/d' -e '/local {/a\\    host_id = 1' /etc/lvm/lvmlocal.conf"
node2 "sed -i -e '/pr_key/d' -e '/local {/a\\    host_id = 2' /etc/lvm/lvmlocal.conf"

node1 vgcreate --shared --setpersist y testvg $dev1 $dev2
node1 vgchange --lockstart --persist start testvg

node1 blockdev --setro $dev2
node1 not vgchange --setlockargs notimeout testvg
node1 blockdev --setrw $dev2

# the ex PR key (generation 0) must not remain registered on the VG devs
node2 "lvmpersist read-keys --device $dev1" > out
cat out
not grep -q "0x100000000000" out
node2 "lvmpersist read-keys --device $dev2" > out
cat out
not grep -q "0x100000000000" out

# rerunning the command succeeds and repairs the VG (the PR was stopped
# by the failed command, so it must be started again first)
retry_node1 "vgchange --lockstart --persist start testvg"
retry_node1 "vgchange --setlockargs notimeout testvg"
node1 "vgs --noheadings -o lock_args testvg" | grep -q "2.1.0"

retry_node1 "vgchange --lockstart --persist start testvg"
retry_node1 "vgremove -y testvg"
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev2 bs=1M count=2"

echo "== Test 4 (H7): the gl lock is adopted after an lvmlockd restart =="

# The global mode is saved in the adopt file and restored on restart,
# so the gl lock is adopted (and gl-using commands work) without a
# lockstart providing the mode.

# lvmlockd only writes the adopt file when running with -A 1
nodes cp $SYSCONFIG ${SYSCONFIG}.orig || true
node1 "echo 'OPTIONS=\"-A 1\"' > $SYSCONFIG"
node1 systemctl restart $LOCKD_SERVICE
sleep 2

node1 vgcreate --shared testvg $dev1
node1 vgchange --lockstart testvg
node1 lvcreate -y -l4 -n lv1 testvg

# the adopt file contains the global state
node1 "grep -q '^GLOBAL: ' $ADOPT_FILE"

node1 killall -9 lvmlockd
sleep 1
node1 systemctl reset-failed $LOCKD_SERVICE || true

nodes cp $SYSCONFIG ${SYSCONFIG}.orig || true
node1 "echo 'OPTIONS=\"-A 1\"' > $SYSCONFIG"
node1 systemctl start $LOCKD_SERVICE
sleep 2

# the gl lock was adopted, not skipped for an unknown global mode
node1 "lvmlockctl --dump" > out
grep -q "adopt lock for gl$" out

# the LV is still active and usable
node1 "dmsetup status testvg-lv1" > out

node1 lvremove -y testvg/lv1

# vgremove uses the gl lock in global mode 2 and works without a
# lockstart after the restart
node1 vgremove -y testvg

node1 cp ${SYSCONFIG}.orig $SYSCONFIG || true
node1 systemctl restart $LOCKD_SERVICE
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"

echo "== Test passed =="
