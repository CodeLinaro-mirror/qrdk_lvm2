#!/bin/bash
# Exercise the PR claim protocol that protects devices specified as dev
# args (vgcreate/pvcreate/vgextend) while the devices are unused.
#
# The claim (lvmpersist start --claim) registers the local key, verifies
# that no key from another host is registered, and then reserves.  Mutual
# exclusion between hosts comes from the register-then-verify-alone
# check; the reservation type (WE, or WEAR on dm-multipath) fences
# writers that are not registered.
#
# - a claim fails with exit status 2 when another host's key is
#   registered, and the local key is unregistered (rolled back)
# - a key left registered by a host that failed while claiming blocks
#   vgcreate from using the devices; recovery preempts the stale key
#   with lvmpersist start --removekey
# - two hosts racing vgcreate on the same unused devices: at most one
#   succeeds, the loser fails cleanly without writing, and the VG is
#   consistent on all nodes
#

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

nodes 'sed -i -e "/pr_key/d" -e "/host_id/d" -e "/local {/a\\    host_id = ${NODE_NUM}" /etc/lvm/lvmlocal.conf'

# PR keys derived from host_id (see lvmpersist.8): node1 uses KEY1,
# node2 uses KEY2.
KEY1=0x1000000000000001
KEY2=0x1000000000000002

#
# A claim fails when another host's key is registered: exit status 2,
# and the local key is rolled back.
#
node2 lvmpersist start --ourkey $KEY2 --access ex --device $dev1 --device $dev2
node2 lvmpersist check-key --key $KEY2 --device $dev1

node1 "lvmpersist start --ourkey $KEY1 --claim --access ex --device $dev1 --device $dev2; test \$? -eq 2"

# the failed claim must not leave the local key registered
node1 not lvmpersist check-key --key $KEY1 --device $dev1
node1 lvmpersist check-key --key $KEY2 --device $dev1

#
# A key left registered by a host that failed while claiming blocks
# vgcreate from using the devices.  node2 does not stop its key, as a
# failed host would not.
#
node1 "vgcreate --shared testvg $dev1 $dev2 2>&1 | grep -q 'are registered'"
node1 not vgs testvg

# the failed vgcreate must not leave the local key registered
node1 not lvmpersist check-key --key $KEY1 --device $dev1

#
# Recovery: preempt the stale key with start --removekey, then stop our
# own key.  With the devices unclaimed, vgcreate succeeds.
#
node1 lvmpersist start --ourkey $KEY1 --removekey $KEY2 --access ex --device $dev1 --device $dev2
node1 not lvmpersist check-key --key $KEY2 --device $dev1
node1 lvmpersist stop --ourkey $KEY1 --device $dev1 --device $dev2
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"

node1 vgcreate --shared testvg $dev1 $dev2
nodep vgchange --lockstart testvg
nodep "vgs --noheadings -o pv_count testvg | grep -q 2"
cleanup_vg testvg

#
# A staggered race: node2 starts shortly after node1, which is between
# claiming and releasing the devices; exactly one host wins.  Depending
# on timing, the loser fails the claim (another host's key is
# registered) or the VG name check, in either case without writing.
#
nodepp 1:"vgcreate --shared testvg $dev1 $dev2" 2:"sleep 1; vgcreate --shared testvg $dev1 $dev2" || true
assert_one_success

# the winner released its claim when vgcreate completed, and the loser
# rolled back: no keys may remain on the devices
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"
node1 "lvmpersist read-keys --device $dev2 | grep -q 'keys: none'"

# the VG is visible and consistent on all nodes
nodep vgchange --lockstart testvg
nodep "vgs --noheadings -o pv_count testvg | grep -q 2"

cleanup_vg testvg
nodep 'rm -f /etc/lvm/archive/testvg*'

#
# Two hosts race vgcreate on the same unused devices.  The claim permits
# at most one winner.  The loser fails the claim without writing, or
# both hosts back off after seeing each other's key (in which case a
# retry must succeed).
#
for i in $(seq 1 5); do
    nodep vgcreate --shared testvg $dev1 $dev2 || true

    if [ "$NODEP_SUCCESS_COUNT" -gt 1 ]; then
        echo "FAIL: iteration $i: nodes $NODEP_SUCCESS_NODES all claimed the same devices"
        exit 1
    fi

    if [ "$NODEP_SUCCESS_COUNT" -eq 0 ]; then
        # all hosts backed off seeing each other's keys; the devices
        # must be clean, and a retry must succeed
        echo "iteration $i: all hosts backed off, retrying"
        node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"
        node1 not vgs testvg
        node1 vgcreate --shared testvg $dev1 $dev2
    else
        echo "iteration $i: node $NODEP_SINGLE_SUCCESS_NODE won the claim"
    fi

    # the winner released its claim when vgcreate completed, and the
    # loser rolled back: no keys may remain on the devices
    node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"
    node1 "lvmpersist read-keys --device $dev2 | grep -q 'keys: none'"

    # the VG is visible and consistent on all nodes
    nodep vgchange --lockstart testvg
    nodep "vgs --noheadings -o pv_count testvg | grep -q 2"

    # cleanup for the next iteration
    cleanup_vg testvg
    nodep 'rm -f /etc/lvm/archive/testvg*'
done

echo "DONE"
