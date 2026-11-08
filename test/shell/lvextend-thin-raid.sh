#!/usr/bin/env bash

# Copyright (C) 2016-2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld

aux have_thin 1 0 0 || skip
aux have_raid 1 3 0 || skip

aux prepare_vg 6 600

lvcreate --type raid1 -l2 --nosync -n pool $vg
lvconvert --yes --thinpool $vg/pool "$dev3"

check lv_field $vg/pool seg_size_pe "2"
check lv_field $vg/pool_tdata seg_size_pe "2" -a

lvextend --yes -l+3 $vg/pool

check lv_field $vg/pool seg_size_pe "5"
check lv_field $vg/pool_tdata seg_size_pe "5" -a

lvremove -f $vg

# A resync request left on the hidden data LV of a thin pool by an
# interrupted command is replayed and cleared by activating the pool,
# although the LV named on the command line is not a RAID LV.
lvcreate --type raid1 -m1 -L8 -n tpool $vg "$dev1" "$dev2"
aux wait_for_sync $vg tpool
lvconvert --yes --thinpool $vg/tpool "$dev3"

lvchange -an $vg/tpool
vgcfgbackup -f backup $vg
aux inject_status backup tpool_tdata RESYNC > pending
grep '"RESYNC"' pending
vgcfgrestore --yes --force -f pending $vg
vgcfgbackup -f restored $vg
grep '"RESYNC"' restored
# The report shows the request on the RAID LV that holds it, not on the pool.
check lv_field $vg/tpool_tdata resync_pending "resync pending" -a
check lv_field $vg/tpool resync_pending ""
lvs -a --noheadings -o lv_name -S 'resync_pending=yes' "$vg" > sel
grep -q tpool_tdata sel
not grep -qw tpool sel

lvchange -ay $vg/tpool
# The load served the injected request, so the live table carries the
# one-shot sync argument.  The resynchronization of an already in-sync
# array can finish before the first status poll, so the table is the
# durable evidence that the request was honoured.
dmsetup table "$vg-tpool_tdata" | grep -qw sync ||
	die "the table of $vg-tpool_tdata does not carry the \"sync\" argument"
aux wait_for_sync $vg tpool_tdata
vgcfgbackup -f cleared $vg
not grep '"RESYNC"' cleared
check lv_field $vg/tpool_tdata resync_pending "" -a

# A refresh of the running pool asks the RAID data LV for the live status
# instead of the thin-pool segment: it must not report a type mismatch,
# and it clears the request that its load served.
vgcfgrestore --yes --force -f pending $vg
lvchange -vvvv --refresh $vg/tpool > out 2>&1
not grep 'Expected raid segment type' out
grep "Forcing $vg-tpool_tdata .* table reload" out
not grep "Child $vg-tpool_tdata .* table loaded, forcing reload of parent" out
aux wait_for_sync $vg tpool_tdata
vgcfgbackup -f served $vg
not grep '"RESYNC"' served

# Changing a RAID leg forces an identical RAID table reload.  Its parent
# must also reload so the changed device limits propagate through the RAID.
pvmove -vvvv "$dev1" "$dev4" > out 2>&1
grep "Forcing $vg-tpool_tdata .* table reload" out
grep "Child $vg-tpool_tdata .* table loaded, forcing reload of parent" out
grep "Forcing $vg-tpool .* table reload" out
lvremove -f $vg

# check 'raid10' resize works for pool metadata resize
# https://bugzilla.redhat.com/1075644
lvcreate --type raid10 -m1 -L5 -i3 --nosync -n pool $vg
lvcreate --type raid10 -m1 -L3 -i3 --nosync -n meta $vg
lvconvert --yes --thinpool $vg/pool --poolmetadata $vg/meta

check lv_field $vg/pool_tdata lv_size "6.00m" -a
check lv_field $vg/pool_tmeta lv_size "3.00m" -a

lvextend --yes --poolmetadatasize +1 --size +1 $vg/pool

check lv_field $vg/pool_tdata lv_size "7.50m" -a
check lv_field $vg/pool_tmeta lv_size "4.50m" -a

lvremove -f $vg

# check resize of pool and metadata being a different segtype
# https://bugzilla.redhat.com/1722666
lvcreate -L4 -n pool $vg
lvcreate --type raid1 -m1 -L2 --nosync -n meta $vg
lvconvert --yes --thinpool $vg/pool --poolmetadata $vg/meta
# using big enough pool so resize of pool metadata is enforced
# (and it's using a different segtype)
lvextend -L3G $vg/pool

vgremove -ff $vg
