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

# Removing stripes is a two step conversion: the first lvconvert run
# reshapes and the second one removes the images freed by it.  Adding
# integrity in between reloads the raid table with those images still
# in it, which the raid target rejects once the reshape has finished
# (the superblocks already describe the smaller array).  The conversion
# has to be refused up front instead of leaving the LV broken.

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 15 0 || skip
aux have_integrity 1 5 0 || skip

aux prepare_pvs 7 48

get_devs

vgcreate $SHARED -s 1M "$vg" "${DEVICES[@]}"

# 6 data stripes over 7 images
lvcreate --yes --type raid5_n -i 6 -l 72 -n $lv1 "$vg"
aux wait_for_sync "$vg" "$lv1"

# Start the stripe removal; the images freed by it stay in the raid set
# and are marked to be removed after the reshape.
lvconvert --yes --force --stripes 4 "$vg/$lv1"
check lv_field "$vg/$lv1" segtype "raid5_n"
check lv_field "$vg/$lv1" stripes 7
aux wait_for_sync "$vg" "$lv1" 1
check lv_attr_bit health "$vg/$lv1" "R"

# This used to fail on the table reload and leave the LV in an unknown
# state; it has to be refused before anything is changed.
not lvconvert --yes --raidintegrity y --raidintegritymode journal \
	"$vg/$lv1" >out 2>&1
grep "wait to be removed" out
not grep "reload ioctl" out
not grep "Failed to revert" out

# The refused conversion must not leave any integrity layer behind.
lvs -a -o name,attr,segtype "$vg" >out
cat out
check lv_attr_bit health "$vg/$lv1" "R"
not grep "_imeta" out
not grep "_iorig" out

# Finishing the stripe removal first makes adding integrity work.
lvconvert --yes --force --stripes 4 "$vg/$lv1"
check lv_field "$vg/$lv1" stripes 5
lvconvert --yes --raidintegrity y --raidintegritymode journal "$vg/$lv1"
lvs -a -o name,attr,segtype "$vg" >out
cat out
grep "integrity" out

vgremove -ff "$vg"
