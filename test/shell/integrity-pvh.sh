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

aux have_integrity 1 5 0 || skip

aux prepare_devs 3 64
vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

lvcreate --type raid1 -m1 --nosync -n $lv1 -l 8 $vg "$dev1" "$dev2"

# The optional PV list restricts every allocation the command makes.
# Integrity metadata must stay on an image's PVs, so a list without
# any image PV is rejected instead of allocating imeta elsewhere.
not lvconvert -y --raidintegrity y $vg/$lv1 "$dev3" > out 2>&1
grep "No PVs for .* are in the requested allocation PV list" out
lvs -a --noheadings -o lv_name $vg > names
not grep -q "${lv1}_rimage_0_imeta" names
not grep -q "${lv1}_rimage_1_imeta" names

# A list with only image 0's PV is accepted for image 0 and rejected for
# image 1, so the imeta LV already created for image 0 must be reverted
# and the LV left unchanged.
lvs -a --noheadings -o lv_name,segtype,devices $vg > lvs-before
not lvconvert -y --raidintegrity y $vg/$lv1 "$dev1" > out 2>&1
grep "No PVs for $vg/${lv1}_rimage_1 are in the requested allocation PV list" out
lvs -a --noheadings -o lv_name,segtype,devices $vg > lvs-after
diff lvs-before lvs-after
# The failed conversion must not deactivate the LV.
check lv_field $vg/$lv1 lv_active active

# Reserve the last two extents on each image PV, then request only those extents.
# Other free extents must not be used to satisfy the request.
last1=$(($(get pv_field "$dev1" pv_pe_count) - 1))
last2=$(($(get pv_field "$dev2" pv_pe_count) - 1))
first1=$((last1 - 1))
first2=$((last2 - 1))
lvcreate -an -l 2 -n reserved1 $vg "$dev1:$first1-$last1"
lvcreate -an -l 2 -n reserved2 $vg "$dev2:$first2-$last2"
not lvconvert -y --raidintegrity y $vg/$lv1 "$dev1:$first1-$last1" "$dev2:$first2-$last2"
lvs -a --noheadings -o lv_name $vg > names
not grep -q "${lv1}_rimage_0_imeta" names
not grep -q "${lv1}_rimage_1_imeta" names
lvremove -ff $vg/reserved1 $vg/reserved2

# The image PVs and the requested extents are accepted.
lvconvert -y --raidintegrity y $vg/$lv1 "$dev1:$first1-$last1" "$dev2:$first2-$last2"
lvs -a -o+devices,segtype $vg > out
grep "${lv1}_rimage_0_imeta" out | grep -F "$dev1($first1)"
grep "${lv1}_rimage_1_imeta" out | grep -F "$dev2($first2)"

# The PV list only says where to allocate, so removal rejects it.
not lvconvert -y --raidintegrity n $vg/$lv1 "$dev3" > out 2>&1
grep "Cannot specify PVs when removing integrity" out
check lv_field $vg/$lv1 segtype raid1

lvchange -an $vg/$lv1
lvremove -ff $vg/$lv1
vgremove -ff $vg
