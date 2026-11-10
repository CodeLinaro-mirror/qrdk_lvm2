#!/bin/bash
# Regression tests for global protection fixes, dev args commands
# (see GLOBAL_PROTECT_ANALYSIS_GLM53_20261109.md):
#
# H8: vgcreate of a local VG on a host using lvmlockd with no shared VGs
#     bootstraps global mode 1 and must not require a PR key (pr_key/host_id).
# M1: PR is not taken on a device arg that already failed a check.
# H1: pvchange --uuid with --select or without args is refused instead of
#     bypassing the global protection and proceeding.
# M9: pvremove only removes the PV label and leaves other signatures.
# M10: pvcreate interrupted during the wipe step does not create PVs.
#
# The H8 test requires no shared VGs in the label scan and is skipped
# when any exist (run the test from a clean/reverted cluster for it).

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

echo "== Test 1 (H8): local vgcreate with use_lvmlockd and no shared VGs =="

# A local VG on a host with use_lvmlockd=1 and no shared VGs bootstraps
# the global mode.  With no pr_key/host_id configured, the old code
# selected global mode 2 and failed to derive a global PR key; the fix
# selects global mode 1 (no global lock exists to take).

if node1 "vgs --noheadings -o lock_type 2>/dev/null" | grep -qE "sanlock|dlm|idm"; then
    echo "SKIP: shared VGs exist, H8 requires a clean cluster"
else
    node1 "sed -i -e '/pr_key/d' -e '/host_id/d' /etc/lvm/lvmlocal.conf"
    node1 vgcreate localvg $dev2
    node1 "vgs --noheadings -o vg_name localvg" | grep -q localvg
    node1 vgremove -y localvg
    # restore the host_id that the node was created with
    node1 "sed -i -e '/pr_key/d' -e '/local {/a\\    host_id = 1' /etc/lvm/lvmlocal.conf"
fi

echo "== Test 2: shared VG with PR for the dev args tests =="

node1 "sed -i -e '/pr_key/d' -e '/local {/a\\    host_id = 1' /etc/lvm/lvmlocal.conf"
node2 "sed -i -e '/pr_key/d' -e '/local {/a\\    host_id = 2' /etc/lvm/lvmlocal.conf"

# The dev args tests use PR for global protection (global mode 2); the
# default policy (persistent_reservations=auto) bootstraps global mode 2
# for the first shared VG in a new environment.
node1 "lvmconfig --type default global/persistent_reservations | grep -q 'persistent_reservations=\[\"auto\"\]'"

node1 vgcreate --shared --setpersist y testvg $dev1 $dev2
node1 vgchange --lockstart --persist start testvg

echo "== Test 3 (M1): no PR on a dev arg that already failed a check =="

# $dev2 is in use by testvg so its usage check fails before the lock is
# taken; $dev3 is blank.  The command fails for $dev2, but $dev3 must
# still be created: the global PR is taken only on devs that have not
# failed.  (Before the fix, the lock list included $dev2, whose
# registered VG keys made the PR start fail for all the devs, and $dev3
# was not created.)

node1 not pvcreate -y $dev2 $dev3
node1 "pvs --noheadings -o pv_name $dev3" | grep -q "$dev3"

# the global PR key is not left registered
node2 "lvmpersist read-keys --device $dev3 | grep -q 'keys: none'"

echo "== Test 4 (H1): pvchange --uuid --select or no args is refused =="

# With global protection in use, pvchange --uuid of orphan PVs requires
# explicit device args.  The old code returned an error code from
# _pvchange_lock_global that the caller treated as success, so the
# command proceeded with no protection and changed the orphan PV uuids.

uuid_before=$(node1 "pvs --noheadings -o uuid $dev3" 2>/dev/null | tr -d ' ')

node1 not pvchange --uuid
node1 "not pvchange --uuid --select 'pv_name=~sd'"

uuid_after=$(node1 "pvs --noheadings -o uuid $dev3" 2>/dev/null | tr -d ' ')

if [ "$uuid_before" != "$uuid_after" ]; then
    echo "ERROR: pvchange --uuid changed the orphan PV uuid without protection"
    exit 1
fi

echo "== Test 5 (M9): pvremove removes only the PV label =="

# pvremove must not wipe or prompt about other signatures found on the
# dev (the old shared wipe step also wiped them).  Write an iso9660
# signature beyond the LVM label area; it must survive pvremove.

node1 "printf '\\x01CD001' | dd of=$dev3 bs=1 seek=32768 conv=notrunc"
node1 "dd if=$dev3 bs=1 skip=32768 count=6" | grep -q "CD001"

node1 pvremove -y $dev3

node1 "blkid -p $dev3" | grep -q iso9660
node1 "dd if=/dev/zero of=$dev3 bs=1M count=2"

echo "== Test 6 (M10): pvcreate interrupted during the wipe =="

# Send SIGINT to a pvcreate of the free devs.  If the interrupt lands
# during the wipe step, no PVs may be created (the old code created PVs
# for the devs that were already wiped).  The command is fast, so the
# interrupt may arrive after it completes; that case is inconclusive
# and the test passes with a notice.

node1 "
interrupted=0
completed=0
for delay in 0.3 0.2 0.1 0.05 0.02; do
    for d in $dev3 $dev4; do dd if=/dev/zero of=\$d bs=1M count=2 2>/dev/null; done
    pvcreate -y $dev3 $dev4 >/tmp/m10.out 2>&1 &
    pid=\$!
    sleep \$delay
    if kill -INT \$pid 2>/dev/null; then
        wait \$pid
        if grep -q 'Interrupted before writing new PVs' /tmp/m10.out; then
            interrupted=1
            break
        fi
    else
        wait \$pid
        completed=1
    fi
done
if [ \$interrupted = 1 ]; then
    echo 'interrupted during wipe'
    if pvs --noheadings -o pv_name 2>/dev/null | grep -qE '$dev3|$dev4'; then
        echo 'ERROR: PVs were created after the interrupt'
        exit 1
    fi
elif [ \$completed = 1 ]; then
    echo 'NOTICE: pvcreate completed before the interrupt; M10 inconclusive'
else
    echo 'NOTICE: interrupt landed outside the wipe step; M10 inconclusive'
fi
for d in $dev3 $dev4; do pvremove -y \$d >/dev/null 2>&1 || true; done
exit 0
"

echo "== Test 7: cleanup =="

node1 vgremove -y testvg
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev2 bs=1M count=2"

echo "== Test passed =="
