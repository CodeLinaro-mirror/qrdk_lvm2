#!/bin/bash
# Regression test for the global protection fixes on a local VG
# (see GLOBAL_PROTECT_ANALYSIS_GLM53_20261109.md):
#
# M5: the PR key registered by persist_vgextend_begin on the new devs is
#     released by unlock_global_devs when the command fails after the
#     begin (before the fix the key and reservation remained registered,
#     blocking use of the devs until removed by hand).

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

DEV_TYPES=""
[ "${CLUSTER_NUM_SCSI:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES scsi"
[ "${CLUSTER_NUM_NVME:-0}" -ge 2 ]     && DEV_TYPES="$DEV_TYPES nvme"
[ "${CLUSTER_NUM_MULTIPATH:-0}" -ge 2 ] && DEV_TYPES="$DEV_TYPES mpath"

if [ -z "$DEV_TYPES" ]; then
    echo "SKIP: no device type with >= 2 devices"
    exit 0
fi

for devtype in $DEV_TYPES; do
    case $devtype in
        scsi)  d1=$scsi1; d2=$scsi2 ;;
        nvme)  d1=$nvme1; d2=$nvme2 ;;
        mpath) d1=$mpath1; d2=$mpath2 ;;
    esac

    echo "=== Testing device type: $devtype ($d1, $d2) ==="

    node1 "sed -i -e '/pr_key/d' -e '/local {/a\\    host_id = 1' /etc/lvm/lvmlocal.conf"

    node1 vgcreate --setpersist y testvg $d1
    node1 vgchange --persist start testvg

    # Make the command fail after persist_vgextend_begin has registered
    # the VG key on the new dev: with the new dev's metadata area
    # ignored, vg_write must write the metadata to the existing PV,
    # which fails on the read-only one.
    node1 blockdev --setro $d1
    node1 not vgextend --metadataignore y testvg $d2
    node1 blockdev --setrw $d1

    # the key started on the new dev by the begin must have been
    # released on the command's exit path
    node1 "lvmpersist read-keys --device $d2" > out
    cat out
    grep -q "keys: none" out

    # the new PV was written before vg_write failed; the failed vgextend
    # left its PV header without VG metadata, so removing it needs force
    node1 pvremove -y -ff $d2

    # the VG is unchanged and still usable
    node1 vgchange --persist check testvg
    node1 vgremove -y testvg
done

echo "== Test passed =="
