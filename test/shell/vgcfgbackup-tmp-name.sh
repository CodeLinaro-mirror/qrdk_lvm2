#!/bin/sh
# A VG name ending in .tmp makes the backup file name unrepresentable,
# which the text format refuses.  The refusal used to leave a metadata
# area with a NULL context behind, and vgcfgbackup then crashed.

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_pvs 1
get_devs

vgcreate $SHARED x.tmp "$dev1"

not vgcfgbackup x.tmp 2>err
grep "may not end in .tmp" err

vgremove -ff x.tmp
