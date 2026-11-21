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

. lib/inittest --skip-with-lvmpolld

aux have_fs ext4 || skip

# The resize helper refuses to resize a filesystem that is still mounted
# after it unmounted it here, and it asks the mountinfo of every process,
# because a mount namespace keeps a filesystem busy where /proc/self/
# mountinfo cannot show it.  That needs unshare(1) and a kernel which
# allows mount namespaces.
command -v unshare >/dev/null || skip "unshare is not available"
unshare -m true 2>/dev/null || skip "mount namespaces are not supported"

aux prepare_vg 1 100

mount_dir="mnt_lvresize_fs_ns"
# The space before the digit makes the mount table write the name as
# "mnt_lvresize_fs_ns\0401", so the refusal also checks the escape decode.
mount_dir_2="mnt_lvresize_fs_ns 1"
mkdir -p "$mount_dir" "$mount_dir_2"

lvcreate -L 30M -n $lv1 $vg
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"

# The block count proves the filesystem was not resized; without
# dumpe2fs the other checks still run.
have_dumpe2fs=0
if command -v dumpe2fs >/dev/null; then
	have_dumpe2fs=1
	dumpe2fs -h "$DM_DEV_DIR/$vg/$lv1" | grep '^Block count' > bc1
fi

# Mount the filesystem from a short-lived process in its own mount
# namespace; the sentinel tells when the mount is in place.
ns_ready="$TESTDIR/ns_ready"
rm -f "$ns_ready"

# The mount in the other namespace goes away when the process holding it
# does, so every exit after the start has to stop it.
stop_ns() {
	kill "$ns_pid" 2>/dev/null || true
	wait "$ns_pid" 2>/dev/null || true
}

unshare -m sh -c '
	mount --make-rprivate / 2>/dev/null || exit 1
	mount "$1" "$2" || exit 1
	touch "$3" || exit 1
	exec sleep 30
' sh "$DM_DEV_DIR/$vg/$lv1" "$TESTDIR/$mount_dir_2" "$ns_ready" &
ns_pid=$!

# An aborting assertion must not leave the holder running.  The test's
# trap replaces the harness's, so it stops the holder and runs the
# teardown itself.
cleanup_ns_and_teardown()
{
	stop_ns
	aux teardown
}
trap 'cleanup_ns_and_teardown' EXIT
for i in {1..100}; do
	test -e "$ns_ready" && break
	kill -0 "$ns_pid" 2>/dev/null || break
	sleep 0.1
done
if ! test -e "$ns_ready"; then
	stop_ns
	skip "cannot mount in a separate mount namespace"
fi
if mountpoint -q "$mount_dir_2"; then
	stop_ns
	umount "$mount_dir_2"
	skip "the mount in the other namespace is visible here"
fi

# Mounted here as well, so the resize unmounts it before the fsck, and
# the helper then finds the other namespace's mount and refuses.
mount "$DM_DEV_DIR/$vg/$lv1" "$mount_dir"
not lvextend --fs resize --fsmode offline -L+10M $vg/$lv1 2>err
stop_ns

grep "still mounted" err
# The refusal names the mount point only the other namespace sees, decoded
# back from the mount table's octal escape rather than half eaten by it.
grep "$mount_dir_2" err
# fsmode offline leaves the filesystem unmounted, and it was not resized.
not mountpoint -q "$mount_dir"
if test "$have_dumpe2fs" -eq 1; then
	dumpe2fs -h "$DM_DEV_DIR/$vg/$lv1" | grep '^Block count' > bc2
	diff bc1 bc2
fi

vgremove -ff $vg
