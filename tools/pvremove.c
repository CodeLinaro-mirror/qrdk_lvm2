/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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

int pvremove(struct cmd_context *cmd, int argc, char **argv)
{
	DM_LIST_INIT(devs); /* dev args */
	struct device_arg *da;
	force_t force = arg_force_value(cmd);
	int repeat = 0;
	int ret = ECMD_FAILED;

	if (!lockf_global(cmd, "ex")) {
		if (force == DONT_PROMPT_OVERRIDE) /* -ff */
			log_warn("WARNING: Skipping global lock for force.");
		else
			return_ECMD_FAILED;
	}

	clear_hint_file(cmd);

	set_pv_notify(cmd);

	/* setup_devices locks the devices file ex, held until device_ids_write. */
	cmd->edit_devices_file = 1;

	/* When forcibly clearing a PV we don't care about a VG lock. */
	if (force == DONT_PROMPT_OVERRIDE)
		cmd->lockd_vg_disable = 1;

 repeat_label_scan:
	if (!lvmcache_label_scan(cmd))
		goto out;

	/*
	 * Devs that are rejected or cannot be used for some reason are skipped
	 * (da->failed is set), and the remaining devs are processed.
	 *
	 * If scan is repeated, state saved in the prior devs list is reused.
	 */
	if (dm_list_empty(&devs) && !dev_args_list_create_available(cmd, argc, argv, &devs))
		goto out;

	/* The command rejects devs excluded by filters, e.g. not in the devices file. */
	dev_args_filter_read(cmd, &devs);

	/* Verify devs are PVs, are orphans (or -ff). */
	dev_args_accepted_for_pvremove(cmd, &devs);

	if (dev_args_all_failed(&devs))
		goto out;

	/* Repeat checks if things may have changed during user prompt. */
	if (!pvremove_user_prompt(cmd, &devs, &repeat))
		goto out;

	if (repeat)
		goto repeat_label_scan;

	if (dev_args_all_failed(&devs))
		goto out;

	/* This lock can acquire PR on the devs. */
	if (lock_global_devs_is_enabled(cmd) && !lock_global_dev_args(cmd, &devs))
		goto out;

	/*
	 * Reopen the devs excl and reread them, and verify they are still
	 * accepted (a concurrent vgextend/vgcreate may have claimed a dev).
	 */
	dev_args_label_scan_excl(cmd, &devs);

	dev_args_accepted_for_pvremove(cmd, &devs);

	if (dev_args_all_failed(&devs))
		goto out;

	/*
	 * Remove the PV label from each dev.  Other signatures on the devs
	 * are not wiped (the user is not prompted about them), and the start
	 * of the devs is not zeroed; pvremove only removes the PV label.
	 */
	if (!dev_args_label_remove(cmd, &devs)) {
		if (sigint_caught()) {
			log_error("Interrupted while removing labels.");
			goto out;
		}
	}

	dm_list_iterate_items(da, &devs) {
		if (da->failed)
			continue;

		/* pvremove -ff clears an unused duplicate. */
		if (force == DONT_PROMPT_OVERRIDE && da->is_unused_dup_dev)
			lvmcache_del_dev_from_duplicates(da->dev);

		log_print_unless_silent("Labels on physical volume \"%s\" successfully wiped.", da->name);
	}

	/* Clear the erased PVID from the device entry. */
	if (!dev_args_update_devices_file(cmd, &devs, NULL, 1))
		goto out;

	if (!dev_args_any_failed(&devs))
		ret = ECMD_PROCESSED;
out:
	if (!unlock_global_dev_args(cmd, &devs))
		ret = ECMD_FAILED;

	return ret;
}
