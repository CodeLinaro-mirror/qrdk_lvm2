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

aux prepare_pvs 1
get_devs

vgcreate $SHARED "$vg" "$dev1"
lvcreate -an -l1 -n "$lv1" "$vg"
vgcfgbackup -f metadata "$vg"
vgcfgrestore --test -f metadata "$vg"

# Invalid live LV names must be rejected while importing a backup.
sed "s/^\\([[:space:]]*\\)$lv1 {$/\\1\"$lv1 bad\" {/" metadata > bad_lv
grep "\"$lv1 bad\" {" bad_lv
not vgcfgrestore --test -f bad_lv "$vg" 2>err
grep "Invalid logical volume name" err

# The full VG importer must validate names independently of the scan summary.
sed "s/^$vg {$/\"$vg bad\" {/" metadata > bad_vg
grep "\"$vg bad\" {" bad_vg
not vgcfgrestore --test -f bad_vg "$vg" 2>err
grep "Invalid volume group name in metadata" err

# The PV section name keys vg->pv_names with its full length, so it is
# bounded like the other names imported from metadata.
pv_bad=$(printf 'p%.0129d' 0)
sed "s/^\\([[:space:]]*\\)pv0 {$/\\1$pv_bad {/" metadata > bad_pv
grep "$pv_bad {" bad_pv
not vgcfgrestore --test -f bad_pv "$vg" 2>err
grep "Invalid physical volume section name" err

# Historical LV names and their optional compatibility name field are also
# imported from untrusted metadata. Reuse the valid LV ID from the backup.
lv_uuid=$(sed -n '/^[[:space:]]*id =/ {
s/.*"\([^"]*\)".*/\1/
h
}
${x;p}' metadata)
test -n "$lv_uuid"

sed "/^[[:space:]]*physical_volumes {$/i\\
    historical_logical_volumes {\\
        \"bad name\" {\\
            id = \"$lv_uuid\"\\
        }\\
    }" metadata > bad_history_key
grep "\"bad name\" {" bad_history_key
not vgcfgrestore --test -f bad_history_key "$vg" 2>err
grep "Invalid removed logical volume name" err

sed "/^[[:space:]]*physical_volumes {$/i\\
    historical_logical_volumes {\\
        histname {\\
            id = \"$lv_uuid\"\\
            name = \"bad name\"\\
        }\\
    }" metadata > bad_history_name
grep "name = \"bad name\"" bad_history_name
not vgcfgrestore --test -f bad_history_name "$vg" 2>err
grep "Invalid name for removed logical volume" err

vgremove -ff "$vg"
