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

aux have_raid 1 13 0 || skip
aux target_at_least dm-delay 1 1 0 || skip "Missing dm-delay target."

# Succeed while both legs are alive but not yet in sync and the array is
# resyncing.  The two components come from one status read.
all_a_resync() {
	local state

	state=$(get dm_status_raid "$1/$2" health action) || return 1
	echo "${3}: health and action \"${state}\""
	[[ "$state" = "aa resync" ]]
}

aux prepare_pvs 2
get_devs
vgcreate $SHARED -s 2m "$vg" "${DEVICES[@]}"

# Delay the second image data, but leave its metadata area undelayed.
extent_size=$(get vg_field "$vg" vg_extent_size --units s --nosuffix)
first_data_extent=$(($(get first_extent_sector "$dev2") + extent_size))
aux delay_dev "$dev2" 0 400 "$first_data_extent:$((2 * extent_size))"
lvcreate --type raid1 -m 1 -aey --ignoremonitoring -l 2 -n "$lv1" \
	"$vg" "$dev1" "$dev2"

printf 'raid1-resync-data\n' > "$DM_DEV_DIR/$vg/$lv1"
aux wait_raid_state "$vg" "$lv1" resync
all_a_resync "$vg" "$lv1" "Before primary removal" || \
	skip "Did not observe an all-a RAID1 resync."

if lvconvert --yes -m 0 "$vg/$lv1" "$dev1" >out 2>&1; then
	cat out
	die "Primary removal unexpectedly succeeded during resync."
fi
cat out
grep -F "Unable to remove primary RAID image while array resyncing." out

all_a_resync "$vg" "$lv1" "Before secondary removal" || \
	skip "Resync finished before secondary removal."
lvconvert --yes -m 0 "$vg/$lv1" "$dev2"
check lv_field "$vg/$lv1" segtype linear
check lv_on "$vg" "$lv1" "$dev1"
dd if="$DM_DEV_DIR/$vg/$lv1" bs=512 count=1 2>/dev/null | \
	grep -a -q '^raid1-resync-data'

aux enable_dev "$dev2"
vgremove -ff "$vg"
