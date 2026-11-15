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

# Exported metadata must not name a PV like an existing LV, as a segment
# stacked on LV 'pv0' would otherwise be read back as placed on the PV.


. lib/inittest --skip-with-lvmpolld

aux prepare_vg 2

lvcreate -an -Zn -l 2 -n $lv1 $vg "$dev1"
lvcreate -an -Zn -l 2 -n pv0 $vg "$dev1"

vgcfgbackup -f back $vg

# Name pv0 is taken by the LV, so PVs are pv1 and pv2
sed -n '/^\tphysical_volumes {/,/^\t}/p' back > pvs
not grep "pv0 {" pvs
grep "pv1 {" pvs
grep "pv2 {" pvs

# Hide LV pv0 and stack $lv1 on top of it
awk '/^\t\tpv0 \{/ { hide = 1 }
     hide && /status = / { sub(/, "VISIBLE"\]/, "]"); hide = 0 }
     { sub(/"pv1", 0$/, "\"pv0\", 0"); print }' back > backnew

vgcfgrestore -f backnew $vg
check lv_field $vg/$lv1 devices "pv0(0)"
check lv_field $vg/pv0 devices "$dev1(2)" -a

# Mapping is preserved across a metadata update
vgchange --addtag test $vg
check lv_field $vg/$lv1 devices "pv0(0)"
check lv_field $vg/pv0 devices "$dev1(2)" -a

# and across backup and restore
vgcfgbackup -f back2 $vg
vgcfgrestore -f back2 $vg
check lv_field $vg/$lv1 devices "pv0(0)"
check lv_field $vg/pv0 devices "$dev1(2)" -a
