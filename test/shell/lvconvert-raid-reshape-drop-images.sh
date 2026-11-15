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

# A shrinking reshape runs in two steps: the reshape moves the data off the
# last leg and leaves that image pair in the metadata flagged for removal,
# then a second request with the same target layout finds the kernel already
# at the smaller device count and drops the pair.  The second step is not a
# reshape: it needs no --force and asks no confirmation, so refusing or
# prompting there would stall a conversion that has nothing left to do.
#
# Only the long reshape test walked this step, so cover it on its own.

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 14 0 || skip

# Wait until the kernel runs @vg/@lv on @devs devices, which is how a
# finished reshape looks from outside: the metadata still lists the freed
# image pair, the kernel has already reshaped onto fewer devices.  Waiting
# for 'idle' alone also matches the moment before the reshape starts.
wait_for_dev_count() {
	local i

	for i in {1..200}; do
		# One status read, so the count and the action describe the
		# same state of the array.
		[ "$(get dm_status_raid "$1/$2" dev_count action)" = "$3 idle" ] &&
			return 0
		sleep .05
	done

	die "$1/$2 did not reshape onto $3 devices"
}

aux prepare_pvs 4 16
get_devs

vgcreate $SHARED -s 512k "$vg" "${DEVICES[@]}"

# 3 data stripes plus one parity leg on 4 PVs.
lvcreate --yes --type raid5_ls -i 3 -l 12 -n $lv1 "$vg" \
	"$dev1" "$dev2" "$dev3" "$dev4"
aux wait_for_sync "$vg" "$lv1"

size_before=$(get lv_field "$vg/$lv1" lv_size --units s --nosuffix)

# Shrink to 2 data stripes: the kernel reshapes onto 3 devices while the
# freed pair stays in the metadata, flagged for removal.
lvconvert --yes --force --stripes 2 "$vg/$lv1"
wait_for_dev_count "$vg" "$lv1" 3

lvs -a -o lv_name,segtype,stripes,data_stripes,lv_size "$vg"
check lv_field "$vg/$lv1" stripes 4
check lv_field "$vg/$lv1" data_stripes 2

test "$(lvs -a --noheadings -o lv_name "$vg" | wc -l)" -eq 9 ||
	die "expected the freed image pair to stay in $vg after the reshape:" \
	    "$(lvs -a --noheadings -o lv_name "$vg")"

# Drop the freed pair.  No --yes and no input, so a prompt here would
# refuse; no --force, which removing stripes still needs.
lvconvert --stripes 2 "$vg/$lv1" < /dev/null
aux wait_for_sync "$vg" "$lv1" 1

lvs -a -o lv_name,segtype,stripes,data_stripes,lv_size "$vg"
check lv_field "$vg/$lv1" stripes 3
check lv_field "$vg/$lv1" data_stripes 2
check pv_field "$dev4" pv_used 0 --units b --nosuffix

test "$(lvs -a --noheadings -o lv_name "$vg" | wc -l)" -eq 7 ||
	die "expected the freed image pair to be gone from $vg:" \
	    "$(lvs -a --noheadings -o lv_name "$vg")"

test "$(get lv_field "$vg/$lv1" lv_size --units s --nosuffix)" -lt "$size_before" ||
	die "LV size did not shrink"

vgremove -ff "$vg"
