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

# A filesystem without a block device is reported with major 0 in
# mountinfo, and the text around it is controlled by whoever created the
# mount: mount point and mount root always, source for most filesystems
# (an unprivileged FUSE user may set all three).  Such text must not make
# libdm report an arbitrary device-mapper device as mounted, or the user
# can make root's lvchange -an fail with "contains a filesystem in use".
#
# tmpfs has major 0 like FUSE and root can mount it anywhere, so both
# spoofed fields are covered without /dev/fuse and without a second user.
#
# The mount table is only consulted while the device has an opener, so
# each case holds the device node for 3s: long enough to enter the check,
# short enough for the 5s retry window to see the device closed again.
#
# Runs only with extended tests (make check_local_extended).

. lib/inittest --with-extended

# tmpfs is the cheap stand-in for FUSE: major 0 and root can mount it.
grep -qw tmpfs /proc/filesystems || skip "missing tmpfs"

aux have_fs ext4 || skip

aux prepare_vg 1

lvcreate -n $lv1 -L 4M $vg

lv_dev="$DM_DEV_DIR/$vg/$lv1"
mnt="$PWD/mnt_mountinfo_spoof"
mkdir -p "$mnt/dev/mapper/$vg-$lv1" "$mnt/real"

cleanup_mounted_and_teardown()
{
	umount "$mnt/real" 2>/dev/null || true
	umount "$mnt/dev/mapper/$vg-$lv1" 2>/dev/null || true
	umount "$mnt" 2>/dev/null || true
	aux teardown
}
trap 'cleanup_mounted_and_teardown' EXIT

# Hold the node open while lvchange checks it, as a momentary opener such
# as udev's blkid does: an open node is what makes lvm consult the mount
# table, so this is the state the spoof exploits.  That lvm really does
# consult it is shown by the mounted-filesystem case below.
deactivate_while_open()
{
	local holder rc

	( exec 9<"$lv_dev" || exit 1; sleep 3 ) &
	holder=$!
	rc=0
	lvchange -an "$vg/$lv1" > lvchange.out 2>&1 || rc=$?
	wait "$holder"

	if [ "$rc" -ne 0 ]; then
		echo "lvchange -an failed with $rc:"
		cat lvchange.out
		return "$rc"
	fi
}

# A filesystem really mounted on the LV must make the deactivation
# refuse.  This proves that the mount table is consulted here, so the
# cases below fail or pass on the spoofed text and not on nothing being
# checked at all.
mkfs.ext4 "$lv_dev"
mount "$lv_dev" "$mnt/real"
not lvchange -an "$vg/$lv1"
umount "$mnt/real"
check lv_field $vg/$lv1 lv_active "active"

# The mount point names the device, as in the reported mount.
mount -t tmpfs tmpfs "$mnt/dev/mapper/$vg-$lv1"
deactivate_while_open
check lv_field $vg/$lv1 lv_active ""
umount "$mnt/dev/mapper/$vg-$lv1"

# The source names the device, as an unprivileged FUSE mount can.
lvchange -ay "$vg/$lv1"
mount -t tmpfs "/dev/mapper/$vg-$lv1" "$mnt"
deactivate_while_open
check lv_field $vg/$lv1 lv_active ""
umount "$mnt"

vgremove -ff $vg
