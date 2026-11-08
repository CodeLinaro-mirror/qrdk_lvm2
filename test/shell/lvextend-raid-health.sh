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

. lib/inittest

aux have_raid 1 5 0 || skip
# From 1.15.0 the target synchronizes the new space itself, so an extension
# of a running array leaves no request and the table has no sync argument;
# an older target leaves the request, which the load turns into the argument.
SYNC_TOKEN=no
aux have_raid 1 15 0 || SYNC_TOKEN=yes
aux prepare_vg 3

# backup_expect_resync <file> <yes|no>: save a VG backup into <file> and
# assert whether it asks for a full resynchronization.  Several callers
# restore from the same file afterwards.
backup_expect_resync() {
	vgcfgbackup -f "$1" "$vg" > /dev/null
	if [ "$2" = yes ]; then
		grep '"RESYNC"' "$1"
	else
		not grep '"RESYNC"' "$1"
	fi
}

# check_table_arg <map> <arg> <yes|no>: assert whether the live table carries
# <arg>.  A loaded table keeps the arguments it was loaded with, so this does
# not depend on when a background action finishes.
check_table_arg() {
	dmsetup table "$1" > table
	if [ "$3" = yes ]; then
		grep -qw "$2" table ||
			die "the table of $1 does not carry the \"$2\" argument"
	else
		not grep -qw "$2" table ||
			die "the table of $1 still carries the \"$2\" argument"
	fi
}

# check_sync_table <map> <yes|no>: a load served a pending resynchronization
# request, so the table carries the one-shot sync argument.
check_sync_table() {
	check_table_arg "$1" sync "$2"
}

# degrade_leg <dev> <byte>: fail writes to one RAID leg while its PV label
# stays readable, dirty the array through the top LV and check that the leg
# is reported dead.
degrade_leg() {
	aux error_dev "$1" "$(get first_extent_sector "$1"):"
	echo "$2" > "$DM_DEV_DIR/$vg/$lv1"
	sync
	check raid_leg_status "$vg" "$lv1" "AD"
}

lvcreate --yes --type raid1 -m 1 -L 16M -n "$lv1" "$vg" "$dev1" "$dev2"
aux wait_for_sync "$vg" "$lv1"

# Keep the PV label readable but fail writes to the second RAID leg.
degrade_leg "$dev2" b
dmsetup table "$vg-$lv1" > table_before
not lvextend --yes -L +4M "$vg/$lv1" > out 2>&1
grep 'Cannot resize or resynchronize RAID LV' out
check lv_field "$vg/$lv1" lv_size "16.00m"
dmsetup table "$vg-$lv1" > table_after
cmp table_before table_after

# A replacement must finish recovery before extending the array.
aux delay_dev "$dev3" 0 100 "$(get first_extent_sector "$dev3"):"
lvconvert -y --repair "$vg/$lv1" "$dev3"
aux wait_raid_state "$vg" "$lv1" recover rebuild
not lvextend --yes -L +4M "$vg/$lv1" > out 2>&1
grep 'Cannot resize or resynchronize RAID LV' out
check lv_field "$vg/$lv1" lv_size "16.00m"
aux enable_dev "$dev3"
aux wait_for_sync "$vg" "$lv1"
check raid_leg_status "$vg" "$lv1" "AA"
aux enable_dev "$dev2"

lvextend --yes -L +4M "$vg/$lv1"
# An extension of a running array leaves the sync request only when the
# target cannot synchronize the new space itself.
check_sync_table "$vg-$lv1" "$SYNC_TOKEN"
aux wait_for_sync "$vg" "$lv1"
check lv_field "$vg/$lv1" lv_size "20.00m"
dd if="$DM_DEV_DIR/$vg/$lv1" bs=1 count=1 2>/dev/null | grep -q b

# A completed extension must not leave a one-shot request in VG metadata.
backup_expect_resync backup no
lvchange --refresh "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"

# Simulate a committed request left after an interrupted operation.  A
# refresh replays it and then clears it.
aux inject_status backup "$lv1" RESYNC > pending
grep '"RESYNC"' pending
vgcfgrestore --yes -f pending "$vg"
backup_expect_resync restored yes
lvs --noheadings -o lv_name,resync_pending "$vg/$lv1" | grep -q 'resync pending'

# A region size change is not combined with a pending resync: it would
# reload the table and leave the request behind.
region=$(get lv_first_seg_field "$vg/$lv1" regionsize --units s --nosuffix)
not lvconvert --regionsize "$(( region * 2 ))s" "$vg/$lv1" > out 2>&1
grep 'Cannot change region size' out
lvchange --refresh "$vg/$lv1"
check_sync_table "$vg-$lv1" yes
backup_expect_resync cleared no
lvs --noheadings -o lv_name,resync_pending "$vg/$lv1" | not grep -q 'resync pending'
aux wait_for_sync "$vg" "$lv1"

# Refuse to replay the request on a degraded array and keep it for later.
vgcfgrestore --yes -f pending "$vg"
degrade_leg "$dev3" c
dmsetup table "$vg-$lv1" > table_before
not lvchange --refresh "$vg/$lv1" > out 2>&1
grep 'Cannot resize or resynchronize RAID LV' out
dmsetup table "$vg-$lv1" > table_after
cmp table_before table_after
backup_expect_resync still_pending yes
vgcfgrestore --yes -f backup "$vg"
aux enable_dev "$dev3"

# A pending request must not block repairing a failed leg.  The table
# carries the rebuild and the full resync waits for the load after it.
vgcfgrestore --yes -f pending "$vg"
degrade_leg "$dev3" d
aux delay_dev "$dev2" 0 100 "$(get first_extent_sector "$dev2"):"
lvconvert -y --repair "$vg/$lv1" "$dev2"
aux wait_raid_state "$vg" "$lv1" recover rebuild
backup_expect_resync deferred yes
# A refresh during the rebuild reloads the table without the request, so
# the load it performs must not be taken as the request being served.
lvchange --refresh "$vg/$lv1"
backup_expect_resync still_deferred yes
aux enable_dev "$dev2"
aux wait_for_sync "$vg" "$lv1"
check raid_leg_status "$vg" "$lv1" "AA"
aux enable_dev "$dev3"

# The deferred request is served by the next load instead of dropped.
lvchange --refresh "$vg/$lv1"
check_sync_table "$vg-$lv1" yes
backup_expect_resync served no
aux wait_for_sync "$vg" "$lv1"

# A stopped array whose leg's PV is gone is activated degraded without
# the request: refusing the activation would make the array unreadable,
# and the request has to stay for the activation that can serve it.  The
# backed up metadata puts the second leg on dev3.
vgcfgrestore --yes -f pending "$vg"
lvchange -an "$vg/$lv1"
aux disable_dev "$dev3"
lvchange -ay --activationmode partial "$vg/$lv1"
check raid_leg_status "$vg" "$lv1" "AD"
lvchange -an "$vg/$lv1"
aux enable_dev "$dev3"
backup_expect_resync kept_by_missing_leg yes
lvchange -ay "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"
backup_expect_resync served_after_missing_leg no

# Refresh of a stopped array is a no-op and must not clear its request.
lvchange -an "$vg/$lv1"
vgcfgrestore --yes -f pending "$vg"
lvchange --refresh "$vg/$lv1"
check lv_field "$vg/$lv1" lv_active ""
backup_expect_resync kept_by_inactive_refresh yes

# The activation filter can also return success without loading a table.
lvchange -ay --config 'activation/volume_list = [ "vg_not_exist" ]' "$vg/$lv1"
check lv_field "$vg/$lv1" lv_active ""
backup_expect_resync kept_by_activation_filter yes
lvchange -ay "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"
backup_expect_resync served_after_filter no

# replay_on_activation <label>: activate a stopped array that holds the
# request and check that the activation served it and cleared it.
replay_on_activation() {
	lvchange -an "$vg/$lv1"
	vgcfgrestore --yes -f pending "$vg"
	lvchange -ay "$vg/$lv1"
	check_sync_table "$vg-$lv1" yes
	aux wait_for_sync "$vg" "$lv1"
	backup_expect_resync "$1" no
}

# Activating a stopped array replays the request, then clears it.
replay_on_activation cleared

# Activation does not reload a running array, so the request survives it
# and the user is pointed at the operation that does reload.
vgcfgrestore --yes -f pending "$vg"
lvchange -ay "$vg/$lv1" > out 2>&1
grep 'needs "lvchange --refresh"' out
backup_expect_resync kept yes
lvchange --refresh "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"
backup_expect_resync cleared no

# A load that rebuilds a leg cannot carry the resync as well.  The
# request must survive that load instead of being cleared as served.
lvchange -an "$vg/$lv1"
aux inject_status backup "$lv1" RESYNC > pending_rebuild
aux inject_status pending_rebuild "${lv1}_rimage_1" REBUILD > injected
grep '"REBUILD"' injected
vgcfgrestore --yes -f injected "$vg"
lvchange -ay "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"
backup_expect_resync kept_by_rebuild yes
vgcfgrestore --yes -f backup "$vg"
lvchange --refresh "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"
backup_expect_resync after_rebuild no

# A cleared request must not start another resync after a restart.  A
# spurious resync appears in the status at once, so sample the whole
# window and fail at the first non-idle action instead of sleeping blind.
aux delay_dev "$dev3" 0 100 "$(get first_extent_sector "$dev3"):"
lvchange -an "$vg/$lv1"
lvchange -ay "$vg/$lv1"
for _ in {1..10}; do
	check raid_action "$vg" "$lv1" idle
	sleep .1
done
check in_sync "$vg" "$lv1"
aux enable_dev "$dev3"

vgcfgrestore --yes -f backup "$vg"

# A resize of a stopped array commits the new size without loading a table,
# so the load that would carry the request never happens, and there is no
# running array whose health could be checked.  The request must survive the
# resize and be served by the activation that follows.
lvchange -an "$vg/$lv1"
vgcfgrestore --yes -f pending "$vg"
lvresize --yes --fs ignore -L 24M "$vg/$lv1"
check lv_field "$vg/$lv1" lv_size "24.00m"
backup_expect_resync kept_by_inactive_grow yes

# The same holds for a shrink, which needs dm-raid 1.9.0.
if aux have_raid 1 9 0; then
	lvresize --yes --fs ignore -L 16M "$vg/$lv1"
	check lv_field "$vg/$lv1" lv_size "16.00m"
	backup_expect_resync kept_by_inactive_shrink yes
fi

lvs --noheadings -o lv_name,resync_pending "$vg/$lv1" | grep -q 'resync pending'
lvchange -ay "$vg/$lv1"
aux wait_for_sync "$vg" "$lv1"
backup_expect_resync served_after_inactive_resize no
