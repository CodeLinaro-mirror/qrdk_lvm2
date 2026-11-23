#!/usr/bin/env bash

# Copyright (C) 2024-2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA2110-1301 USA
#

#
# Two device size rules are checked here.  Both are asynchronous:
# - a stripe adding reshape keeps the size the LV had before it started
#   until the reshape finished, then the kernel grows it
# - a stripe removing reshape shrinks the size before the reshape starts,
#   so that size can be read at any time
#
# The size an adding reshape holds is only observable while the kernel still
# reports the reshape running, which is what the delay and the background
# lvconvert below are for.  A removing reshape needs neither, so it runs in
# the foreground and without a delay.
#
# Sampling the held size
# ----------------------
#
# lvconvert is started in the background and the LV is polled every 50 ms:
# 'dmsetup status' reports the sync action, 'blockdev' the size.  Only a
# sample with 'reshape' reported both before and after the size read counts,
# and the size then has to be the pre-reshape one.  The polling does not stop
# when the command returns: on raid4/5/6 md starts the reshape only then, and
# on raid10 it can still be running.
#
# A size read is only evidence about the running array while the LV is
# resumed.  dm core applies the size of the table being loaded in __bind(),
# with the LV still suspended and before the target's preresume() runs, so a
# suspended LV reports the size of a table that is not running - 'idle'
# while the table is not swapped in yet, 'frozen' until raid_resume() after
# the swap.  raid_preresume() then puts back the size of the running array,
# so the resumed LV keeps the pre-reshape size until the reshape ended, and
# only then reports the grown one.
#
# Such a window is not only a nuisance for a test: a reader of the size -
# udev's blkid, a resize helper - sees it too, and a bind that fails later
# is rolled back the same way, leaving a reader with a size the device never
# takes.  Whether that should stay visible is worth a device-mapper
# discussion.
#
# The size and the status cannot be read in one go, so a sample tells the
# two cases apart by the device count of the table that answered it: only
# the table with the added stripes reports the new count, and only that
# table is the live one.
#
# Slowing the reshape down with dm-delay
# --------------------------------------
#
# Without a delay the reshape can finish between two samples, and the test
# then fails without having looked at the held size.  Delaying the writes of
# one PV with dm-delay buys the window with time instead of space: a few MiB
# are enough to keep the reshape busy long enough, so the test stays small
# enough for CI machines.  A reshape writes every stripe to all its members,
# so delaying one member holds the whole array back.  The per-layout 'ms'
# below is the knob for that.  The window has to outlast a poll period and
# not just any sample, since a sample only counts when it reads the size
# between two 'reshape' readings taken one period apart; raid4/5/6 get such
# a window from the checkpoint rule below and need little of the delay,
# while on raid10 the delay is all there is.  20 ms was enough on an idle
# machine and too little on an instrumented build, which polls about three
# times slower.
#
# raid4/5/6: the size is held by a window between two counters
# (drivers/md/raid5.c):
#
#   reshape_progress  is advanced once per chunk, when the chunk is
#                     scheduled in reshape_request(), i.e. before its
#                     writes have completed
#   reshape_safe      is advanced only on a metadata checkpoint, where it is
#                     set to reshape_progress
#
# get_reshape_loc() reports everything between the two counters as
# LOC_INSIDE_RESHAPE and refuses I/O there (STRIPE_SCHEDULE_AND_RETRY, the
# bio is parked on conf->wait_for_reshape) until a checkpoint collapses the
# window.  A checkpoint happens only when copying one more chunk would
# overwrite the source of an earlier one, or every 10 seconds
# ("time_after(jiffies, conf->reshape_checkpoint + 10*HZ)"), so a reshape
# that takes a couple of seconds gets a single one, at its end, and the
# window then spans everything it has copied.
#
# That matters because udev runs blkid over the LV as soon as an event
# arrives, and blkid reads the first ~2.5 MiB and the last ~1.5 MiB of the
# LV.  With a shrink the window starts at its end and grows downwards, so
# the last-MiB read is inside it from the first chunk on; with a grow the
# same happens to the first-MiB read.  Such a read waits for the next
# checkpoint, and while it waits it holds bd_mutex, which stalls every other
# access to the LV - that is the multi-second "waiting for zero" seen at the
# end of lvconvert.
#
# raid10: drivers/md/raid10.c has no such window.  reshape_request() raises
# the array barrier for every batch it copies, at most RESYNC_SECTORS
# (64 KiB) or the rest of the stripe chunk, and reshape_request_write()
# writes the batch to every member (conf->copies * 2 bios), so regular I/O
# waits in wait_barrier() for each batch.  I/O spanning reshape_progress
# waits in regular_request_wait() as well.  The region size is only the
# write intent bitmap chunk; it changes neither the batch nor the delay, so
# --regionsize does not help here.
#

LVM_SKIP_LARGE_TESTS=${LVM_SKIP_LARGE_TESTS:-1}
SKIP_FSCK=${SKIP_FSCK:-1}
SKIP_RESIZE=${SKIP_RESIZE:-0}

# Left empty on purpose: the test runs with udev sync and samples the held
# size while the command runs, so the options
#	"--noudevsync" "--config" "activation/verify_udev_operations=1"
# are not wanted, and they would also require making sure the device paths
# exist without udev.
declare -a UDEVOPTS=()

# Timeout in seconds for polling a held size, and for waiting for a size
# update afterwards.
CHECK_SIZE_TIMEOUT=5

# Held-size polling: the delay between two samples, in ms, and how many
# samples fit into CHECK_SIZE_TIMEOUT.  The sleep argument and the count are
# both derived from POLL_DELAY_MS so they cannot disagree.
POLL_DELAY_MS=50
POLL_INTERVAL=$(printf '0.%03u' "$POLL_DELAY_MS")
POLL_COUNT=$(( CHECK_SIZE_TIMEOUT * 1000 / POLL_DELAY_MS ))

# List of stripes to add or remove
tst_stripes=""

# Write delay in ms applied to the delayed range of the first 'PV' while
# stripes are added, where the size the reshape holds has to be sampled
# before the reshape ends.  Needs to be varied depending on layout as they
# come with different reshape overhead.
ms=0

# PV numbers and size
npvs=0
pvsz=0


. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip "Timing is too slow with valgrind."
test "$(aux total_mem)" -gt 1048576 || skip "Not enough RAM for this test"

aux kernel_at_least 6 9 0 || skip
aux have_fs ext4 || skip

test "$SKIP_RESIZE" -eq 1 ||
	command -v resize2fs >/dev/null ||
	skip "resize2fs not available"

# Optimized filesystem check function with better error handling
function _skip_or_fsck
{
	local device=$1
	local options=${2:-y}

	[ "$SKIP_FSCK" -eq 1 ] || fsck -f"$options" "$device"
}

function _get_pvs
{
	case "$LVM_SKIP_LARGE_TESTS" in
	0)
		npvs=64
		pvsz=32
		;;
	*)
		npvs=20
		pvsz=10
		;;
	esac
}

function _get_stripes_and_delay
{
	local raid_type=$1

	case "$LVM_SKIP_LARGE_TESTS" in
	0)
		case "$raid_type" in
		raid[45]*)
			ms=20
			tst_stripes="5 4 11 9 15 13 19 18 22 21 25 23 28 30 31 29 33 31 34 33 37 45 41 50 55 60 63 30 15 10 8 3"
			;;
		raid6*)
			ms=20
			tst_stripes="5 4 11 9 15 13 19 18 22 21 25 23 28 30 31 29 33 31 34 33 37 45 41 50 55 60 62 30 15 10 8 3"
			;;
		raid10*)
			ms=50
			tst_stripes="5 8 9 11 13 14 15 19 20 22 23 25 27 28 29 32"
			;;
		esac
		;;
	*)
		case "$raid_type" in
		raid[45]*)
			ms=20
			tst_stripes="5 4 8 7 11 9 15 19 18 10"
			;;
		raid6*)
			ms=20
			tst_stripes="5 4 8 7 11 9 15 18 10 5"
			;;
		raid10*)
			ms=50
			tst_stripes="5 6 7 8 9 10"
			;;
		esac
		;;
	esac
}

# Report the reshape length in sectors of the first rimage of @vg/@lv.
#
# Reshape length is a per-segment report field: only the first segment
# of the rimage reports it, any other segment adds an empty row,
# hence sum up any values reported.
function _get_raid_reshape_len
{
	local vg=$1
	local lv=$2
	local line
	local reshape_len=0

	# Avoid using pipes as exit codes may cause test failure
	for line in $(lvs --noheadings --nosuffix -aoreshapelen --unit s "$vg/${lv}_rimage_0")
	do
		# Drop everything past 'S'
		reshape_len=$(( reshape_len + ${line/S*} ))
	done

	echo "$reshape_len"
}

function _get_size
{
	local vg=$1
	local lv=$2
	local data_stripes=$3
	local reshape_len rimagesz

	# Get rimage size early
	rimagesz=$(blockdev --getsz "$DM_DEV_DIR/mapper/${vg}-${lv}_rimage_0")

	# Get any reshape size in sectors
	reshape_len=$(_get_raid_reshape_len $vg $lv)

	# Get rimage size - reshape length
	rimagesz=$(( rimagesz - reshape_len ))

	# Calculate size of LV based on above sizes
	echo $(( rimagesz * data_stripes ))
}

function _check_size
{
	local vg=$1
	local lv=$2
	local data_stripes=$3
	local lv_size
	local calc_size

	lv_size=$(blockdev --getsz "$DM_DEV_DIR/$vg/$lv")
	calc_size=$(_get_size $vg $lv "$data_stripes")

	# Compare size of LV with calculated one
	[[ lv_size -eq calc_size ]]
}

# Poll @lv while the lvconvert starting a reshape runs and shortly after it,
# and verify it exposes @old_size while its reshape is ongoing.
#
# The kernel grows the size only once the reshape finished, so a sample
# taken while the reshape runs has to still be @old_size.  The action is
# read before and after the size and both readings have to report 'reshape':
# the reshape end can overtake a single read, and right after a reload
# dm-raid reports no reshape for a moment before the sync thread starts (the
# transient _raid_status_read_stable() in raid_manip.c waits it out).  The
# second reading is reused as the first one of the next poll.
#
# A grown size is the end of the reshape only once the table with the added
# stripes is live - the header says why a reload exposes its size first.
#
# Returns 0 when the held size is confirmed, 1 when it violates the
# contract, 2 when the reshape ended before a running one could be sampled
# with the size still held, 3 when the reshape still runs at the timeout
# without such a sample, and 4 when a sample carried no device count or saw
# a reshape on a table that is not the one with the added stripes.
function _check_size_hold
{
	local vg=$1
	local lv=$2
	local old_size=$3
	local new_dev_count=$4
	local lv_size
	local action
	local action2
	local dev_count
	local status
	local grown
	local live
	local i

	action=$(get dm_status_raid "$vg/$lv" action)

	for (( i = 0; i < POLL_COUNT; i++ ))
	do
		lv_size=$(blockdev --getsz "$DM_DEV_DIR/$vg/$lv")
		status=$(get dm_status_raid "$vg/$lv" action dev_count)
		read -r action2 dev_count <<< "$status"
		# Every poll tells the live table by this count, so a status
		# without one can decide nothing in any poll below.
		[ -n "$dev_count" ] || return 4

		if [ "$action" = reshape ] && [ "$action2" = reshape ]; then
			# A reshape under the old table would answer nothing either.
			[ "$dev_count" = "$new_dev_count" ] || return 4
			# Reshape running: the size has to still be @old_size
			[ "$lv_size" -eq "$old_size" ] && return 0
			return 1
		fi

		grown=0
		[ "$lv_size" -ne "$old_size" ] && grown=1
		live=0
		[ "$dev_count" = "$new_dev_count" ] && live=1

		# The reshape ended once nothing reports it any more, the size grew
		# and the live table is the one with the added stripes.  A reload
		# exposes its size before its table is live, so that is no end.
		if [ "$grown" = 1 ] && [ "$live" = 1 ] &&
		   [ "$action" = idle ] && [ "$action2" = idle ]; then
			return 2
		fi

		action=$action2
		sleep "$POLL_INTERVAL"
	done

	# The timeout left the reshape running with the size still held: the
	# contract holds, but no sample saw it running around the size read.
	[ "$(get dm_status_raid "$vg/$lv" action)" = reshape ] && return 3

	return 2
}

# Run _reshape_layout() with the remaining arguments in the background so the
# size can be polled while it runs, and verify the LV keeps exposing @old_size
# until its reshape finished.  Waiting for the command too means a failed
# conversion fails the test, not just an unsampled size.  The metadata is read
# back by the caller, after its waits.
function _lvconvert_size_held
{
	local raid_type=$1
	local data_stripes=$2
	local vg=$3
	local lv=$4
	local old_size=$5
	shift 5
	local new_dev_count
	local pid
	local rc=0

	# The table with the added stripes reports this device count in the
	# status; _check_size_hold() tells the reshape end by it and requires
	# it once the reshape is seen.
	new_dev_count=$(_total_stripes "$raid_type" "$data_stripes")

	_reshape_layout "$raid_type" "$data_stripes" $vg $lv "$@" &
	pid=$!

	# '|| rc=$?' and not a bare call: the test shell runs with 'set -e', so
	# a nonzero return would abort the run before the command is waited for
	_check_size_hold "$vg" "$lv" "$old_size" "$new_dev_count" || rc=$?

	# The delay has done its job once the held size has been looked for, so
	# reset it whatever the sampling found.  It is what keeps the reshape
	# running slowly and the command below waits for that reshape, so this
	# is worth about a second per reshape; on a sample that failed it also
	# lets the reshape drain and the command exit instead of waiting for it
	# at the delayed rate.
	aux enable_dev "$dev1"

	# Fails the test when the command itself failed
	wait "$pid"

	case "$rc" in
	0)	return 0 ;;
	1)	die "LV size should be small" ;;
	2)	die "reshape of $vg/$lv finished before its size could be sampled" ;;
	3)	die "reshape of $vg/$lv still ran at the timeout without a size sample" ;;
	4)	die "status of $vg/$lv reported an unexpected device count" ;;
	*)	die "unexpected result $rc from _check_size_hold" ;;
	esac
}

function _check_size_timeout
{
	local i

	for (( i = 0; i < POLL_COUNT; i++ ))
	do
		_check_size "$@" && return
		sleep "$POLL_INTERVAL"
	done

	return 1
}

function _total_stripes
{
	local raid_type=$1
	local data_stripes=$2

	case "$raid_type" in
	raid[45]*) echo $(( data_stripes + 1 )) ;;
	raid6*)    echo $(( data_stripes + 2 )) ;;
	raid10*)   echo $(( data_stripes * 2 )) ;;
	esac
}

function _lvcreate
{
	local raid_type=$1
	local data_stripes=$2
	local size=$3
	local vg=$4
	local lv=$5
	shift 5
	local stripes

	stripes=$(_total_stripes "$raid_type" "$data_stripes")

	lvcreate -y -aey --type "$raid_type" -i "$data_stripes" -L "$size" -n $lv $vg "$@"

	check lv_first_seg_field $vg/$lv segtype "$raid_type"
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$stripes"

	echo y|mkfs.ext4 "$DM_DEV_DIR/$vg/$lv"
	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
}

# Start converting @vg/@lv to @raid_type with @data_stripes.  The reshape runs
# in the background: the caller waits for it and reads the metadata back only
# afterwards, so an lvs run never gets in the way of a running reshape.
function _reshape_layout
{
	local raid_type=$1
	local data_stripes=$2
	local vg=$3
	local lv=$4
	shift 4

	lvconvert -y --ty "$raid_type" --stripes "$data_stripes" "${UDEVOPTS[@]}" $vg/$lv "$@"
}

function _not_reshape_layout
{
	local raid_type=$1
	local data_stripes=$2
	local vg=$3
	local lv=$4
	shift 4

	not lvconvert -y --ty "$raid_type" --stripes "$data_stripes" "${UDEVOPTS[@]}" $vg/$lv "$@"
	check lv_first_seg_field $vg/$lv segtype "$raid_type"
}

# Add a stripe to @vg/@lv with its first PV delayed, so the reshape runs long
# enough for the size it holds to be sampled, and verify the LV keeps
# exposing the pre-reshape size until the reshape finished.  The delay is
# dropped by _lvconvert_size_held() as soon as it has seen that size.
function _reshape_adding
{
	local raid_type=$1
	local data_stripes=$2
	local vg=$3
	local lv=$4
	shift 4
	local old_size

	# Delay the writes past the data start of the first PV, so that the size
	# the reshape holds can be sampled before the reshape finishes.
	aux delay_dev "$dev1" "0" "$ms" \
		"$(( $(get first_extent_sector "$dev1") + 2048 ))"

	# Size the LV exposes before the stripe is added, it has to stay at
	# that value until the reshape finished
	old_size=$(blockdev --getsz "$DM_DEV_DIR/$vg/$lv")

	_lvconvert_size_held "$raid_type" "$data_stripes" "$vg" "$lv" "$old_size" "$@"
}

function _add_stripes
{
	local raid_type=$1
	local vg=$2
	local lv=$3
	local data_stripes=$4
	local stripes=
	local stripesize="$((16 << (data_stripes % 5))).00k" # Stripe size variation

	stripes=$(_total_stripes "$raid_type" "$data_stripes")

	_reshape_adding "$raid_type" "$data_stripes" $vg $lv --stripesize "$stripesize"

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
	aux wait_for_sync $vg $lv 0

	# Now size consistency has to be fine
	_check_size_timeout $vg $lv "$data_stripes" || die "LV size should be grown"

	# The reshape is done: read the metadata back only now, an lvs run before
	# the waits above only slows the reshape down.
	check lv_first_seg_field $vg/$lv segtype "$raid_type"
	check lv_first_seg_field $vg/$lv stripesize "$stripesize"
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$stripes"

	# Check, use grown capacity for the filesystem and check again
	if [ "$SKIP_RESIZE" -eq 0 ]
	then
		# Mandatory fsck before resize2fs.
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
		resize2fs "$DM_DEV_DIR/$vg/$lv"
	fi

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
}

function _remove_stripes
{
	local raid_type=$1
	local vg=$2
	local lv=$3
	local data_stripes=$4
	local cur_data_stripes
	local stripes
	local stripesize="$((16 << (data_stripes % 5))).00k" # Stripe size variation

	cur_data_stripes=$(get lv_field "$vg/$lv" datastripes -a)
	stripes=$(get lv_field "$vg/$lv" stripes -a)

	# Check, shrink filesystem to the resulting smaller size and check again
	if [ "$SKIP_RESIZE" -eq 0 ]
	then
		# Mandatory fsck before resize2fs.
		fsck -fy "$DM_DEV_DIR/$vg/$lv"
		resize2fs "$DM_DEV_DIR/$vg/$lv" "$(_get_size $vg $lv "$data_stripes")s"
		_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"
	fi

	_reshape_layout "$raid_type" "$data_stripes" $vg $lv --force --stripesize "$stripesize"

	# Size has to be inconsistent, as to be removed legs still exist.  The
	# size was reduced before the reshape started, so this holds whether or
	# not the reshape is still running.
	_check_size $vg $lv "$cur_data_stripes" && die "LV size should be reduced but not rimage count"

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"

	# The sub LVs left over by the reshape are still there, so a further
	# reshape request has to be refused until they are dropped below
	_not_reshape_layout "$raid_type" $(( data_stripes + 1 )) $vg $lv --force
	aux wait_for_sync $vg $lv 1

	# Remove freed legs as they are now idle has to succeed without --force
	_reshape_layout "$raid_type" "$data_stripes" $vg $lv
	aux wait_for_sync $vg $lv 1
	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv" "fn"

	# Now that the reshape is done, the layout can be read back.
	check lv_first_seg_field $vg/$lv segtype "$raid_type"
	check lv_first_seg_field $vg/$lv stripesize "$stripesize"
	check lv_first_seg_field $vg/$lv datastripes "$data_stripes"
	check lv_first_seg_field $vg/$lv stripes "$(_total_stripes "$raid_type" "$data_stripes")"

	# Now size consistency has to be fine
	_check_size_timeout $vg $lv "$data_stripes" || die "LV size should be completely reduced"

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv" || return 0
}

function _test
{
	local vg=$1
	local lv=$2
	local raid_type=$3
	local data_stripes=$4
	local cur_data_stripes
	local rimagesz

	_get_stripes_and_delay "$raid_type"

	# Calculate maximum rimage size in MiB and subtract 3 extents to leave room for rounding
	rimagesz=$(( $(blockdev --getsz "$dev1") / 2048 - 3 ))

	# Create $raid_type with @data_stripes data stripes
	_lvcreate "$raid_type" "$data_stripes" $(( data_stripes * rimagesz ))M $vg $lv --stripesize 128k
	_check_size $vg $lv "$data_stripes" || die "LV size bogus"
	check lv_first_seg_field $vg/$lv stripesize "128.00k"
	aux wait_for_sync $vg $lv 0

	# Reshape it to one more stripe and 256K stripe size
	_reshape_adding "$raid_type" $(( data_stripes + 1 )) $vg $lv --stripesize 256K

	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"

	# Wait for sync to finish to check the extended LV size
	aux wait_for_sync $vg $lv 0

	_check_size_timeout $vg $lv $(( data_stripes + 1 )) || die "LV size should be grown"
	_skip_or_fsck "$DM_DEV_DIR/$vg/$lv"

	# Loop adding stripes and check size consistency on each iteration
	for data_stripes in $tst_stripes
	do
		cur_data_stripes=$(get lv_field "$vg/$lv" datastripes -a)
		if [ "$cur_data_stripes" -lt "$data_stripes" ]; then
			_add_stripes "$raid_type" $vg $lv "$data_stripes"
		else
			_remove_stripes "$raid_type" $vg $lv "$data_stripes"
		fi
	done

	lvremove -ff $vg
}

#
# Main
#
_get_pvs
aux prepare_pvs $npvs $pvsz
get_devs
vgcreate $SHARED -s 1M "$vg" "${DEVICES[@]}"

# Test all respective RAID levels
_test $vg $lv1 raid4 2
_test $vg $lv1 raid5_ra 2
_test $vg $lv1 raid6_nc 3
_test $vg $lv1 raid10 2

vgremove -ff $vg
