#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Do not take a btrfs copy for the mounted filesystem'

. lib/inittest --skip-with-lvmpolld

aux have_fsinfo || skip "Test requires --fs checksize support"
aux have_fs btrfs || skip

aux prepare_vg 1 512

mount_dir="mnt_uuid_copy"
mkdir -p "$mount_dir"

cleanup_btrfs_uuid()
{
	umount "$mount_dir" || true
	lvchange -an "$vg" || true
	vgremove -ff "$vg" || true
	aux teardown
}
trap 'cleanup_btrfs_uuid' EXIT

lvcreate -L 128M -n $lv1 $vg
lvcreate -L 128M -n $lv2 $vg

mkfs.btrfs -f "$DM_DEV_DIR/$vg/$lv1" >/dev/null
mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

# Copy the superblock, so lv2 carries the uuid of the mounted filesystem
# without being one of its devices: this is what a copy, a snapshot or a
# crafted superblock looks like to the fs detection.
dd if="$DM_DEV_DIR/$vg/$lv1" of="$DM_DEV_DIR/$vg/$lv2" bs=64k count=2 conv=notrunc oflag=direct

# Both devices now start with the same superblock, including the fs uuid.
dd if="$DM_DEV_DIR/$vg/$lv1" of=sb1 bs=64k count=2 2>/dev/null
dd if="$DM_DEV_DIR/$vg/$lv2" of=sb2 bs=64k count=2 2>/dev/null
cmp sb1 sb2

# lv2 shares the uuid, but is not a device of the mounted filesystem, so
# the filesystem found on it must not be reported as mounted at the mount
# point of the other one.
not lvresize -y --fs checksize -L96M $vg/$lv2 >out 2>&1
grep -F "File system btrfs found on" out
grep -F "File system reduce is required (see btrfs-progs or --resizefs.)" out
not grep -F "mounted at" out

# The mounted filesystem is untouched.
mountpoint -q "$mount_dir"
