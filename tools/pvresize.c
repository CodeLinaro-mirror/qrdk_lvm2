/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2005 Zak Kipling. All rights reserved.
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

struct pvresize_params {
	uint64_t new_size;
	struct dm_list devs;
	unsigned pv_orphans_only:1;
	unsigned vg_members_only:1;
	unsigned scan_done:1;	/* lvmcache_label_scan was done, process_each_pv can skip it */

	unsigned done;
	unsigned total;
};

static int _pvresize_lock_global(struct cmd_context *cmd, int argc, char **argv,
				 struct pvresize_params *params)
{
	if (!lock_global_devs_is_enabled(cmd))
		return lockf_global(cmd, "ex");

	if (!lockf_global(cmd, "ex"))
		return 0;

	/*
	 * PR or GL are only needed for orphan PVs, but we must read the
	 * devices to check if they are orphans.  A full label scan is done
	 * first because lock_global_devs needs the global mode, which is
	 * determined from the shared VGs found by the scan.  The scan also
	 * reads the labels of the device args, which is used to find the
	 * orphan PVs from PV_EXT_USED.  A PV usage misclassification (the
	 * scan runs before the global protection is held) is caught by the
	 * scan in process_each_pv, which repeats the scan after the orphan
	 * PVs are protected, before anything is written.
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

	/* Resizing VG-owned PVs is protected by the VG lock. */
	if (dev_args_all_vg_members(&params->devs)) {
		params->vg_members_only = 1;
		params->scan_done = 1;
		return 1;
	}

	/*
	 * To simplify, require all devs to be orphans or not, so
	 * that all are protected with PR/GL or all are protected
	 * with VG locks.  (Use separate commands to do them.)
	 */
	if (!dev_args_all_pv_orphans(&params->devs))
		return_0;

	if (!lock_global_dev_args(cmd, &params->devs))
		return 0;

	params->pv_orphans_only = 1;
	return 1;
}

static int _pvresize_single(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct physical_volume *pv,
			    struct processing_handle *handle)
{
	struct pvresize_params *params = (struct pvresize_params *) handle->custom_handle;

	if (!params) {
		log_error(INTERNAL_ERROR "Invalid resize params.");
		return ECMD_FAILED;
	}
	params->total++;

	/* In case state changed after the check above and before rereading. */
	if (params->pv_orphans_only && !is_orphan(pv)) {
		log_error("PV %s is no longer an orphan.", pv_dev_name(pv));
		return_ECMD_FAILED;
	}

	if (params->vg_members_only && is_orphan(pv)) {
		log_error("PV %s is no longer used by a VG.", pv_dev_name(pv));
		return_ECMD_FAILED;
	}

	if (is_orphan(pv) && !dev_open_excl(pv->dev))
		return_ECMD_FAILED;

	if (!pv_resize_single(cmd, vg, pv, params->new_size, arg_count(cmd, yes_ARG)))
		return_ECMD_FAILED;

	params->done++;

	return ECMD_PROCESSED;
}

int pvresize(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvresize_params params = { 0 };
	struct processing_handle *handle = NULL;
	int ret = ECMD_FAILED;

	if (arg_sign_value(cmd, setphysicalvolumesize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical volume size may not be negative");
		return EINVALID_CMD_LINE;
	}

	params.new_size = arg_uint64_value(cmd, setphysicalvolumesize_ARG, UINT64_C(0));

	dm_list_init(&params.devs);

	if (!_pvresize_lock_global(cmd, argc, argv, &params))
		goto out;

	set_pv_notify(cmd);

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		goto out;
	}

	handle->custom_handle = &params;

	ret = process_each_pv(cmd, argc, argv, NULL, 0,
			      READ_FOR_UPDATE | (params.scan_done ? PROCESS_SKIP_SCAN : 0),
			      handle, _pvresize_single);

	log_print_unless_silent("%u physical volume(s) resized or updated / %u physical volume(s) "
				"not resized", params.done, params.total - params.done);
out:
	unlock_global_dev_args(cmd, &params.devs);
	destroy_processing_handle(cmd, handle);
	return ret;
}
