/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
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

struct pvchange_params {
	struct dm_list devs;
	unsigned pv_orphans_only:1;
	unsigned vg_members_only:1;
	unsigned scan_done:1;	/* lvmcache_label_scan was done, process_each_pv can skip it */
	unsigned skip_orphans:1;
	unsigned done;
	unsigned total;
};

static int _pvchange_lock_global(struct cmd_context *cmd, int argc, char **argv,
			         struct pvchange_params *params)
{
	if (!lock_global_devs_is_enabled(cmd)) {
		if (!lockf_global(cmd, "ex"))
			return 0;
		clear_hint_file(cmd);
		return 1;
	}

	/* With --select the args are not devices, so there are no devs to check. */
	if ((!argc || arg_is_set(cmd, select_ARG)) && !arg_is_set(cmd, all_ARG)) {
		log_error("Protecting orphan PVs with PR or the global lock requires explicit PV device arguments for pvchange --uuid.");
		return 0;
	}

	if (arg_is_set(cmd, all_ARG))
		log_warn("WARNING: Skipping orphan PVs for pvchange --uuid --all (orphan PVs are protected with PR or the global lock.)");

	/* The global flock must precede clear_hint_file()'s hints lock. */
	if (!lockf_global(cmd, "ex"))
		return 0;

	clear_hint_file(cmd);

	if (arg_is_set(cmd, all_ARG)) {
		params->skip_orphans = 1;
		return 1;
	}

	/*
	 * A full label scan is done first because lock_global_devs needs the
	 * global mode, which is determined from the shared VGs found by
	 * the scan.  The scan also reads the labels of the device args, which
	 * is used to find the orphan PVs from PV_EXT_USED.  A PV usage
	 * misclassification (the scan runs before the global protection is
	 * held) is caught by the scan in process_each_pv, which repeats the
	 * scan after the orphan PVs are protected, before anything is written.
	 * Reject old pv ext versions which don't include PV_EXT_USED.
	 */

	if (!setup_devices(cmd))
		return_0;

	if (!dev_args_list_create(cmd, argc, argv, &params->devs))
		return 0;

	if (!dev_args_filter_nodata(cmd, &params->devs))
		return_0;

	dev_args_filter_clear(cmd, &params->devs);

	if (!lvmcache_label_scan(cmd))
		return_0;

	if (!dev_args_filter_read(cmd, &params->devs))
		return_0;

	if (dev_args_old_ext_version(&params->devs))
		return_0;

	/*
	 * Changing VG-owned PVs is protected by VG locks, and
	 * changing orphan PVs is protected by global lock or PR.
	 * To simplify, require all devs to be orphans or not, so
	 * that all are protected with PR/GL or all are protected
	 * with VG locks.  (Use separate commands to do them.)
	 */

	if (dev_args_all_vg_members(&params->devs)) {
		params->vg_members_only = 1;
		params->scan_done = 1;
		return 1;
	}

	if (!dev_args_all_pv_orphans(&params->devs))
		return_0;

	if (!lock_global_dev_args(cmd, &params->devs))
		return 0;

	params->pv_orphans_only = 1;
	return 1;
}

static int _pvchange_single(struct cmd_context *cmd, struct volume_group *vg,
			    struct physical_volume *pv, struct processing_handle *handle)
{
	struct pvchange_params *params = (struct pvchange_params *) handle->custom_handle;
	const char *pv_name = pv_dev_name(pv);
	char pvid[ID_LEN + 1]  __attribute__((aligned(8))) = { 0 };
	char uuid[64] __attribute__((aligned(8)));
	struct dev_use *du = NULL;
	unsigned done = 0;
	int used;

	int allocatable = arg_int_value(cmd, allocatable_ARG, 0);
	int mda_ignore = arg_int_value(cmd, metadataignore_ARG, 0);
	int tagargs = arg_is_set(cmd, addtag_ARG) + arg_is_set(cmd, deltag_ARG);

	params->total++;

	if (params->skip_orphans && is_orphan(pv)) {
		log_verbose("Skipping orphan PV %s for pvchange --uuid.", pv_name);
		return ECMD_PROCESSED;
	}

	/* In case state changed after the check above and before rereading. */
	if (params->pv_orphans_only && !is_orphan(pv)) {
		log_error("PV %s is no longer an orphan.", pv_name);
		return_ECMD_FAILED;
	}

	if (params->vg_members_only && is_orphan(pv)) {
		log_error("PV %s is no longer used by a VG.", pv_name);
		return_ECMD_FAILED;
	}

	/*
	 * The primary location of this check is in vg_write(), but it needs
	 * to be copied here to prevent the pv_write() which is called before
	 * the vg_write().
	 */
	if (vg && lvmcache_has_duplicate_devs() && vg_has_duplicate_pvs(vg)) {
	    	if (!find_config_tree_bool(vg->cmd, devices_allow_changes_with_duplicate_pvs_CFG, NULL)) {
			log_error("Cannot update volume group %s with duplicate PV devices.",
				  vg->name);
			log_duplicate_pv_remedy();
			goto bad;
		}
		if (arg_is_set(cmd, uuid_ARG)) {
			log_duplicate_pv_remedy();
			goto bad;
		}
	}

	/* If in a VG, must change using volume group. */
	if (vg && !is_orphan(pv)) {
		if (tagargs && !(vg->fid->fmt->features & FMT_TAGS)) {
			log_error("Volume group containing %s does not "
				  "support tags", pv_name);
			goto bad;
		}
		if (arg_is_set(cmd, uuid_ARG) && lvs_in_vg_activated(vg)) {
			log_error("Volume group containing %s has active "
				  "logical volumes", pv_name);
			goto bad;
		}
	} else {
		if (tagargs) {
			log_error("Can't change tag on Physical Volume %s not "
				  "in volume group", pv_name);
			goto bad;
		}

		if ((used = is_used_pv(pv)) < 0)
			goto_bad;

		if (used && (arg_force_value(cmd) != DONT_PROMPT_OVERRIDE)) {
			log_error("PV %s is used by a VG but its metadata is missing.", pv_name);
			log_error("Can't change PV '%s' without -ff.", pv_name);
			goto bad;
		}
	}

	if (arg_is_set(cmd, allocatable_ARG)) {
		if (is_orphan(pv) &&
		    !(pv->fmt->features & FMT_ORPHAN_ALLOCATABLE)) {
			log_error("Allocatability not supported by orphan "
				  "%s format PV %s", pv->fmt->name, pv_name);
			goto bad;
		}

		/* change allocatability for a PV */
		if (allocatable && (pv_status(pv) & ALLOCATABLE_PV)) {
			log_warn("Physical volume \"%s\" is already "
				 "allocatable.", pv_name);
		} else if (!allocatable && !(pv_status(pv) & ALLOCATABLE_PV)) {
			log_warn("Physical volume \"%s\" is already "
				 "unallocatable.", pv_name);
		} else if (allocatable) {
			log_verbose("Setting physical volume \"%s\" "
				    "allocatable", pv_name);
			pv->status |= ALLOCATABLE_PV;
			done = 1;
		} else {
			log_verbose("Setting physical volume \"%s\" NOT "
				    "allocatable", pv_name);
			pv->status &= ~ALLOCATABLE_PV;
			done = 1;
		}
	}

	if (is_orphan(pv) && !params->pv_orphans_only) {
		if (!cmd->lockf_global_ex && !lockf_global(cmd, "ex"))
			return_ECMD_FAILED;
	}

	if (is_orphan(pv) && !dev_open_excl(pv->dev))
		goto_bad;

	if (tagargs) {
		/* tag or deltag */
		if (arg_is_set(cmd, addtag_ARG) && !change_tag(cmd, NULL, NULL, pv, addtag_ARG))
			goto_bad;

		if (arg_is_set(cmd, deltag_ARG) && !change_tag(cmd, NULL, NULL, pv, deltag_ARG))
			goto_bad;

		done = 1;
	}

	if (arg_is_set(cmd, metadataignore_ARG)) {
		if (vg && (vg_mda_copies(vg) != VGMETADATACOPIES_UNMANAGED) &&
		    (arg_count(cmd, force_ARG) == PROMPT) &&
		    yes_no_prompt("Override preferred number of copies "
				  "of VG %s metadata? [y/n]: ",
				  pv_vg_name(pv)) == 'n')
			goto_bad;
		if (!pv_change_metadataignore(pv, mda_ignore))
			goto_bad;

		done = 1;
	}

	if (arg_is_set(cmd, uuid_ARG)) {
		/* --uuid: Change PV ID randomly */

		du = get_du_for_pvid(cmd, pv->dev->pvid);

		memcpy(&pv->old_id, &pv->id, sizeof(pv->id));
		if (!id_create(&pv->id)) {
			log_error("Failed to generate new random UUID for %s.",
				  pv_name);
			goto bad;
		}
		if (!id_write_format(&pv->id, uuid, sizeof(uuid)))
			goto_bad;
		log_verbose("Changing uuid of %s to %s.", pv_name, uuid);
		if (!is_orphan(pv) && (!pv_write(cmd, pv, 1))) {
			log_error("pv_write with new uuid failed for %s.", pv_name);
			goto bad;
		}

		done = 1;
	}

	if (!done) {
		log_print_unless_silent("Physical volume %s not changed", pv_name);
		return ECMD_PROCESSED;
	}

	log_verbose("Updating physical volume \"%s\"", pv_name);
	if (vg && !is_orphan(pv)) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg->name);
			goto bad;
		}
		backup(vg);
	} else if (!(pv_write(cmd, pv, 0))) {
		log_error("Failed to store physical volume \"%s\"", pv_name);
		goto bad;
	}

	if (du) {
		char *new_pvid;

		memcpy(pvid, &pv->id.uuid, ID_LEN);
		if (!(new_pvid = strdup_pvid(pvid)))
			log_error("Failed to set pvid for devices file.");
		else {
			free(du->pvid);
			du->pvid = new_pvid;
			if (!device_ids_write(cmd))
				log_warn("Failed to update devices file.");
		}
	}

	log_print_unless_silent("Physical volume \"%s\" changed", pv_name);

	params->done++;
	return ECMD_PROCESSED;

bad:
	log_error("Physical volume %s not changed", pv_name);

	return ECMD_FAILED;
}

int pvchange(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvchange_params params = { 0 };
	struct processing_handle *handle = NULL;
	int ret;

	dm_list_init(&params.devs);

	if (arg_is_set(cmd, uuid_ARG))
		cmd->edit_devices_file = 1;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}

	handle->custom_handle = &params;

	set_pv_notify(cmd);

	if (arg_is_set(cmd, uuid_ARG)) {
		/* --uuid is the only option that can modify orphan PVs and may need global locking */
		if (!_pvchange_lock_global(cmd, argc, argv, &params)) {
			ret = ECMD_FAILED;
			goto out;
		}
	}

	ret = process_each_pv(cmd, argc, argv, NULL, 0,
			      READ_FOR_UPDATE | (params.scan_done ? PROCESS_SKIP_SCAN : 0),
			      handle, _pvchange_single);

	log_print_unless_silent("%u physical volume%s changed / %u physical volume%s not changed.",
				params.done, params.done == 1 ? "" : "s",
				params.total - params.done, (params.total - params.done) == 1 ? "" : "s");

out:
	unlock_global_dev_args(cmd, &params.devs);
	destroy_processing_handle(cmd, handle);
	return ret;
}
