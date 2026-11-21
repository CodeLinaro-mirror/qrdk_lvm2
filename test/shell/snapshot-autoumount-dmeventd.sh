#!/usr/bin/env bash

# Copyright (C) 2010-2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# no automatic extensions please



. lib/inittest --skip-with-lvmpolld

aux have_fs ext2 || skip

aux lvmconf "activation/snapshot_autoextend_percent = 0" \
            "activation/snapshot_autoextend_threshold = 100"

aux prepare_dmeventd
aux prepare_vg 2
# The blank before the digit makes the mount table write the name as
# "${PREFIX}mnt\040space\0401", which dmeventd must decode before it
# passes the mount point to umount.
mntdir="${PREFIX}mnt space 1"
mntdir_esc="${PREFIX}mnt\\040space\\0401"

lvcreate -aey -L8 -n base $vg
mkfs.ext2 "$DM_DEV_DIR/$vg/base"

lvcreate -s -L4 -n snap $vg/base
lvchange --monitor y $vg/snap

mkdir "$mntdir"
# Use remount-ro  to avoid logging kernel WARNING
mount -o errors=remount-ro "$DM_DEV_DIR/mapper/$vg-snap" "$mntdir"

test "$(dmsetup info -c --noheadings -o open $vg-snap)" -eq 1

grep -F "$mntdir_esc" /proc/mounts

# overfill 4M snapshot (with metadata)
not dd if=/dev/zero of="$mntdir/file" bs=1M count=4 conv=fdatasync

# Should be nearly instant check of dmeventd for invalid snapshot.
# Wait here for umount and open_count drops to 0 as it may
# take a while to finalize umount operation (it might be already
# removed from /proc/mounts, but still opened).
for i in {1..100}; do
	sleep .1
	test "$(dmsetup info -c --noheadings -o open $vg-snap)" -eq 0 && break
done

not grep -F "$mntdir_esc" /proc/mounts

vgremove -f $vg
