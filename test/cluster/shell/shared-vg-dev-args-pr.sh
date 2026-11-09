#!/bin/bash
# Exercise the global protection of dev args (pvcreate/pvremove/vgcreate)
# in lvmlockd global mode 2, where PR with the global key is used to
# protect devices that are not owned by a VG.
#
# - pvcreate/pvremove take and release the global PR on a blank dev
# - a global PR key (generation 0) registered on a dev by another host
#   prevents a dev arg from being used (the command must fail)
# - pvresize/pvchange on an orphan PV use the same protection
#

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

nodes 'sed -i -e "/pr_key/d" -e "/host_id/d" -e "/local {/a\\    host_id = ${NODE_NUM}" /etc/lvm/lvmlocal.conf'

# The test runs in global mode 2, where PR with the global key protects
# devices that are not owned by a VG; the default policy
# (persistent_reservations=auto) bootstraps global mode 2 for the first
# shared VG in a new environment.
node1 "lvmconfig --type default global/persistent_reservations | grep -q 'persistent_reservations=\[\"auto\"\]'"

# A global PR key has generation 0: 0x100000000000XXXX (XXXX=host_id).
# Use a foreign host_id (9) that no node uses.
FOREIGN_KEY=0x1000000000000009

#
# establish global mode 2 with a shared VG using PR
#
node1 vgcreate --shared --setpersist y testvg $dev1
nodep vgchange --lockstart --persist start testvg

#
# pvcreate on a blank dev takes and releases the global PR
#
node2 lvmpersist read-keys --device $dev2 | grep -q "keys: none"

node1 pvcreate -y $dev2
node1 pvs $dev2

# the global PR key is not left registered
node2 lvmpersist read-keys --device $dev2 | grep -q "keys: none"

#
# pvresize on the orphan PV uses the same protection
#
node1 pvresize -y --setphysicalvolumesize 500m $dev2
node1 pvs $dev2 --noheadings -o pv_size | grep -q "500"

# pvchange on the orphan PV uses the same protection
node1 pvchange -u $dev2

#
# pvremove on the orphan PV takes and releases the global PR
#
node1 pvremove -y $dev2
node1 not pvs $dev2
node2 lvmpersist read-keys --device $dev2 | grep -q "keys: none"

#
# a global key registered on a dev by another host prevents using the dev
#
node2 lvmpersist start --ourkey $FOREIGN_KEY --prtype WE --device $dev2
node2 lvmpersist check-key --key $FOREIGN_KEY --device $dev2

# pvcreate must fail, not use the dev
node1 not pvcreate -y $dev2
node1 not pvs $dev2

# vgcreate must fail, not use the dev
node1 not vgcreate --shared testvg2 $dev2
node1 not vgs testvg2

# vgextend of the existing VG must fail, not use the dev
node1 not vgextend testvg $dev2
node1 vgs testvg --noheadings -o pv_count | grep -q 1

node2 lvmpersist stop --ourkey $FOREIGN_KEY --device $dev2

# with the foreign key removed, pvcreate succeeds
node1 pvcreate -y $dev2
node1 pvs $dev2
node1 pvremove -y $dev2

#
# vgcreate --shared using existing orphan PVs (not blank devs)
#
node1 pvcreate -y $dev2
node1 vgcreate --shared testvg2 $dev2
node1 vgremove -y -f testvg2

#
# cleanup: lockstop the other nodes, then vgremove (which stops the
# local lockspace and PR); node1's lockspace must stay running for the
# global lock used by vgremove in global mode 2.
#
for node_num in $(seq 2 $CLUSTER_NUM_NODES); do
    noden ${node_num} vgchange --lockstop --persist stop testvg || true
done
sleep 1
while ! node1 vgremove -y -f testvg; do
    sleep 1
done

echo "DONE"
