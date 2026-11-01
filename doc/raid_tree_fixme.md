# RAID integrity image reduction: dtree rename findings

## Follow-up experiment

In a separate worktree for this experiment, based on the pre-documentation
source at `18b14b1b0`, extending the existing
`_lv_adjust_real_uuid()` check to include integrity LVs allowed the
extracted image to use its existing `-real` UUID and be renamed before
the remaining images.  This is different from forcing a `-real` layer
name.  The broad `_add_new_lv_to_dtree()` seed from the original prototype
was not needed in the tests below.  The tree-wide rename prototype remains.

On this prototype, without the pre-flight rename, the targeted RAID1 test
passed with checks for surviving image LV UUIDs and data after a deactivate
and reactivate.  `integrity-misc.sh` also passed, including removal from
each of the three PVs.  The tests do not prove that the generic tree-wide
resolver is safe for other targets, or that rollback and all failure paths
are correct.  The prototype still retains the rollback code and is not a
finished replacement for the current branch's fix.

The earlier claim that the extracted data LV must be retired only after the
top-level resume is therefore not established: in this experiment it was
renamed while retaining its existing device and later removed.  The updated
prototype is preserved in `doc/raid_tree_fixme.diff`; the experiment was
tested in a separate worktree that is not part of the repository.

## Status

The current branch has a working fix based on a pre-flight temporary rename
and a rollback metadata copy.  The scratch prototype removes the pre-flight
and passes the targeted tests, but its failure handling and generic libdm
changes are not ready to replace that fix.  It must not be merged as-is.

At the time of writing:

- Working branch: `dev-zkabelac-iraid-1`.  The commit before the two local
  documentation commits is
  `18b14b1b0` (`WHATS_NEW: update`).
- The working fix commits are `9f2b822ae` (`raid: validate an image
  extraction before mutating the array`) and `dbc6ebad7` (`raid: fix
  integrity image rename when reducing raid image count`).
- The test cleanup commit is `d93fcfe33` (`test: make raid read-repair
  corruption parity-aware`).
- Nothing has been pushed.
- Tested in a separate worktree for the experiment, based on the source at
  `18b14b1b0`.  Its tracked modifications and the untracked
  `test/shell/integrity-onecommit.sh` are captured in the saved diff.

This document and the prototype diff are tracked in local-only commits.
The original one-commit gaps are summarized in the FIXME in
`_raid_prepare_integrity_reduction()` in `lib/metadata/raid_manip.c`
(around line 4030); its proposed post-resume solution has been superseded
by the experiment described here.

## Problem

`lvconvert --yes -m 1` on a 3-image RAID1 LV with integrity removes one
image and shifts the remaining images down:

- image 1 becomes image 0,
- image 2 becomes image 1,
- the integrity metadata and origin sub-LVs follow their parent image,
- the removed image is renamed to `..._extracted` and then removed.

The kernel device names are not renamed atomically with the metadata
update.  The final rename involves a chain of name conflicts:

1. `_extract_image_components()` renames the removed data and metadata
   LVs to `..._extracted` in metadata.  The old kernel devices still have
   the old names.
2. `_shift_and_rename_image_components()` renames `_rimage_1` to
   `_rimage_0`, but the old `_rimage_0` kernel device may still exist.
3. The integrity sub-LVs (`_imeta`, `_iorig`) are children of the image
   data LVs, so their names also shift across parents.

dm-integrity image wrappers keep their kernel names across metadata
renames until the devices are reloaded.  A naive single commit therefore
fails with "name is already in use" or "Device or resource busy".

## Current working solution

The branch uses a two-phase rename:

1. `_raid_copy_lv_for_rollback()` exports and imports a private copy of
   the VG.  The copied LV is the rollback source.
2. `_rename_area_lvs(lv, "_")` renames every rimage and rmeta component
   to `_rimage__N` / `_rmeta__N` (and the integrity sub-LVs to the
   corresponding temporary names).
3. `lv_update_and_reload(lv)` commits those temporary names and reloads
   the top-level LV, releasing the old kernel names.
4. The extraction and final rename happen in memory.
5. `_lv_update_and_reload_list(lv, 0, removal_lvs)` commits the reduced
   array and reloads it.
6. On failure, `_raid_restore_rollback_copy()` restores the original
   names, unless the reduced array was already committed.

`_raid_reduction_committed()` decides whether the reduced array reached
disk.  It rejects a missing `vg->vg_committed` with `INTERNAL_ERROR` and
compares `first_seg(lv_committed(lv))->area_count` with the requested
count.  If the reduced array was committed, the code logs that the
original layout was not restored and does not overwrite the committed
reduction.

The rollback copy exists because the pre-flight commit is already on
disk.  If the final commit fails, the temporary names must be replaced by
the original ones.  The post-commit guard exists because the final
commit can succeed and a later activation or resume can still fail.

## Why a one-commit reduction is hard

Two separate gaps must be closed.

### 1. libdm only orders sibling renames

`_rename_conflict_exists()` in `libdm/libdm-deptree.c` (around line 2041)
walks only `parent->uses`, so it sees name conflicts between siblings
under one parent.  `dm_tree_activate_children()` (around line 2142)
defers a conflicting sibling and retries the same priority level.

That is enough for a normal component rename, but not for the integrity
sub-LVs.  `_rimage_1_imeta` is a child of `_rimage_1`, and
`_rimage_2_imeta` is a child of `_rimage_2`; they are cousins, not
siblings.  Shifting `_rimage_2_imeta` to `_rimage_1_imeta` is a
cross-parent rename chain the sibling-only resolver cannot order.

With only the pre-flight removed, the first failure is:

```
device-mapper: rename ioctl on ..._rimage_2_imeta failed: Device or resource busy
device-mapper: ioctl: Unable to change name on mapped device ..._rimage_2_imeta
  to one that already exists: ..._rimage_1_imeta
Failed to rename ..._rimage_2_imeta to ..._rimage_1_imeta
Problem reactivating logical volume .../LV1.
Image reduction for .../LV1 was committed; the original layout was not restored.
```

A tree-wide resolver is the libdm part of the solution: collect nodes
with a pending `new_name`, add a dependency when one node's new name is
another node's current name, and rename in dependency order before
resuming parents.  True cycles still need temporary names.

The scratch prototype replaces the sibling-only check with:

- `_find_rename_conflict()`: walks the whole dtree from the root and
  returns the node whose current name equals the requested new name.
- `_rename_node_with_deps()`: recursively renames a conflicting node
  first, with a `rename_in_progress` guard to reject cycles.
- `dm_tree_activate_children()`: calls `_rename_node_with_deps()` for
  each child with a pending `new_name`.

The prototype is roughly 90 lines of logic in `libdm/libdm-deptree.c`.
It fixes the cross-parent `_imeta`/`_iorig` chain in the tested case;
its generic activation behavior still needs validation.

### 2. Historical investigation of the extracted data LV

The following describes the earlier failed experiment.  The follow-up
UUID adjustment above shows that post-resume retirement is not the only
possible approach for the tested reduction.

After the resolver fix, the remaining failure is the data LV itself:

```
Cannot rename ..._rimage_1 to ..._rimage_0:
name is already in use by ..._rimage_0
```

The old visible `_rimage_0` device is still active.  It is the removed
image that was renamed to `..._extracted` in metadata.  The generic tree
rename cannot retire it because the new top-level table does not
reference it; the old top-level table does.

The earlier experiment added a seed in `_add_new_lv_to_dtree()` before
`dm_tree_add_new_dev_with_udev_flags()`:

```c
if (!_add_dev_to_dtree(dm, dtree, lv, layer))
	return_0;
```

That experiment made the dtree queue the remaining image renames
(`_rimage_1 -> _rimage_0`, `_rimage_2 -> _rimage_1`, and the
corresponding `_imeta`/`_iorig` renames).  It did not retire the old
extracted data LV.  The follow-up passed without this seed.

The old data LV is available in the rollback metadata copy
(`_raid_copy_lv_for_rollback()` / `rollback_lv`), but `activate_lv()`
only receives the new committed LV.  The activation tree therefore has
no old node for the extracted data LV.

Deactivating the old extracted LV before the top-level resume does not
work either:

```
Deactivating old logical volume .../LV1_rmeta_0 before .../LV1.
Device .../LV1_rmeta_0 (252:3) is used by another device.
Deactivated volume is still .../LV1_rmeta_0 present.
Failed to deactivate .../LV1_rmeta_0 before resuming .../LV1.
...
Cannot rename .../LV1_rimage_1_imeta to .../LV1_rimage_0_imeta:
name is already in use by .../LV1_rimage_0_imeta.
```

The pre-reduction top-level table is still suspended and references the
old extracted devices.  `deactivate_lv()` cannot remove a device in use.

The previously proposed LVM2-level post-resume step was:

1. Resume the top-level LV so the old top-level table is replaced.
2. Deactivate the old extracted LV, now that the top-level table no
   longer references it.
3. Activate the old LV under its public name (or otherwise finish the
   rename sequence).

That post-resume sequence was not implemented.  The follow-up instead
found the old `-real` device and renamed it before the resume.

## UUID and the `-real` layer

Extraction does not create a new UUID.  `_extract_image_components()`
only calls `lv_set_name()` on the same data and metadata LVs:

- old `_rimage_0`: `DM_UUID=...UaFf...-real`
- new `_rimage_0_extracted`: `DM_UUID=...UaFf...`

The base UUID is the same.  The old visible `_rimage_0` name is on the
`-real` layer; the new top-level is the plain UUID.

`_uuid_suffix_list` in `lib/activate/dev_manager.c` (around line 50)
contains `cdata`, `cmeta`, `cvol`, `imeta`, `pool`, `tdata`, `tmeta`,
`vdata`, and `vpool`; it does not contain `real`.  Adding `real` alone
does not fix a plain-UUID lookup: `_info()` and
`dm_tree_find_node_by_uuid()` only strip a suffix from the lookup UUID;
they do not append `-real` to a plain lookup.

The relevant special case is `_lv_adjust_real_uuid()` in
`lib/activate/dev_manager.c` (around line 3710).  It builds the `-real`
UUID and adjusts the lookup UUID when the LV is active as `-real`, but it
returns early for integrity LVs because of this condition:

```c
!lv_is_linear(lv) && !lv_is_striped(lv) && !lv_is_mirrored(lv) &&
!lv_is_error(lv) && !lv_is_zero(lv)
```

The integrity data LV is none of those, so the adjustment never runs.
Forcing the `-real` layer explicitly in `_add_new_lv_to_dtree()` queued
wrong names (`_rimage_1 -> _rimage_0-real`,
`_rimage_0 -> _rimage_0-real`) and caused `lvcreate` reload failures
("reload ioctl ... Invalid argument").  That experiment was reverted.

## Targeted test

The scratch worktree adds `test/shell/integrity-onecommit.sh`:

- 3 PVs, `lvcreate --type raid1 -m 2 --nosync -l 8 --raidintegrity y`,
- wait for recalc on all images,
- `lvconvert --yes -m 1 $vg/$lv1 "$dev1"`,
- on failure, dump `dmsetup ls --tree` and
  `dmsetup info -c -o name,uuid,minor` and die.

The updated test checks the surviving LV UUIDs and integrity segment types,
absence of extracted device names, and data after deactivate/reactivate.
It passes on the follow-up prototype.  Run it with:

```sh
sudo make check_local T=shell/integrity-onecommit.sh
```

The existing `test/shell/integrity-misc.sh` collision test is at
`_test_extraction_name_collision()` (around line 191).  It fabricates
`lv1_rimage__0_extracted` so the post-pre-flight rename collides.  For a
one-commit path it must fabricate `lv1_rimage_0_extracted` instead, and
the seqno expectation must accept a failure before or after the single
commit.  The scratch test already makes those adjustments.

## What was observed

- Removing the pre-flight alone fails on the cross-parent
  `_imeta`/`_iorig` rename chain.
- The prototype resolver orders that chain.  Adding integrity LVs to
  `_lv_adjust_real_uuid()` permits the extracted data device to be renamed
  in the tested reduction, without the broad dtree seed.
- The targeted RAID1 test passes, including LV UUID, cleanup and data
  checks.  `integrity-misc.sh` passes, including removal from each of the
  three PVs and its extraction-name collision case.
- In the earlier experiment, deactivating the old extracted LV before the
  top-level resume failed because the old suspended table referenced it.
- In an earlier post-commit reload failure, the existing
  `_raid_reduction_committed()` guard reported the committed reduction
  and did not restore the original layout over it.

## What is not proven

- Whether the tree-wide resolver preserves the required UUID-prefix and
  activation-priority behavior: it can recursively rename a conflicting
  node before that node's normal checks.  It rejects true cycles rather
  than resolving them with temporary names.
- Failure and recovery behavior, especially after a metadata commit or
  partial kernel rename.  The prototype still retains the rollback copy
  and guard designed around the pre-flight commit.
- The exact number of metadata writes on successful reduction and cleanup;
  the targeted test does not assert it.
- Whether the resolver and the `-real` adjustment are safe across other
  activation paths and stacked targets, including raid, mirror, snapshot,
  cache, thin and integrity.

## Future work

1. Review the current two-phase fix as a possible intermediate solution.
2. Validate the generic tree-wide resolver, its UUID and priority scope,
   and cycle behavior independently across stacked targets.
3. Design and test failure handling and recovery around the reduced-array
   commit and partial kernel rename; determine which rollback pieces can
   actually be removed.
4. Verify metadata write counts and test the `-real` adjustment across
   other LV activation paths.
5. Clean up the prototype, update the FIXME and tests, and decide whether
   to upstream the two-phase fix first or wait for the mature approach.

## Preliminary diff

`doc/raid_tree_fixme.diff` contains the tested experimental prototype:

- the tree-wide libdm rename resolver,
- the integrity-LV `_lv_adjust_real_uuid()` adjustment,
- removal of the pre-flight temporary rename,
- the `integrity-misc.sh` one-commit adjustments,
- the targeted `integrity-onecommit.sh` test.

It excludes the failed deactivation experiment and the earlier broad
dtree seed.  The diff is research material and must not be merged as a
finished fix.

## References

Main branch code:

- `lib/metadata/raid_manip.c`
  - `_raid_prepare_integrity_reduction()` (pre-flight and rollback)
  - `_raid_copy_lv_for_rollback()`
  - `_raid_restore_rollback_copy()`
  - `_raid_reduction_committed()`
  - `_raid_begin_integrity_reduction()`
  - `_raid_commit_integrity_reduction()`
  - `_raid_abort_integrity_reduction()`
  - `_raid_remove_images()`
  - `_extract_image_components()` (around line 3191)
  - `_shift_and_rename_image_components()` (around line 1120)
  - `_lv_update_and_reload_list()` (around line 801)
- `lib/activate/dev_manager.c`
  - `_uuid_suffix_list` (around line 50)
  - `_add_dev_to_dtree()` (around line 2506)
  - `_create_partial_dtree()` (around line 3142)
  - `_lv_adjust_real_uuid()` (around line 3710)
  - `_add_new_lv_to_dtree()` (around line 3756)
  - `_tree_action()` (around line 4175)
- `libdm/libdm-deptree.c`
  - `dm_tree_add_new_dev_with_udev_flags()` (around line 1184)
  - `_rename_conflict_exists()` (around line 2041)
  - `dm_tree_activate_children()` (around line 2142)
- `lib/metadata/metadata.c`
  - `vg_write()` (around line 2937)
  - `vg_commit()` (around line 3156)
  - `lv_committed()` (around line 4314)
- `lib/activate/activate.c`
  - `activate_lv()` (around line 2965)
  - `_lv_activate()` (around line 2637)

Scratch artifacts (temporary, outside the repo, next to the experiment
worktree):

- Worktree: separate experiment worktree, not part of the repository
- Branch: `dev-zkabelac-dtree-rename-exp-20261101-080541`
- Resolver patch: `lvm2-dtree-resolver.patch`
- One-commit patch: `lvm2-dtree-onecommit.patch`
- Test logs: `lvm2-onecommit.out`, `lvm2-onecommit-seed.out`,
  `lvm2-onecommit-deact.out`
- Build log: `lvm2-dtree-build-deact.out`

- Preliminary diff (excludes the failed deactivation experiment):
  `doc/raid_tree_fixme.diff`

The scratch worktree is research material, not a candidate patch.
