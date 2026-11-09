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

/*
 * Initial sanity checking of recovery-related command-line arguments.
 * These args are: --restorefile, --uuid, and --physicalvolumesize
 *
 * Output arguments:
 * pp: structure allocated by caller, fields written / validated here
 */
static int _pvcreate_restore_params_from_args(struct cmd_context *cmd, int argc,
					      struct pvcreate_params *pp)
{
	pp->restorefile = arg_str_value(cmd, restorefile_ARG, NULL);

	if (arg_is_set(cmd, restorefile_ARG) && !arg_is_set(cmd, uuidstr_ARG)) {
		log_error("--uuid is required with --restorefile");
		return 0;
	}

	if (!arg_is_set(cmd, restorefile_ARG) && arg_is_set(cmd, uuidstr_ARG)) {
		if (!arg_is_set(cmd, norestorefile_ARG) &&
		    find_config_tree_bool(cmd, devices_require_restorefile_with_uuid_CFG, NULL)) {
			log_error("--restorefile is required with --uuid");
			return 0;
		}
	}

	if (arg_is_set(cmd, uuidstr_ARG) && argc != 1) {
		log_error("Can only set uuid on one volume at once");
		return 0;
	}

	if (arg_is_set(cmd, uuidstr_ARG)) {
		pp->uuid_str = arg_str_value(cmd, uuidstr_ARG, "");
		if (!id_read_format(&pp->pva.id, pp->uuid_str))
			return 0;
		pp->pva.idp = &pp->pva.id;
	}

	if (arg_sign_value(cmd, setphysicalvolumesize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical volume size may not be negative");
		return 0;
	}
	pp->pva.size = arg_uint64_value(cmd, setphysicalvolumesize_ARG, UINT64_C(0));

	if (arg_is_set(cmd, restorefile_ARG) || arg_is_set(cmd, uuidstr_ARG))
		pp->zero = 0;
	return 1;
}

static int _pvcreate_restore_params_from_backup(struct cmd_context *cmd,
					       struct pvcreate_params *pp)
{
	struct volume_group *vg;
	struct pv_list *existing_pvl;

	/*
	 * When restoring a PV, params need to be read from a backup file.
	 */
	if (!pp->restorefile)
		return 1;

	if (!(vg = backup_read_vg(cmd, NULL, pp->restorefile))) {
		log_error("Unable to read volume group from %s", pp->restorefile);
		return 0;
	}

	if (!(existing_pvl = find_pv_in_vg_by_uuid(vg, &pp->pva.id))) {
		release_vg(vg);
		log_error("Can't find uuid %s in backup file %s",
			  pp->uuid_str, pp->restorefile);
		return 0;
	}

	pp->pva.ba_start = pv_ba_start(existing_pvl->pv);
	pp->pva.ba_size = pv_ba_size(existing_pvl->pv);
	pp->pva.pe_start = pv_pe_start(existing_pvl->pv);
	pp->pva.extent_size = pv_pe_size(existing_pvl->pv);
	pp->pva.extent_count = pv_pe_count(existing_pvl->pv);

	release_vg(vg);
	return 1;
}

int pvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvcreate_params pp;
	struct dm_list devs; /* dev args */
	int repeat = 0;
	int ret = ECMD_FAILED;

	/*
	 * Five kinds of pvcreate param values:
	 * 1. defaults
	 * 2. recovery-related command line args
	 * 3. recovery-related args from backup file
	 * 4. normal command line args
	 *    (this also checks some settings from 2 & 3)
	 * 5. argc/argv free args specifying devices
	 */

	pvcreate_params_set_defaults(&pp);

	if (!_pvcreate_restore_params_from_args(cmd, argc, &pp))
		return EINVALID_CMD_LINE;

	if (!_pvcreate_restore_params_from_backup(cmd, &pp))
		return EINVALID_CMD_LINE;

	if (!pvcreate_params_from_args(cmd, &pp))
		return EINVALID_CMD_LINE;

	/*
	 * If --metadatasize was not given with --restorefile, set it to pe_start.
	 * Later code treats this as a maximum size and reduces it to fit.
	 */
	if (!arg_is_set(cmd, metadatasize_ARG) && arg_is_set(cmd, restorefile_ARG))
		pp.pva.pvmetadatasize = pp.pva.pe_start;

	/* FIXME Also needs to check any 2nd metadata area isn't inside the data area! */

	dm_list_init(&devs);

	if (!lockf_global(cmd, "ex"))
		return_ECMD_FAILED;

	clear_hint_file(cmd);

	set_pv_notify(cmd);

	cmd->create_edit_devices_file = 1;

	if (!lvmcache_label_scan(cmd))
		goto out;

	/*
	 * pvcreate can use some of the devs: each of these marks the devs that
	 * fail, and the command continues if any dev is left.
	 */
	if (!dev_args_list_create_available(cmd, argc, argv, &devs))
		goto out;

	cmd->use_full_md_check = 1;    /* ensure pvcreate doesn't overwrite md component */
	cmd->filter_deviceid_skip = 1; /* pvcreate args are added to the devices file */

	dev_args_filter_clear(cmd, &devs);
	dev_args_filter_nodata(cmd, &devs);
	dev_args_filter_clear(cmd, &devs);

 repeat_label_scan:
	dev_args_label_scan(cmd, &devs);
	dev_args_filter_read(cmd, &devs);

	/* Verify devs are appropriate targets for pvcreate. */
	dev_args_accepted_for_pvcreate(cmd, &devs, &pp);

	if (dev_args_all_failed(&devs))
		goto out;

	/* Repeat checks if things may have changed during user prompt. */
	if (!pvcreate_user_prompt(cmd, &devs, &repeat))
		goto out;

	if (repeat)
		goto repeat_label_scan;

	cmd->use_full_md_check = 0;
	cmd->filter_deviceid_skip = 0;

	if (dev_args_all_failed(&devs))
		goto out;

	/* This lock can acquire PR on the devs. */
	if (lock_global_devs_is_enabled(cmd) && !lock_global_dev_args(cmd, &devs))
		goto out;

	/*
	 * Reopen the devs excl and reread them, and verify they are still
	 * accepted (another command may have used a dev.)
	 */
	cmd->filter_deviceid_skip = 1;
	dev_args_label_scan_excl(cmd, &devs);
	cmd->filter_deviceid_skip = 0;

	dev_args_accepted_for_pvcreate(cmd, &devs, &pp);

	if (dev_args_all_failed(&devs))
		goto out;

	/*
	 * Write the new PVs.
	 * If wipe_and_zero is interrupted, stop before writing any PVs.
	 * Skip devs that fail to be wiped/zeroed, and use the remaining.
	 */
	if (!dev_args_wipe_and_zero(cmd, &devs)) {
		if (sigint_caught()) {
			log_error("Interrupted before writing new PVs.");
			goto out;
		}
	}

	dev_args_pv_setup_and_zero(cmd, &devs, &pp.pva, &pp.pvs);
	dev_args_pv_write(cmd, &devs, &pp.pvs);

	/* Add devs to the devices file. */
	dev_args_update_devices_file(cmd, &devs, &pp.pvs, 0);

	if (!dev_args_any_failed(&devs))
		ret = ECMD_PROCESSED;
out:
	cmd->use_full_md_check = 0;
	cmd->filter_deviceid_skip = 0;

	if (!unlock_global_dev_args(cmd, &devs))
		ret = ECMD_FAILED;

	return ret;
}
