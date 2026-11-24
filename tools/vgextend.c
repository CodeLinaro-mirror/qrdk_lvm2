/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"
#include "lib/device/persist.h"

struct vgextend_params {
	struct pvcreate_params pp;
	struct dm_list devs;
};

static int _restore_pv(struct volume_group *vg, const char *pv_name)
{
	struct pv_list *pvl = NULL;
	pvl = find_pv_in_vg(vg, pv_name);
	if (!pvl) {
		log_warn("WARNING: PV %s not found in VG %s", pv_name, vg->name);
		return 0;
	}

	if (!pvl->pv->dev) {
		log_warn("WARNING: The PV %s is still missing.", pv_name);
		return 0;
	}

	if (pvl->pv->status & MISSING_PV)
		goto clear_flag;

	/*
	 * when the PV has no used PE's vg_read clears the MISSING_PV flag
	 * and sets this so we know.
	 */
	if (pvl->pv->unused_missing_cleared)
		goto clear_flag;

	log_warn("WARNING: PV %s was not missing in VG %s", pv_name, vg->name);
	return 0;

clear_flag:
	pvl->pv->status &= ~MISSING_PV;
	return 1;
}

static int _vgextend_restoremissing_single(struct cmd_context *cmd,
					   const char *vg_name, struct volume_group *vg,
					   struct processing_handle *handle __attribute__((unused)))
{
	int fixed = 0;
	int i;

	for (i = 1; i < cmd->position_argc; i++)
		if (_restore_pv(vg, cmd->position_argv[i]))
			fixed++;

	if (!fixed) {
		log_error("No PV has been restored.");
		return ECMD_FAILED;
	}

	if (!vg_write(vg) || !vg_commit(vg))
		return_ECMD_FAILED;

	log_print_unless_silent("Volume group \"%s\" successfully extended", vg_name);

	return ECMD_PROCESSED;
}

int vgextend_restoremissing(struct cmd_context *cmd, int argc __attribute__((unused)),
			    char **argv __attribute__((unused)))
{
	const char *vg_name;

	vg_name = skip_dev_dir(cmd, cmd->position_argv[0], NULL);

	clear_hint_file(cmd);

	cmd->handles_missing_pvs = 1;

	return process_each_vg(cmd, 0, NULL, vg_name, NULL,
			       READ_FOR_UPDATE, 0, NULL,
			       &_vgextend_restoremissing_single);
}

static int _vgextend_single(struct cmd_context *cmd, const char *vg_name,
			    struct volume_group *vg, struct processing_handle *handle)
{
	struct vgextend_params *vp = (struct vgextend_params *) handle->custom_handle;
	struct pvcreate_params *pp = &vp->pp;
	DM_LIST_INIT(pr_devs);
	uint32_t mda_copies;
	uint32_t mda_used;
	int vg_pr_enabled = (vg->pr & VG_PR_REQUIRE) ? 1 : 0;
	int global_pr_held = (cmd->global_devs.held == GLOBAL_DEVS_PR);
	int ok;
	int ret = ECMD_FAILED;

	if (arg_is_set(cmd, metadataignore_ARG) &&
	    (pp->force == PROMPT) && !pp->yes &&
	    (vg_mda_copies(vg) != VGMETADATACOPIES_UNMANAGED) &&
	    (yes_no_prompt("Override preferred number of copies of VG %s metadata? [y/n]: ", vg_name) == 'n')) {
		log_error("Volume group %s not changed", vg_name);
		return ECMD_FAILED;
	}

	/*
	 * The devs were checked before they were locked (PR), and before the VG
	 * was known.  Now the devs are locked and the VG is locked and read,
	 * so reopen the devs excl and reread them (they stay open excl to the
	 * end), then check them against the VG.  Nothing has been written to the
	 * devs yet, so a failure here leaves them as they were.
	 */
	cmd->filter_deviceid_skip = 1;
	ok = dev_args_label_scan_excl(cmd, &vp->devs);
	cmd->filter_deviceid_skip = 0;
	if (!ok)
		goto_out;

	if (!dev_args_accepted_for_vgextend(cmd, &vp->devs, vg, pp))
		goto_out;

	/*
	 * The VG has PR enabled for regular operation.
	 * If PR was not already acquired on new devs by lock_global_devs,
	 * then start PR on new devs to match the PR state expected by the
	 * VG's PR setting.  This is done before the devs are written so that
	 * the command fails before writing if PR cannot be started.
	 */
	if (vg_pr_enabled && !global_pr_held) {
		if (!dev_args_to_device_list(cmd, &vp->devs, &pr_devs, 1))
			goto_out;
		if (!persist_vgextend_begin(cmd, vg, &pr_devs)) {
			log_error("Failed to start PR on new devices, VG not extended.");
			goto out;
		}
	}

	/*
	 * Write the new PVs (devs that are already orphan PVs are used as they are.)
	 */
	if (!dev_args_wipe_and_zero(cmd, &vp->devs))
		goto_out;

	if (!dev_args_pv_setup_and_zero(cmd, &vp->devs, &pp->pva, &pp->pvs))
		goto_out;

	if (!dev_args_pv_write(cmd, &vp->devs, &pp->pvs))
		goto_out;

	if (!dev_args_pv_orphans_read(cmd, &vp->devs, &pp->pvs))
		goto_out;

	if (!dev_args_update_devices_file(cmd, &vp->devs, &pp->pvs, 0))
		goto_out;

	unlock_devices_file(cmd);

	if (!vg_extend_each_pv(vg, pp))
		goto_out;

	if (arg_is_set(cmd, metadataignore_ARG)) {
		mda_copies = vg_mda_copies(vg);
		mda_used = vg_mda_used_count(vg);

		if ((mda_copies != VGMETADATACOPIES_UNMANAGED) &&
		    (mda_copies != mda_used)) {
			log_warn("WARNING: Changing preferred number of copies of VG %s metadata from %" PRIu32 " to %" PRIu32,
				 vg_name, mda_copies, mda_used);
			vg_set_mda_copies(vg, mda_used);
		}
	}

	log_verbose("Volume group \"%s\" will be extended by %u new physical volumes.",
		    vg_name, dm_list_size(&pp->pvs));

	if (!vg_write(vg) || !vg_commit(vg))
		goto_out;

	/*
	 * The VG has PR enabled for regular operation, and PR was
	 * already acquired on new devs by lock_global_devs.
	 *
	 * Update PR on new devs to match the parameters of the VG,
	 * which may differ from the parameters from lock_global_devs,
	 * e.g. change the PR type to sh for shared VGs, update PR keys
	 * with sanlock generation.
	 *
	 * If the PR update fails, release PR on the new devs - the VG
	 * is extended, but won't allow updates/activation until PR can
	 * be started on the new devs, or the new devs are dropped from
	 * the VG.
	 *
	 * During update, it's safe to release the current PR from the
	 * new devs before acquiring a new PR, because the new devs have
	 * been written, so another command won't try to use them.
	 */
	if (vg_pr_enabled && global_pr_held) {
		if (!dev_args_to_device_list(cmd, &vp->devs, &pr_devs, 1))
			goto_out;
		if (!persist_vgextend_update(cmd, vg, &pr_devs)) {
			log_error("Failed to update PR on new devices, VG is extended with limited functionality.");
			goto out;
		}
	}

	/*
	 * PR acquired in persist_vgextend_begin (and possibly updated
	 * in persist_vgextend_update) are kept by the VG, and should
	 * not be released in persist_vgextend_end.
	 */
	persist_global_devs_hand_off(cmd);

	log_print_unless_silent("Volume group \"%s\" successfully extended", vg_name);
	ret = ECMD_PROCESSED;
out:
	return ret;
}

int vgextend_general(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle = NULL;
	struct vgextend_params vp = { 0 };
	struct pvcreate_params *pp = &vp.pp;
	DM_LIST_INIT(devs);
	const char *vg_name;
	int repeat = 0;
	int ret = ECMD_FAILED;

	vg_name = skip_dev_dir(cmd, argv[0], NULL);
	argc--;
	argv++;

	pvcreate_params_set_defaults(pp);

	if (!pvcreate_params_from_args(cmd, pp))
		return EINVALID_CMD_LINE;

	/* Don't create a new PV on top of an existing PV like pvcreate does. */
	pp->preserve_existing = 1;

	/* pvcreate within vgextend cannot be forced. */
	pp->force = PROMPT;

	dm_list_init(&vp.devs);

	if (!lockf_global(cmd, "ex"))
		return_ECMD_FAILED;

	clear_hint_file(cmd);

	set_pv_notify(cmd);

	cmd->edit_devices_file = 1;

	if (!lvmcache_label_scan(cmd))
		goto_out;

	/* All specified devs must be used or the command fails. */
	if (!dev_args_list_create_unique(cmd, argc, argv, &vp.devs))
		goto_out;

	cmd->use_full_md_check = 1;    /* ensure pvcreate doesn't overwrite md component */
	cmd->filter_deviceid_skip = 1; /* vgextend args are added to the devices file */

	dev_args_filter_clear(cmd, &vp.devs);

	if (!dev_args_filter_nodata(cmd, &vp.devs))
		goto_out;

	dev_args_filter_clear(cmd, &vp.devs);

 repeat_label_scan:
	if (!dev_args_label_scan(cmd, &vp.devs))
		goto_out;

	if (!dev_args_filter_read(cmd, &vp.devs))
		goto_out;

	/* Early checks do not use the vg struct which is not available here. */
	if (!dev_args_accepted_for_vgextend_early(cmd, &vp.devs, pp))
		goto_out;

	/* Repeat checks if things may have changed during user prompt. */
	if (!vgextend_user_prompt(cmd, &vp.devs, &repeat))
		goto_out;

	if (repeat)
		goto repeat_label_scan;

	cmd->use_full_md_check = 0;
	cmd->filter_deviceid_skip = 0;

	if (dev_args_any_failed(&vp.devs))
		goto_out;

	/* This lock can acquire PR on the devs. */
	if (lock_global_devs_is_enabled(cmd) && !lock_global_dev_args(cmd, &vp.devs))
		goto_out;

	if (!(handle = init_processing_handle(cmd, NULL)))
		goto_out;

	handle->custom_handle = &vp;

	/*
	 * The devices file lock (taken exclusively by setup_devices because
	 * edit_devices_file is set) is kept until _vgextend_single has written
	 * the new PVs and device ids, which happens after the VG lock is taken.
	 *
	 * This is not ideal: while the VG lock is being waited for, other
	 * commands block taking the devices file lock (setup_devices, shared),
	 * and a command that holds the VG lock and then blocks taking the
	 * devices file lock (device_id_lvremove, exclusive) would deadlock with
	 * this command.  We may want to improve this later, e.g. by releasing
	 * the lock here and taking it again in _vgextend_single after the VG
	 * lock, failing if device_ids_version_unchanged() reports a change.
	 */

	/*
	 * It is always ok to add new PVs to a VG, even if there are missing
	 * PVs.  No LVs are affected by this operation, but repair processes -
	 * particularly for RAID segtypes - can be facilitated.
	 */
	cmd->handles_missing_pvs = 1;

	ret = process_each_vg(cmd, 0, NULL, vg_name, NULL,
			      READ_FOR_UPDATE | PROCESS_SKIP_SCAN, 0, handle,
			      &_vgextend_single);
out:
	cmd->use_full_md_check = 0;
	cmd->filter_deviceid_skip = 0;

	if (handle)
		destroy_processing_handle(cmd, handle);

	if (!dev_args_to_device_list(cmd, &vp.devs, &devs, 0))
		ret = ECMD_FAILED;
	if (!persist_vgextend_end(cmd, &devs))
		ret = ECMD_FAILED;
	if (!unlock_global_dev_args(cmd, &vp.devs))
		ret = ECMD_FAILED;

	return ret;
}

int vgextend(struct cmd_context *cmd, int argc, char **argv)
{
	log_error("missing command definition");
	return ECMD_FAILED;
}
