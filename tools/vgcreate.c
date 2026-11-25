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

/*
 * Usually, setpersist (writing PR settings in VG metadata)
 * is separate from persist start (starting PR on devices).
 * Both steps can be optionally combined in one command with:
 * --setpersist y --persist start.  So,
 *
 * . vgcreate --setpersist y                   does not start PR
 * . vgcreate --persist start                  does start PR
 * . vgcreate --setpersist y --persist start   does start PR
 * . vgcreate --pr                             does start PR
 *
 * --pr is a shortcut for --setpersist y --persist start (and
 * cannot be combined with either option.)
 *
 * A special case which does not follow this pattern is creating
 * a shared VG, where PR is started even without --persist start
 * when setpersist is enabled:
 *
 * . vgcreate --shared --setpersist y          does start PR
 *
 * This case is different because creating a shared VG includes
 * automatically starting the VG lockspace.  The proper sequence
 * of using a shared VG is first starting PR, then starting the
 * lockspace.  So, to maintain this proper sequence, the vgcreate
 * needs to include an automatic PR start prior to the automatic
 * lock start.
 *
 * Creating a shared VG without setpersist, but PR start is
 * currently disallowed, because it's not clear if this
 * combination would ever be useful.
 *
 * . vgcreate --shared --persist start         disallowed
 */

static int vgcreate_persist_params_from_args(struct cmd_context *cmd, struct pvcreate_params *pp)
{
	int shared_vg = arg_is_set(cmd, shared_ARG);
	int pr_policy;
	int setpersist_on;

	if (arg_is_set(cmd, setpersist_ARG) &&
	    !setpersist_arg_flags(arg_str_value(cmd, setpersist_ARG, NULL), &pp->setpersist_flags))
		return_0;

	if ((pr_policy = persist_policy_get(cmd)) < 0)
		return 0;

	/*
	 * --pr is a shortcut for --setpersist y --persist start: the
	 * command takes a PR on the devs it uses, and the VG is
	 * created with PR enabled and started.
	 */
	if (arg_is_set(cmd, pr_ARG))
		pp->setpersist_flags |= SETPR_Y;

	/*
	 * PR is not permitted with policy none.
	 * (--setpersist n is allowed to clear PR settings)
	 */
	if (pr_policy == PR_POLICY_NONE) {
		if (pp->setpersist_flags & SETPR_POLICY_MASK) {
			log_error(PR_POLICY_NONE_MSG);
			return 0;
		}
		if (arg_is_set(cmd, persist_ARG)) {
			log_error(PR_POLICY_NONE_MSG);
			return 0;
		}
	}

	/*
	 * With PR policy set to "full", a new VG automatically has PR enabled
	 * (assuming the devices all support PR), without the user needing to
	 * include an explicit '--setpersist y' on the command line.
	 * (Unless the user explicitly says no with --setpersist n.)
	 */
	if (!arg_is_set(cmd, setpersist_ARG) && !arg_is_set(cmd, pr_ARG) && (pr_policy == PR_POLICY_FULL)) {
		pp->setpersist_flags = SETPR_Y;
		pp->setpersist_from_full = 1;
	}

	setpersist_on = (pp->setpersist_flags & SETPR_ON_MASK) ? 1 : 0;

	/*
	 * "start" is the only permitted option for --persist in vgcreate,
	 * and --pr is a shortcut that implies --persist start.
	 */
	pp->start_pr = arg_is_set(cmd, persist_ARG) || arg_is_set(cmd, pr_ARG);

	if (!shared_vg && arg_is_set(cmd, locktype_ARG)) {
		int lock_type_num = get_lock_type_from_string(arg_str_value(cmd, locktype_ARG, NULL));
		if (lock_type_num == LOCK_TYPE_SANLOCK || lock_type_num == LOCK_TYPE_DLM)
			shared_vg = 1;
	}

	if (!setpersist_on && shared_vg && pp->start_pr) {
		log_error("A shared VG should include --setpersist y|require to use PR.");
		return 0;
	}

	/*
	 * Automatic PR start for shared VGs because lockstart is automatic in vgcreate,
	 * and PR start happens before lockstart.
	 */
	if (setpersist_on && shared_vg)
		pp->start_pr = 1;

	return 1;
}

int vgcreate(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvcreate_params pp;
	struct vgcreate_params vp_new;
	struct vgcreate_params vp_def;
	struct volume_group *vg = NULL;
	struct vgcreate_lockd_params lockd_params = { 0 };
	DM_LIST_INIT(devs);
	DM_LIST_INIT(pr_devs);
	const char *tag;
	char *vg_name;
	struct arg_value_group_list *current_group;
	int vg_name_locked = 0;
	int repeat = 0;
	int ok;
	int ret = ECMD_FAILED;

	vg_name = argv[0];
	argc--;
	argv++;

	pvcreate_params_set_defaults(&pp);

	if (!pvcreate_params_from_args(cmd, &pp))
		return EINVALID_CMD_LINE;

	if (!vgcreate_persist_params_from_args(cmd, &pp))
		return EINVALID_CMD_LINE;

	pp.force = PROMPT;        /* vgcreate cannot be forced (-ff) to use a PV that is in a VG. */
	pp.preserve_existing = 1; /* Don't create a new PV on top of an existing PV like pvcreate does. */

	if (!vgcreate_params_set_defaults(cmd, &vp_def, NULL))
		return EINVALID_CMD_LINE;
	vp_def.vg_name = vg_name;
	if (!vgcreate_params_set_from_args(cmd, &vp_new, &vp_def, &pp))
		return EINVALID_CMD_LINE;
	if (!vgcreate_params_validate(cmd, &vp_new))
		return EINVALID_CMD_LINE;

	cmd->create_edit_devices_file = 1;
	cmd->include_shared_vgs = 0;

	if (!lockf_global(cmd, "ex"))
		return_ECMD_FAILED;

	clear_hint_file(cmd);

	set_pv_notify(cmd);

	if (!lock_vol(cmd, vp_new.vg_name, LCK_VG_WRITE, NULL)) {
		log_error("Can't get lock for %s.", vp_new.vg_name);
		goto out;
	}
	vg_name_locked = 1;

	if (!lvmcache_label_scan(cmd))
		goto_out;

	if (lvmcache_vginfo_from_vgname(vp_new.vg_name, NULL)) {
		log_error("A volume group called %s already exists.", vp_new.vg_name);
		goto out;
	}

	if (!dev_args_list_create_unique(cmd, argc, argv, &devs))
		goto_out;

	cmd->use_full_md_check = 1;    /* ensure pvcreate doesn't overwrite md component */
	cmd->filter_deviceid_skip = 1; /* vgcreate args are added to the devices file */

	dev_args_filter_clear(cmd, &devs);

	if (!dev_args_filter_nodata(cmd, &devs))
		goto_out;

	dev_args_filter_clear(cmd, &devs);

 repeat_label_scan:
	if (!dev_args_label_scan(cmd, &devs))
		goto_out;

	if (!dev_args_filter_read(cmd, &devs))
		goto_out;

	if (!dev_args_accepted_for_vgcreate(cmd, &devs, &pp))
		goto_out;

	/* Repeat checks if things may have changed during user prompt. */
	if (!vgcreate_user_prompt(cmd, &devs, &repeat))
		goto_out;

	if (repeat)
		goto repeat_label_scan;

	cmd->use_full_md_check = 0;
	cmd->filter_deviceid_skip = 0;

	if (dev_args_any_failed(&devs))
		goto_out;

	if (!lockd_vgcreate_begin(cmd, vp_new.lock_type, &lockd_params))
		goto_out;

	/* This lock can acquire PR on the devs (or GL depending on Global Mode). */
	if (!lock_global_dev_args(cmd, &devs))
		goto_out;

	/* Reopen devs excl, reread, and verify they are still accepted. */
	cmd->filter_deviceid_skip = 1;
	ok = dev_args_label_scan_excl(cmd, &devs);
	cmd->filter_deviceid_skip = 0;
	if (!ok)
		goto_out;

	if (!dev_args_accepted_for_vgcreate(cmd, &devs, &pp))
		goto_out;

	/* Remove any stale key file for this VG name. */
	if (pp.start_pr)
		persist_key_file_remove_name(cmd, vp_new.vg_name);

	/*
	 * When starting PR (--persist start or --setpersist y with shared vg),
	 * do it before writing, but after we are sure we can use the devs.
	 * If lock_global_devs above already started PR, then that PR key and
	 * mode are kept as-is and updated later in persist_vgcreate_update().
	 * PR from persist_vgcreate_begin is released by persist_vgcreate_end
	 * unless the PR was transferred to the VG.
	 */
	if (pp.start_pr && (cmd->global_devs.held != GLOBAL_DEVS_PR)) {
		if (!dev_args_to_device_list(cmd, &devs, &pr_devs, 1))
			goto_out;
		if (!persist_vgcreate_begin(cmd, vp_new.vg_name, pp.setpersist_flags, &pr_devs)) {
			log_error("Failed to start persistent reservation.");
			goto_out;
		}
	}

	/* Write the new PVs (devs that are already orphan PVs are used as they are.) */
	if (!dev_args_wipe_and_zero(cmd, &devs))
		goto_out;

	if (!dev_args_pv_setup_and_zero(cmd, &devs, &pp.pva, &pp.pvs))
		goto_out;

	if (!dev_args_pv_write(cmd, &devs, &pp.pvs))
		goto_out;

	if (!dev_args_pv_orphans_read(cmd, &devs, &pp.pvs))
		goto_out;

	if (!dev_args_update_devices_file(cmd, &devs, &pp.pvs, 0))
		goto_out;

	unlock_devices_file(cmd);

	/*
	 * Any necessary pvcreates are done, now create the VG.
	 * (devs remain open excl)
	 */

	if (!(vg = vg_create(cmd, vp_new.vg_name)))
		goto_out;

	if (vg->fid->fmt->features & FMT_CONFIG_PROFILE)
		vg->profile = vg->cmd->profile_params->global_metadata_profile;

	if (!vg_set_extent_size(vg, vp_new.extent_size) ||
	    !vg_set_max_lv(vg, vp_new.max_lv) ||
	    !vg_set_max_pv(vg, vp_new.max_pv) ||
	    !vg_set_alloc_policy(vg, vp_new.alloc) ||
	    !vg_set_system_id(vg, vp_new.system_id) ||
	    !vg_set_mda_copies(vg, vp_new.vgmetadatacopies) ||
	    !vg_set_persist(vg, pp.setpersist_flags))
		goto_out;

	if (arg_is_set(cmd, setautoactivation_ARG) && !arg_int_value(cmd, setautoactivation_ARG, 1))
		vg->status |= NOAUTOACTIVATE;

	/* attach the pv's */
	if (!vg_extend_each_pv(vg, &pp))
		goto_out;

	if (vp_new.max_lv != vg->max_lv)
		log_warn("WARNING: Setting maxlogicalvolumes to %u (0 means unlimited).",
			 vg->max_lv);

	if (vp_new.max_pv != vg->max_pv)
		log_warn("WARNING: Setting maxphysicalvolumes to %u (0 means unlimited).",
			 vg->max_pv);

	if (arg_is_set(cmd, addtag_ARG)) {
		dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
			if (!grouped_arg_is_set(current_group->arg_values, addtag_ARG))
				continue;

			if (!(tag = grouped_arg_str_value(current_group->arg_values, addtag_ARG, NULL))) {
				log_error("Failed to get tag");
				goto_out;
			}

			if (!vg_change_tag(vg, tag, 1))
				goto_out;
		}
	}

	/* Store VG on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg))
		goto_out;

	/*
	 * The VG is initially written without lock_type set, i.e. it starts as
	 * a local VG.  lockd_init_vg() then writes the VG a second time with
	 * both lock_type and lock_args set.  lockd_init_vg requires the excl
	 * opens to be closed so that dm devs can be created from them (lvmlock.)
	 */
	if (is_lockd_type(vp_new.lock_type))
		dev_args_close(cmd, &devs);

	if (!lockd_init_vg(cmd, vg, vp_new.lock_type, 0,
			   arg_str_value(cmd, setlockargs_ARG, NULL),
			   lockd_params.global_mode, lockd_params.bootstrap)) {
		log_error("Failed to initialize lock args for lock type %s", vp_new.lock_type);
		vg_remove_pvs(vg);
		vg_remove_direct(vg);
		goto_out;
	}

	unlock_vg(cmd, vg, vp_new.vg_name);
	vg_name_locked = 0;

	log_print_unless_silent("Volume group \"%s\" successfully created%s%s",
				vg->name,
				vg->system_id ? " with system ID " : "", vg->system_id ? : "");

	/* Potentially update PR key and mode for continuing VG use. */
	if (!persist_vgcreate_update(cmd, vg, pp.setpersist_flags, pp.start_pr))
		goto_out;

	/* PR is not released if it was handed off to the VG (in update) */
	if (!unlock_global_dev_args(cmd, &devs))
		goto out2;

	/*
	 * Start the VG lockspace because it will likely be used right away.
	 * Optionally wait for the start to complete so the VG can be fully
	 * used after this command completes (otherwise, the VG can only be
	 * read without locks until the lockspace is done starting.)
	 */
	if (vg_is_shared(vg)) {
		if (!lockd_start_vg(cmd, vg, NULL)) {
			log_error("Failed to start locking");
			goto out2;
		}

		if (cmd->lockopt & LOCKOPT_NOWAIT) {
			log_print_unless_silent("Starting locking.  VG is read-only until locks are ready.");
		} else {
			/* It is OK if the user does Ctrl-C to cancel the wait. */
			log_print_unless_silent("Starting locking.  Waiting until locks are ready...");
			lockd_start_wait(cmd);
		}
	}
	ret = ECMD_PROCESSED;
	goto out2;

out:
	cmd->use_full_md_check = 0;
	cmd->filter_deviceid_skip = 0;
	if (vg_name_locked)
		unlock_vg(cmd, NULL, vp_new.vg_name);
	persist_vgcreate_end(cmd, &pr_devs);
	unlock_global_dev_args(cmd, &devs);
out2:
	if (vg)
		release_vg(vg);

	return ret;
}
