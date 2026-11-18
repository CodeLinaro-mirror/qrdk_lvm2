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

test_description="ensure 'vgreduce --removemissing' removes a partial pmspare"


. lib/inittest --skip-with-lvmpolld

aux have_thin 1 0 0 || skip

aux prepare_vg 3

lvcreate --type thin-pool -L 8M --poolmetadatasize 1M -n pool "$vg" "$dev1"

SPARE=$(lvs --noheadings -a -o name "$vg" | grep _pmspare | tr -d ' []')
test -n "$SPARE"

# Move the spare to a separate PV so that PV can be failed independently.
pvmove -n "$vg/$SPARE" "$dev1" "$dev3"

aux disable_dev "$dev3"

# --mirrorsonly only handles mirror images and leaves the spare alone.
not vgreduce --removemissing --mirrorsonly "$vg"
lvs "$vg/$SPARE" >/dev/null

# The spare is internal, so it is removed without a prompt, with a warning.
vgreduce --removemissing "$vg" 2>&1 | grep "disables automatic recovery"

lvs -a -o name,devices "$vg" | not grep unknown
not lvs "$vg/$SPARE"
check lv_field "$vg/pool" lv_name "pool"

aux enable_dev "$dev3"
vgremove -ff "$vg"
