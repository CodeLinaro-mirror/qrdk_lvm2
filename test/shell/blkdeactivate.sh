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

test_description='Exercise blkdeactivate'

. lib/inittest

# The script only runs root-owned tools from a root-owned path, so the
# test needs the staged copy made by lib/inittest when run as root.
test -x "${LVM_TEST_BINDIR-}/blkdeactivate" || skip
aux have_fs ext4 || skip

# The blank before the digit makes lsblk write the path as
# "mnt\x20space\x201" and the mount table as "mnt\040space\0401", so this
# covers the \xHH decoder; test/shell/fsadm.sh and
# test/shell/lvresize-fs-ns.sh use the same shape of name for the octal
# decoder the sibling scripts carry.
mount_dir="mnt space 1"
mount_dir_esc="mnt\\040space\\0401"
mkdir -p "$mount_dir"

# A failure can leave the filesystem, or the tmpfs covering it, mounted.
# The suite teardown removes the devices but does not unmount, and the
# path is inside $TESTDIR, which cannot be removed while it is busy.
cleanup_mounted_and_teardown()
{
	umount "$mount_dir" 2>/dev/null || true
	umount "$mount_dir/sub" 2>/dev/null || true
	aux teardown
}
trap 'cleanup_mounted_and_teardown' EXIT

#################################################################
# A device directory another user could write to is refused: the
# nodes blkdeactivate would use could be planted there.  The control
# node (10:236 is the device-mapper one) is what makes the script
# check the directory instead of falling back to the default.
#################################################################
mkdir -p "$TESTDIR/baddevdir/mapper"
mknod "$TESTDIR/baddevdir/mapper/control" c 10 236
chmod 0777 "$TESTDIR/baddevdir"
DM_DEV_DIR="$TESTDIR/baddevdir" not blkdeactivate >out 2>err
grep "DM_DEV_DIR .*must not be group or world writable" err

aux prepare_devs 3 100

#################################################################
# A mounted filesystem is unmounted and its LV deactivated.
#################################################################
vgcreate "$vg" "$dev1"
lvcreate -L 20M -n "$lv1" "$vg"
lvcreate -L 20M -n "$lv2" "$vg"

lvchange -ay "$vg/$lv1"
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"

blkdeactivate -u "$DM_DEV_DIR/$vg/$lv1" >out 2>err
grep "unmounting $vg-$lv1" out
grep "deactivating Logical Volume $vg/$lv1.*done" out
check lv_field "$vg/$lv1" lv_active ""
not grep -F "$TESTDIR/$mount_dir_esc " /proc/self/mountinfo

#################################################################
# A mounted device is not deactivated unless -u was given.
#################################################################
lvchange -ay "$vg/$lv1"
mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
blkdeactivate "$DM_DEV_DIR/$vg/$lv1" >out 2>err
grep "\[SKIP\]: unmount of" out
check lv_field "$vg/$lv1" lv_active "active"
umount "$mount_dir"

#################################################################
# A failed umount does not lead to a forced removal: the device is
# still mounted, so blkdeactivate has to report the skip.
#################################################################
dmfs="${PREFIX}dmfs"
src_devno=$(dmsetup info -c --noheadings -o major,minor "${dev3##*/}" | tr -s ' ' ':')
dmsetup create "$dmfs" --table "0 20480 linear $src_devno 0"
mkfs.ext4 "$DM_DEV_DIR/mapper/$dmfs"

mkdir -p "$mount_dir/sub"
mount "$DM_DEV_DIR/mapper/$dmfs" "$mount_dir/sub"
# Cover the mount point: the path no longer names the mount, so an umount
# of it fails while the filesystem stays mounted on the device.
mount -t tmpfs tmpfs "$mount_dir"
not blkdeactivate -u -d force "$DM_DEV_DIR/mapper/$dmfs" >out 2>err
grep "skipping" out
# The mounted device is not handed to the deactivation at all.
not grep "deactivating" out
# The device was not replaced by an error target under the mounted fs.
dmsetup info "$dmfs" >/dev/null
grep -F "$TESTDIR/$mount_dir_esc/sub " /proc/self/mountinfo

# The same with --one-by-one, the mode picked automatically when umount
# has no --all-targets option: the mount point is tried on its own and
# the check goes through findmnt instead of lsblk.  Without findmnt
# blkdeactivate has no one-by-one mode, so only this run is left out.
if test -x /bin/findmnt; then
	not blkdeactivate -1 -u -d force "$DM_DEV_DIR/mapper/$dmfs" >out 2>err
	grep "skipping" out
	not grep "deactivating" out
	dmsetup info "$dmfs" >/dev/null
	grep -F "$TESTDIR/$mount_dir_esc/sub " /proc/self/mountinfo
fi

umount "$mount_dir"
umount "$mount_dir/sub"
rmdir "$mount_dir/sub"
dmsetup remove "$dmfs"

#################################################################
# Deactivating a snapshot asks for confirmation, and the answer
# must not be read from stdin, which the caller may fill with the
# rest of the device list.
#################################################################
lvcreate -L 20M -n "$lv3" "$vg"
lvcreate -L 20M -n "$lv4" -s "$vg/$lv3"
vgchange -ay "$vg"
check lv_field "$vg/$lv4" lv_active "active"

# 'y' on stdin stands for the device list the prompt used to consume.
printf 'y\n' | not blkdeactivate "$DM_DEV_DIR/$vg/$lv4" "$DM_DEV_DIR/$vg/$lv2" >out 2>err
grep "skipping" out
check lv_field "$vg/$lv4" lv_active "active"
check lv_field "$vg/$lv3" lv_active "active"
# The second device of the list was still processed.
check lv_field "$vg/$lv2" lv_active ""

vgremove -ff "$vg"

#################################################################
# An LV name is not a path into the device directory: a foreign
# entry under the VG's directory that has the name of an inactive
# LV must not be taken for that LV's device.
#################################################################
victim="${PREFIX}victim"
dmsetup create "$victim" --table "0 2048 zero"
victim_devno=$(dmsetup info -c --noheadings -o major,minor "$victim" | tr -s ' ' ':')
dmsetup create "$victim-holder" --table "0 2048 linear $victim_devno 0"

vgcreate "$vg" "$dev2"
lvcreate -an -L 10M -n "$victim" "$vg"

# The device-link directory of the VG appears with its first active LV
# and links that LV only, so the name of an inactive LV is free there,
# and a host entry of that name (an mdadm array node, a stale udev link)
# sits where the LV's device would be looked up.  One symlink to a
# foreign device stands for such an entry, but only in a directory this
# test is sure LVM made: stop if it is there already.
test -d "$DM_DEV_DIR/$vg" &&
	skip "the device directory of $vg exists already"

lvcreate -ay -L 10M -n live "$vg"
check lv_field "$vg/live" lv_active "active"
ln -s "../mapper/$victim" "$DM_DEV_DIR/$vg/$victim"

# -l wholevg walks every LV of the VG, including the inactive one.
blkdeactivate -l wholevg "$DM_DEV_DIR/mapper/$vg-live" >out 2>err
check lv_field "$vg/live" lv_active ""
# The foreign device and what is stacked on it are not blkdeactivate's.
dmsetup info "$victim" >/dev/null
dmsetup info "$victim-holder" >/dev/null

rm -f "$DM_DEV_DIR/$vg/$victim"
dmsetup remove "$victim-holder"
dmsetup remove "$victim"

vgremove -ff "$vg"
