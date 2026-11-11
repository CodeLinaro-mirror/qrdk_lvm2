#!/bin/bash
# T6: persistent_reservations=full without lvmlockd
# (see PR_POLICY_REDESIGN_20261112.md, formerly global_devices_pr)
#
# full protects unowned devices (pvcreate, pvremove, vgcreate,
# vgextend, pvchange --uuid, pvresize of orphan PVs) with PR on hosts that
# do not use lvmlockd:
#
# - pvcreate/pvremove on PR-capable devs take and release the global PR
#   (the generation 0 key derived from host_id)
# - a PR key registered on a dev by another host prevents the dev from
#   being used: the command fails before writing anything, so no PV is
#   created on any of the command's devs
# - a dev that does not support PR (here a loop device) is used without
#   protection between hosts, and the command succeeds
# - a command using both PR-capable and non-PR devs fails: when PR is
#   used for global devices, all the devs must support it
# - vgcreate --setpersist with full: the global PR taken on
#   the devs becomes the local VG's PR (the documented use of
#   full together with local VGs)
#
# The devices are shared between the nodes (exported by node0), so node2
# can register a foreign PR key on a dev that node1 uses.

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

# The executor regenerates /etc/lvm/lvm.conf at test setup containing only
# the non-default settings (local/log sections), so append the test settings.
node1 "printf 'global {\n\tuse_lvmlockd = 0\n\tpersistent_reservations = full\n}\ndevices {\n\tuse_devicesfile = 0\n}\n' >> /etc/lvm/lvm.conf"
node1 "lvmconfig --mergedconfig global/persistent_reservations | grep -q 'persistent_reservations=\"full\"'"

# node1's global PR key is derived from its host_id; set it explicitly
# (a fresh cluster may not have one configured).  The executor flattens
# lvmlocal.conf into lvm.conf at test setup, so also remove any stale
# keys there: they could override or conflict with the host_id below.
node1 "sed -i -e '/pr_key/d' -e '/host_id/d' /etc/lvm/lvm.conf"
node1 'sed -i -e "/pr_key/d" -e "/host_id/d" -e "/local {/a\\    host_id = 1" /etc/lvm/lvmlocal.conf'
HOSTID=$(node1 "lvmconfig --mergedconfig local/host_id" | sed 's/^host_id=//')
if [ -z "$HOSTID" ]; then
    echo "ERROR: failed to set host_id on node1"
    exit 1
fi
GLOBAL_KEY=$(printf '0x100000000000%04x' "$HOSTID")

# a foreign PR key from node2 (a generation 0 key that no node derives)
FOREIGN_KEY=0x1000000000000009

# clear leftover labels
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev2 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev3 bs=1M count=2"

echo "== Test 1: pvcreate takes and releases the global PR =="

node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"
node1 "lvmpersist read-keys --device $dev2 | grep -q 'keys: none'"

node1 "pvcreate -y $dev1 $dev2 2>&1" > out
grep -q "started.*with key $GLOBAL_KEY" out
grep -q "stopped.*with key $GLOBAL_KEY" out
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"
node1 "lvmpersist read-keys --device $dev2 | grep -q 'keys: none'"
node1 pvs $dev1 $dev2

echo "== Test 2: pvremove takes and releases the global PR =="

node1 "pvremove -y $dev1 $dev2 2>&1" > out
grep -q "started.*with key $GLOBAL_KEY" out
grep -q "stopped.*with key $GLOBAL_KEY" out
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"
node1 not pvs $dev1

echo "== Test 3: a foreign PR key prevents using the dev =="

node2 "lvmpersist start --ourkey $FOREIGN_KEY --prtype WE --device $dev3"
node2 "lvmpersist check-key --key $FOREIGN_KEY --device $dev3"

# the command fails before writing: no PV is created on any of the devs
node1 "not pvcreate -y $dev1 $dev3 2>&1" > out
grep -q "PR keys are registered" out
node1 not pvs $dev1
node1 not pvs $dev3
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"

node2 "lvmpersist stop --ourkey $FOREIGN_KEY --device $dev3"
node1 "lvmpersist read-keys --device $dev3 | grep -q 'keys: none'"

echo "== Test 4: a non-PR dev is used without PR =="

# clean up any loop device left over from a previous (failed) run
node1 "losetup -j /tmp/t6-nonpr.img -O NAME | xargs -r losetup -d; rm -f /tmp/t6-nonpr.img"

# a loop device does not support PR
node1 "truncate -s 128m /tmp/t6-nonpr.img"
LOOP=$(node1 "losetup -f --show /tmp/t6-nonpr.img" | tail -1)
node1 udevadm settle

node1 "pvcreate -v -y $LOOP 2>&1" > out
grep -q "Devices do not support PR; they are not protected between hosts" out
node1 pvs $LOOP

echo "== Test 5: mixed PR support is an error =="

# when PR is used for global devices, all the devs must support it
node1 "not pvcreate -y $dev1 $LOOP 2>&1" > out
grep -q "does not support persistent reservation" out
node1 not pvs $dev1
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"

node1 pvremove -y $LOOP
node1 "losetup -d $LOOP"
node1 "rm -f /tmp/t6-nonpr.img"

echo "== Test 6: vgcreate --setpersist --persist start with full =="

# The documented use with local VGs: lock_global_devs takes the global PR
# on the devs, and persist_vgcreate_update transfers it to the VG (for a
# local VG the VG key is the same generation 0 key, registered with
# exclusive access.)  --persist start is required for the transfer: with
# --setpersist y alone the global PR is simply released on unlock.
node1 "vgcreate --setpersist y --persist start testvg $dev1 $dev2 2>&1" > out
grep -q "started.*with key $GLOBAL_KEY" out

node1 "lvmpersist check-key --key $GLOBAL_KEY --device $dev1"
node1 "lvmpersist check-key --key $GLOBAL_KEY --device $dev2"
node1 "lvmpersist read-reservation --device $dev1 | grep -q WE"

node1 "vgremove -y testvg 2>&1" > out
grep -q "stopped.*with key $GLOBAL_KEY" out
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"

node1 pvremove -y $dev1 $dev2
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev2 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev3 bs=1M count=2"

echo "== Test passed =="
