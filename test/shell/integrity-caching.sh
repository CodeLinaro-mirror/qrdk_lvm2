#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


. shell/integrity-basic.sh
. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip # too slow test for valgrind
aux have_fs ext4 || skip
which resize2fs || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 9 80

integrity_make_files 4096
integrity_make_rand

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	aux clear_devs "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	integrity_prepare_vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
}

# Push the original files out of the cache so the re-read reaches the raid
# images underneath.
_evict_cache() {
	mkdir "$mnt/new1"
	cp randA "$mnt/new1/randA"
	cp randB "$mnt/new1/randB"
	cp randC "$mnt/new1/randC"
	sync
	du -h "$mnt/new1"
	cp -r "$mnt/new1" "$mnt/new2" || true
	cp -r "$mnt/new1" "$mnt/new3" || true
	cp -r "$mnt/new1" "$mnt/new4" || true
	sync
	du -h "$mnt"
	df -h "$mnt"
}

# Flush file data to the origin while keeping writecache in the read path.
_flush_writecache() {
	sync
	dmsetup message "$vg-$lv1" 0 flush
}

_cached_read_repair() {
	# An empty max-corrupt corrupts every device listed, as this test always
	# did.  The raid6 case passes all five devices, but only the data members
	# holding the file pattern match, so at most nr-2 are written.
	# INTEGRITY_RAID_SEG is the raid origin segment beneath the cache.
	local flush_fn=_evict_cache

	[[ $convert_type != writecache ]] || flush_fn=_flush_writecache
	local INTEGRITY_FLUSH_FN=$flush_fn
	local INTEGRITY_RAID_SEG=$vg/${lv1}_${suffix}

	integrity_test_mkfs_ext4_read_repair "" "$@"
}

# lv1 is a raid+integrity LV
# three variations of caching on lv1:
#
# 1. lvcreate --type cache-pool -n fast -l 4 $vg $dev6
#    lvconvert --type cache --cachepool fast $vg/$lv1
#
# 2. lvcreate --type linear -n fast -l 4 $vg $dev6
#    lvconvert --type cache --cachvol fast $vg/$lv1
#
# 3. lvcreate --type linear -n fast -l 4 $vg $dev6
#    lvconvert --type writecache --cachvol fast $vg/$lv1

do_test() {

# --cachepool | --cachevol
local create_type=$1

# cache | writecache
local convert_type=$2

# --type cache-pool | --type linear
local convert_option=$3

# corig | wcorig
local suffix=$4

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
_cached_read_repair "$dev1"
integrity_not_zero_mismatches $vg/${lv1}_${suffix}_rimage_0
integrity_not_zero_mismatches $vg/${lv1}_${suffix}
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvs -a -o name,size,segtype,devices,sync_percent $vg
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}
_cached_read_repair "$dev1" "$dev2"
integrity_not_zero_mismatches $vg/${lv1}_${suffix}_rimage_0
integrity_not_zero_mismatches $vg/${lv1}_${suffix}
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvconvert --splitcache $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}
_cached_read_repair "$dev1" "$dev2" "$dev3"
lvs -o name,integritymismatches \
	$vg/${lv1}_${suffix}_rimage_0 \
	$vg/${lv1}_${suffix}_rimage_1 \
	$vg/${lv1}_${suffix}_rimage_2
integrity_not_zero_mismatches $vg/${lv1}_${suffix}
lvconvert --splitcache $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing integrity from an active LV

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
integrity_add_more_data_to_mnt
lvconvert --splitcache $vg/$lv1
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding integrity to an active LV

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
# Can only be enabled while raid is top level lv (for now.)
not lvconvert --raidintegrity y $vg/${lv1}_${suffix}
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while inactive

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
umount "$mnt"
lvchange -an $vg/$lv1
# use two new devs for raid extend to ensure redundancy
vgextend $vg "$dev7" "$dev8"
lvs -a -o name,segtype,devices $vg
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8"
lvs -a -o name,segtype,devices $vg
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while active

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
# use two new devs for raid extend to ensure redundancy
vgextend $vg "$dev7" "$dev8"
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8"
lvs -a -o name,size,segtype,devices,sync_percent $vg
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
vgextend $vg "$dev7" "$dev8" "$dev9"
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8" "$dev9"
lvs -a -o name,size,segtype,devices,sync_percent $vg
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding image to raid1

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
# currently only allowed while raid is top level lv
not lvconvert -y -m+1 $vg/${lv1}_${suffix}
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing image from raid1

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvconvert -y -m-1 $vg/${lv1}_${suffix}
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test disallowed operations on raid+integrity

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
not lvconvert -y -m-1 $vg/$lv1
not lvconvert -y -m-1 $vg/${lv1}_${suffix}
not lvconvert --splitmirrors 1 -n tmp -y $vg/$lv1
not lvconvert --splitmirrors 1 -n tmp -y $vg/${lv1}_${suffix}
not lvconvert --splitmirrors 1 --trackchanges -y $vg/$lv1
not lvconvert --splitmirrors 1 --trackchanges -y $vg/${lv1}_${suffix}
not lvreduce -L4M $vg/$lv1
not lvreduce -L4M $vg/${lv1}_${suffix}
not lvcreate -s -n snap -L4M $vg/${lv1}_${suffix}
# plan to enable snap on top level raid+integrity, so then
# snap+writecache+raid+integrity should be allowed.
not lvcreate -s -n snap -L4M $vg/$lv1
lvs -a -o name,size,segtype,devices
not pvmove -n $vg/$lv1 "$dev1"
not pvmove -n $vg/${lv1}_${suffix} "$dev1"
not pvmove "$dev1"
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Repeat many of the tests above using bitmap mode

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
_cached_read_repair "$dev1"
integrity_not_zero_mismatches $vg/${lv1}_${suffix}_rimage_0
integrity_not_zero_mismatches $vg/${lv1}_${suffix}
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y --raidintegritymode bitmap -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}_rimage_3
aux wait_recalc $vg/${lv1}_${suffix}_rimage_4
aux wait_recalc $vg/${lv1}_${suffix}
_cached_read_repair "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -o name,integritymismatches \
	$vg/${lv1}_${suffix}_rimage_0 \
	$vg/${lv1}_${suffix}_rimage_1 \
	$vg/${lv1}_${suffix}_rimage_2 \
	$vg/${lv1}_${suffix}_rimage_3 \
	$vg/${lv1}_${suffix}_rimage_4
integrity_not_zero_mismatches $vg/${lv1}_${suffix}
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvremove $vg/$lv1
vgremove -ff $vg

# remove from active lv
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# add to active lv
_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y --raidintegritymode bitmap $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg
}

do_test cache-pool cache --cachepool corig
do_test linear cache --cachevol corig
do_test linear writecache --cachevol wcorig

# TODO: add do_test() variant that skips adding the cache to lv1.
# This would be equivalent to integrity.sh which could be dropped.
