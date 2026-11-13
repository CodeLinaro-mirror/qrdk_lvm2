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

# Converting a 2-legged raid1 to linear, or back to the older mirror type,
# splitting it, and reshaping a raid set all ask for confirmation, and the
# prompt can wait for any time while the array keeps changing.  A leg
# failing while the answer is pending leaves the in-sync ratio at 100%, so
# only the changed health shows it: each command has to refuse after the
# answer instead of acting on the state it read before the question.
#
# The cases share one setup and one script so that none of them pays the
# fixed cost of a test run; the message handed to
# fail_raid_leg_during_prompt() names the operation that failed.

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 3 0 || skip

aux prepare_pvs 6 16
get_devs

vgcreate $SHARED -s 512k "$vg" "${DEVICES[@]}"

#
# fail_raid_leg_during_prompt <VG/LV> <dev> <status> <message> <args>...
# Run lvconvert with <args> and wait for its confirmation prompt, fail
# <dev> while the answer is pending, answer 'y' and require the command to
# refuse: the array changed after the state it decided on, so carrying the
# operation out would act on a state no one confirmed.  <status> is what
# dmsetup status has to report once the leg is dead, <message> the failure
# printed if the command acted anyway.  Only the extents of <dev> fail: its
# metadata area stays writable, so a command that ignores the change is not
# stopped by a failed VG write instead.  <dev> is re-enabled before
# returning; the array keeps the leg dead until something reloads it.
fail_raid_leg_during_prompt() {
	local vlv=$1 dev=$2 expected=$3 msg=$4
	local vg=${vlv%/*} lv=${vlv#*/}
	local pid i pe_start
	local probe_blocks=16	# 1 MiB, enough I/O for dm-raid to notice
	shift 4

	# Read before lvconvert holds the VG lock across its prompt.
	pe_start=$(get first_extent_sector "$dev")

	# 'out' goes too: the redirect below is set up by the child, so until it
	# runs the file would still hold the prompt of the previous call, and the
	# wait would take the command for having asked.
	rm -f answer out
	mkfifo answer
	exec 3<>answer
	lvconvert "$@" <&3 3<&- >out 2>&1 &
	pid=$!

	for i in {1..100}; do
		grep -q '\[y/n\]' out 2>/dev/null && break
		sleep .1
	done
	grep '\[y/n\]' out || {
		should cat out
		die "lvconvert did not ask to confirm $vg/$lv"
	}

	aux error_dev "$dev" "$pe_start:"
	# Write through the array so dm-raid marks the failed leg dead.
	dd if=/dev/zero of="$DM_DEV_DIR/$vlv" bs=64k count="$probe_blocks" \
		oflag=direct 2>/dev/null || true
	dmsetup status "$vg-$lv" | grep "$expected"

	echo y >&3
	wait "$pid" && die "$msg"
	exec 3>&-
	aux enable_dev "$dev"
	cat out
	grep "changed while waiting for confirmation" out
}

# Split a 2-legged raid1, which drops all of its resilience.
_split_prompt() {
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv1 "$vg" "$dev1" "$dev2"
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv2 "$vg" "$dev3" "$dev4"
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv3 "$vg" "$dev5" "$dev6"
	aux wait_for_sync "$vg" "$lv1"
	aux wait_for_sync "$vg" "$lv2"
	aux wait_for_sync "$vg" "$lv3"

	# Answering 'y' on an unchanged array splits it.
	echo y | lvconvert --splitmirrors 1 -n split3 "$vg/$lv3"
	check lv_exists "$vg" split3

	# Keep the answer pending while a leg fails, then require a refusal.
	fail_raid_leg_during_prompt "$vg/$lv1" "$dev2" " raid1 2 AD " \
		"Split carried out on the state read before the prompt." \
		--splitmirrors 1 -n split1 "$vg/$lv1" "$dev1"
	check lv_not_exists "$vg" split1
	check lv_field "$vg/$lv1" segtype raid1

	# Same for a tracked split, whose image would stay visible.
	fail_raid_leg_during_prompt "$vg/$lv2" "$dev4" " raid1 2 AD " \
		"Tracked split carried out on the state read before the prompt." \
		--splitmirrors 1 --trackchanges "$vg/$lv2" "$dev3"
	lvs --noheadings -o lv_name "$vg" >lvs_out
	not grep rimage lvs_out

	lvremove -y "$vg/split3" "$vg/$lv1" "$vg/$lv2"
}

# Convert raid1 back to the older mirror type, which drops the resilience
# of the array as well.
_mirror_prompt() {
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv1 "$vg" "$dev1" "$dev2"
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv2 "$vg" "$dev3" "$dev4"
	aux wait_for_sync "$vg" "$lv1"
	aux wait_for_sync "$vg" "$lv2"

	# Answering 'y' on an unchanged array converts it.
	echo y | lvconvert --type mirror "$vg/$lv2"
	check lv_field "$vg/$lv2" segtype mirror

	fail_raid_leg_during_prompt "$vg/$lv1" "$dev2" " raid1 2 AD " \
		"Mirror conversion carried out on the state read before the prompt." \
		--type mirror "$vg/$lv1"
	check lv_field "$vg/$lv1" segtype raid1

	lvremove -y "$vg/$lv2" "$vg/$lv1"
}

# Convert a 2-legged raid1 to linear, which keeps one of the two legs and
# drops all resilience.
_linear_prompt() {
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv1 "$vg" "$dev1" "$dev2"
	lvcreate --yes --type raid1 -m 1 -l 8 -n $lv2 "$vg" "$dev3" "$dev4"
	aux wait_for_sync "$vg" "$lv1"
	aux wait_for_sync "$vg" "$lv2"

	# Answering 'y' on an unchanged array converts it.
	echo y | lvconvert -m 0 "$vg/$lv2"
	check lv_field "$vg/$lv2" segtype linear

	# The first leg is the one kept; fail it while the answer is pending.
	fail_raid_leg_during_prompt "$vg/$lv1" "$dev1" " raid1 2 DA " \
		"Linear conversion carried out on the state read before the prompt." \
		-m 0 "$vg/$lv1"
	check lv_field "$vg/$lv1" segtype raid1

	lvremove -y "$vg/$lv2" "$vg/$lv1"
}

# Add a data stripe to a raid5, which moves the whole array.
# A reshape needs dm-raid 1.15, the conversions above need only 1.3, so the
# reshape case runs where the target can do it instead of skipping the whole
# script on an older one.
_reshape_prompt() {
	aux have_raid 1 15 0 || return 0

	lvcreate --yes --type raid5_ls -i 3 -l 12 -n $lv1 "$vg" \
		"$dev1" "$dev2" "$dev3" "$dev4"
	aux wait_for_sync "$vg" "$lv1"

	# Answering 'n' leaves the LV as it was.
	echo n | not lvconvert --stripes 4 "$vg/$lv1" >out 2>&1
	cat out
	grep "NOT converted" out
	check lv_field "$vg/$lv1" stripes 4

	# Answering 'y' on an unchanged array carries the reshape out.
	echo y | lvconvert --stripesize 32k "$vg/$lv1"
	check lv_field "$vg/$lv1" stripesize "32.00k"
	aux wait_for_sync "$vg" "$lv1"

	# Keep the answer pending while a leg fails.
	fail_raid_leg_during_prompt "$vg/$lv1" "$dev2" " raid5_ls 4 ADAA " \
		"Reshape carried out on the state read before the prompt." \
		--stripes 4 "$vg/$lv1"
	check lv_field "$vg/$lv1" stripes 4

	lvremove -y "$vg/$lv1"
}

_split_prompt
_mirror_prompt
_linear_prompt
_reshape_prompt

vgremove -ff "$vg"
