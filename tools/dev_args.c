/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2017 Red Hat, Inc. All rights reserved.
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
#include "lib/format_text/format-text.h"
#include "lib/label/hints.h"
#include "lib/device/device_id.h"
#include "lib/device/online.h"
#include "lib/device/persist.h"

/*
 * "dev_args" functions support commands with device args that
 * are not owned by a VG.  A command requires some extra effort
 * to determine if the device/orphan args are actually unused,
 * and can be safely used or written to.  These device args are
 * not protected by a VG lock, since they are not part of a VG,
 * so on a SAN they require some form of global locking/protection,
 * e.g. a global lock or persistent reservation, to protect against
 * multiple hosts using the device at the same time.  lvmlockd uses
 * a global lock or PR depending on its global mode; without lvmlockd,
 * PR is used if lvm.conf persistent_reservations is "full".  (Note:
 * we don't want to take a PR on a device too early, before we know
 * it's safe for us to use it, otherwise that PR may disrupt some
 * other use of the device.  So, as much filtering and checking is
 * done prior to the use of PR as possible.)
 *
 * The commands that deal with device args of this nature are:
 * - pvcreate
 * - pvremove
 * - vgcreate
 * - vgextend
 * - pvchange --uuid of orphan PVs
 * - pvresize of orphan PVs
 *
 * (The last two commands are not frequently used.)
 *
 * A common issue in these commands it that devs need to be read
 * to determine if they are used by a VG.  Then they can be locked
 * or protected appropriately, after which they need to be read
 * again to verify that their state did not change between the first
 * read and the lock.  Reading/scanning devices can be expensive,
 * so commands are carefully structured to minimize how often
 * reading/scanning devices occurs, especially full scans of all
 * devices, which can be very expensive on systems with large numbers
 * of devices. Ideally a command performs only one full scan of all
 * devices.  This is made more challenging by the fact that a PV may
 * not hold VG metadata itself, and only by reading metadata from
 * another PV can we see that it's PVID is referenced in some VG
 * metadata (the PV_EXT_USED flag in the PV header is one way to
 * work around this in some limited scenarios.)
 *
 * Another challenge is that there are two separate uses for PR:
 * 1. PR can be used to protect device args that are not yet used by a VG.
 * 2. A VG can be configured to require PR while the VG is being used.
 * Both cases can arise in vgcreate and vgextend, in which case
 * PR used for 1 needs to be transitioned to 2 ("handed off" from
 * global usage for 1 to VG usage for 2.)  The PR key and mode for
 * 1 and 2 can differ, and can require updating in the hand off.
 */

/*
 * Create a list of struct device_list from a list of device_arg, for functions
 * that take device_list.
 */
int dev_args_to_device_list(struct cmd_context *cmd, struct dm_list *args,
			    struct dm_list *devs, int skip_failed)
{
	struct device_arg *da;

	dm_list_init(devs);

	dm_list_iterate_items(da, args) {
		if (!da->dev)
			continue;
		if (skip_failed && da->failed)
			continue;
		/* allocates struct device_list for dev and adds it to devs */
		if (!device_list_add(cmd->mem, devs, da->dev))
			return_0;
	}

	return 1;
}

static void _dev_args_fail_all(struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs)
		da->failed = 1;
}

void dev_args_filter_clear(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;
		cmd->filter->wipe(cmd, cmd->filter, da->dev, NULL);
	}
}

/* Run only the filters that do not read the device. */
int dev_passes_filter_nodata(struct cmd_context *cmd, struct device *dev)
{
	int ret;

	cmd->filter_nodata_only = 1;
	ret = cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL);
	cmd->filter_nodata_only = 0;

	return ret;
}

int dev_args_filter_nodata(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if (!dev_passes_filter_nodata(cmd, da->dev)) {
			log_error("Cannot use %s: %s", da->name, devname_error_reason(da->name));
			da->failed = 1;
			ret = 0;
		}
	}

	return ret;
}

/*
 * Check the read and filter results after the devs have been scanned or read.
 * The filters use existing bcache data, and read the dev if there is none.
 */
int dev_args_filter_read(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if (da->dev->flags & DEV_SCAN_NOT_READ) {
			log_error("Failed to read device %s.", da->name);
			da->failed = 1;
			ret = 0;
		} else if (!cmd->filter->passes_filter(cmd, cmd->filter, da->dev, NULL)) {
			log_error("Cannot use %s: %s", da->name, devname_error_reason(da->name));
			da->failed = 1;
			ret = 0;
		}
	}

	return ret;
}

/*
 * Close and reopen the dev exclusively (O_EXCL|O_RDWR), which fails if the dev
 * is in use.  This is for the single functions that write one orphan PV,
 * so it takes one dev.
 */
int dev_open_excl(struct device *dev)
{
	if (!label_scan_open_excl(dev)) {
		log_error("Failed to open %s exclusively.", dev_name(dev));
		return 0;
	}

	return 1;
}

/*
 * Scan the devs, which reads the label and the VG metadata and creates lvmcache
 * info for a dev that is a PV.  The bcache data is kept for the filters to use.
 * The filters are checked by dev_args_filter_read after this.  The devs are all
 * scanned (including devs with existing lvmcache info from a prior scan.)
 */
int dev_args_label_scan(struct cmd_context *cmd, struct dm_list *devs)
{
	struct dm_list scan_devs;

	if (!dev_args_to_device_list(cmd, devs, &scan_devs, 1)) {
		_dev_args_fail_all(devs);
		return_0;
	}

	log_debug("Scanning devices (%u).", dm_list_size(&scan_devs));
	if (!label_scan_devs(cmd, cmd->filter, &scan_devs)) {
		_dev_args_fail_all(devs);
		return_0;
	}

	return 1;
}

static void _log_old_ext_version(struct device *dev)
{
	log_error("PV header on %s is an old version and cannot be used with PR or global lock protection.",
		  dev_name(dev));
	log_error("A VG member is updated by the next VG metadata write; an orphan PV can be recreated with pvcreate -ff.");
}

/*
 * A pv_header extension older than version 2 has no PV_EXT_USED flag, so a
 * PV used by a VG could look like an orphan.  Returns 1 (with an error
 * message) if any dev has a PV label with an old extension.  The devs must
 * have been read by a label scan.
 */
int dev_args_old_ext_version(struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if ((da->dev->flags & DEV_LABEL_PVHDR) &&
		    !(da->dev->flags & DEV_LABEL_PVHDR_V2)) {
			_log_old_ext_version(da->dev);
			return 1;
		}
	}

	return 0;
}

/*
 * Returns 1 if all devs are orphan PVs.  Otherwise logs why and returns 0:
 * a dev with no PV label (found by a label scan), or a dev
 * used by a VG (PV_EXT_USED).  Whether orphan and in-use PVs may be mixed is
 * up to the caller.
 */
int dev_args_all_pv_orphans(struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if (!(da->dev->flags & DEV_LABEL_PVHDR)) {
			log_error("Failed to find PVID for device %s.", da->name);
			return 0;
		}

		if (da->dev->flags & DEV_LABEL_PVHDR_VGUSED) {
			log_error("Device %s is used by a VG and is not an orphan PV.", da->name);
			return 0;
		}
	}

	return 1;
}

/*
 * Returns 1 if all devs are PVs used by a VG (PV_EXT_USED), without logging.
 * This is based on the flags from a label scan, so it does
 * not know the VG, and a dev with an old pv_header extension is not found.
 */
int dev_args_all_vg_members(struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if (!(da->dev->flags & DEV_LABEL_PVHDR) ||
		    !(da->dev->flags & DEV_LABEL_PVHDR_VGUSED))
			return 0;
	}

	return 1;
}

int lock_global_dev_args(struct cmd_context *cmd, struct dm_list *args)
{
	struct dm_list devs;

	/*
	 * Only take PR on the devs that have not failed a check: taking PR on
	 * a dev that failed (e.g. it is in use by a VG) would block writes
	 * from another host for the duration of the command.  Devs that fail
	 * after the lock is taken (the post-protection reread) do get PR
	 * registered and are released by unlock_global_dev_args, which uses
	 * all the devs (stopping PR on a dev without our key is a no-op.)
	 */
	if (!dev_args_to_device_list(cmd, args, &devs, 1))
		return_0;

	if (dm_list_empty(&devs)) {
		log_debug("No devices to protect with global lock or PR.");
		return 1;
	}

	return lock_global_devs(cmd, &devs);
}

int unlock_global_dev_args(struct cmd_context *cmd, struct dm_list *args)
{
	struct dm_list devs;

	if (!dev_args_to_device_list(cmd, args, &devs, 0))
		return_0;

	return unlock_global_devs(cmd, &devs);
}

int dev_args_any_failed(struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			return 1;
	}

	return 0;
}

/* Returns 1 if no dev is left to process (all failed, or no devs.) */
int dev_args_all_failed(struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (!da->failed)
			return 0;
	}

	return 1;
}

/*
 * Drop the data and filter results of the devs, before repeating
 * a scan, so that the repeated scan reads the devs again and filters
 * are evaluated again from the newly read data.
 */
void dev_args_label_drop(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (!da->dev)
			continue;
		cmd->filter->wipe(cmd, cmd->filter, da->dev, NULL);
		label_scan_invalidate(da->dev);
	}
}

/*
 * Close and reopen the devs exclusively, then read and filter them.  The devs
 * stay open excl until dev_args_close() is called (or the command exits.)
 * The filter settings (e.g. filter_deviceid_skip) are set by the command.
 * A dev that cannot be opened or read, or no longer passes the filters,
 * is marked failed.
 */
int dev_args_label_scan_excl(struct cmd_context *cmd, struct dm_list *devs)
{
	struct dm_list scan_devs;
	int ret = 1;

	if (!dev_args_to_device_list(cmd, devs, &scan_devs, 1))
		return_0;

	dev_args_filter_clear(cmd, devs);

	log_debug("Rescanning and filtering devices with exclusive open.");
	if (!label_scan_devs_excl(cmd, cmd->filter, &scan_devs)) {
		log_debug("Failed to rescan devs excl.");
		ret = 0;
	}

	if (!dev_args_filter_read(cmd, devs))
		ret = 0;

	return ret;
}

/*
 * Close the devs, dropping the exclusive open from dev_args_label_scan_excl.
 * The excl open prevents the dev from being opened elsewhere, so it must be
 * dropped before the devs are mapped into a dm device.  (E.g. vgcreate
 * --shared activating the lvmlock LV over a new PV fails with EBUSY in the
 * dm linear target if the PV is still open excl.)  Otherwise the excl open
 * is kept to protect the devs from other programs.
 *
 * The bcache blocks for each dev are dropped (the bcache fd slot is
 * released by the close, so cached blocks cannot be kept.)  Dirty blocks are
 * written back first, but a failed write-back is not reported.  Later reads
 * and writes reopen the dev non-excl.
 */
void dev_args_close(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;

	dm_list_iterate_items(da, devs) {
		if (!da->dev)
			continue;
		label_scan_invalidate(da->dev);
	}
}

/*
 * Wipe signatures, remove LVM labels, and zero the start of each dev (unless
 * --zero n).  Signatures that were not asked about before may be asked about
 * here, which happens while the devs are protected.  Devs with skip_write have
 * nothing to wipe.
 */
int dev_args_wipe_and_zero(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	int zero = arg_int_value(cmd, zero_ARG, 1);
	int yes = arg_count(cmd, yes_ARG);
	force_t force = arg_force_value(cmd);
	int wiped = 0;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->skip_write)
			continue;

		log_verbose("Wiping signatures on %s.", da->name);

		/* The user already agreed to wipe the signatures that were found. */
		if (!wipe_known_signatures(cmd, da->dev, da->name, TYPE_LVM1_MEMBER | TYPE_LVM2_MEMBER,
					   0, yes || da->sigs_approved, force, &wiped)) {
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (!label_remove(da->dev)) {
			log_error("Failed to wipe existing label(s) on %s.", da->name);
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (zero) {
			log_verbose("Zeroing start of device %s.", da->name);

			if (!dev_write_zeros(da->dev, 0, 2048)) {
				log_error("%s not wiped: aborting.", da->name);
				da->failed = 1;
				ret = 0;
				continue;
			}
		}

		if (sigint_caught())
			return_0;
	}

	return ret;
}

/*
 * Remove the LVM label from each dev (pvremove).  Unlike
 * dev_args_wipe_and_zero, other signatures found on the dev are not
 * wiped and the user is not prompted about them, and the start of the
 * dev is not zeroed; pvremove only removes the PV label, as it always
 * has.  (Prompting here would also happen while the dev is protected
 * with PR or the global lock, which the check-before-protect ordering
 * tries to avoid.)
 */
int dev_args_label_remove(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->skip_write)
			continue;

		if (!label_remove(da->dev)) {
			log_error("Failed to wipe existing label(s) on %s.", da->name);
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (sigint_caught())
			return_0;
	}

	return ret;
}

static struct pv_list *_pvs_find_dev(struct dm_list *pvs, struct device *dev)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, pvs) {
		if (pvl->pv->dev == dev)
			return pvl;
	}

	return NULL;
}

/*
 * Update the devices file for the devs and write it.  If remove is set
 * (pvremove) the devs are removed.  Otherwise (the create commands) each dev
 * with a PV in pvs is added with the PV's pvid.
 */
int dev_args_update_devices_file(struct cmd_context *cmd, struct dm_list *devs,
				 struct dm_list *pvs, int remove)
{
	struct device_arg *da;
	struct pv_list *pvl;
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if (remove) {
			device_id_pvremove(cmd, da->dev);
			continue;
		}

		if (!(pvl = _pvs_find_dev(pvs, da->dev)))
			continue;

		/* allow deviceidtype_ARG/deviceid_ARG ? */
		memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);
		if (!device_id_add(cmd, da->dev, pvid, NULL, NULL, 0)) {
			log_error("Failed to add device id for %s.", da->name);
			da->failed = 1;
			ret = 0;
		}
	}

	if (!device_ids_write(cmd))
		stack;

	return ret;
}

/*
 * Set up a new PV for each dev, and add it to the pvs list (struct pv_list).
 *
 * pv_create() sets up the PV, its data area and its metadata areas (mdas) in
 * memory, but it also writes to disk: for each mda (pvmetadatacopies, usually
 * one at the start of the dev and optionally a second at the end),
 * pv_add_metadata_area() zeroes the first 4096 bytes of the mda (the area that
 * will hold the mda header) with dev_write_zeros().  Nothing else is written
 * here.  The PV label and mda headers are written later by dev_args_pv_write.
 *
 * Zeroing the first 2048 bytes of the dev (with --zero y) and wiping signatures
 * is done earlier by dev_args_wipe_and_zero, not here.
 *
 * TODO: investigate if the mda zeroing could be moved to the "wipe and zero"
 * step, or to the pv_write step.
 */
int dev_args_pv_setup_and_zero(struct cmd_context *cmd, struct dm_list *devs,
			       struct pv_create_args *pva, struct dm_list *pvs)
{
	struct device_arg *da;
	struct physical_volume *pv;
	struct pv_list *pvl;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->skip_write)
			continue;

		if (!(pvl = dm_pool_alloc(cmd->mem, sizeof(*pvl)))) {
			log_error("alloc pvl failed.");
			da->failed = 1;
			ret = 0;
			continue;
		}

		log_debug("Creating a new PV on %s.", da->name);

		if (!(pv = pv_create(cmd, da->dev, pva))) {
			log_error("Failed to setup physical volume \"%s\".", da->name);
			da->failed = 1;
			ret = 0;
			continue;
		}

		log_verbose("Set up physical volume for \"%s\" with %" PRIu64
			    " available sectors.", da->name, pv_size(pv));

		pvl->pv = pv;
		dm_list_add(pvs, &pvl->list);
	}

	return ret;
}

/* Write the new PVs set up by dev_args_pv_setup_and_zero. */
int dev_args_pv_write(struct cmd_context *cmd, struct dm_list *devs, struct dm_list *pvs)
{
	struct device_arg *da;
	struct pv_list *pvl;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->skip_write)
			continue;

		if (!(pvl = _pvs_find_dev(pvs, da->dev))) {
			log_error(INTERNAL_ERROR "No PV set up for %s.", da->name);
			da->failed = 1;
			ret = 0;
			continue;
		}

		log_verbose("Writing physical volume data to disk \"%s\".", da->name);

		if (!pv_write(cmd, pvl->pv, 0)) {
			log_error("Failed to write physical volume \"%s\".", da->name);
			dm_list_del(&pvl->list);
			da->failed = 1;
			ret = 0;
			continue;
		}

		log_print_unless_silent("Physical volume \"%s\" successfully created.", da->name);
	}

	return ret;
}

int pvcreate_params_from_args(struct cmd_context *cmd, struct pvcreate_params *pp)
{
	pp->yes = arg_count(cmd, yes_ARG);
	pp->force = (force_t) (int) arg_count(cmd, force_ARG);

	if (arg_int_value(cmd, labelsector_ARG, 0) >= (int64_t)LABEL_SCAN_SECTORS) {
		log_error("labelsector must be less than %llu.",
			  LABEL_SCAN_SECTORS);
		return 0;
	}

	pp->pva.label_sector = arg_int64_value(cmd, labelsector_ARG,
					       DEFAULT_LABELSECTOR);

	if (arg_is_set(cmd, metadataignore_ARG))
		pp->pva.metadataignore = arg_int_value(cmd, metadataignore_ARG,
						   DEFAULT_PVMETADATAIGNORE);
	else
		pp->pva.metadataignore = find_config_tree_bool(cmd, metadata_pvmetadataignore_CFG, NULL);

	if (arg_is_set(cmd, pvmetadatacopies_ARG) &&
	    !arg_int_value(cmd, pvmetadatacopies_ARG, -1) &&
	    pp->pva.metadataignore) {
		log_error("metadataignore only applies to metadatacopies > 0.");
		return 0;
	}

	pp->zero = arg_int_value(cmd, zero_ARG, 1);

	if (arg_sign_value(cmd, dataalignment_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical volume data alignment may not be negative.");
		return 0;
	}
	pp->pva.data_alignment = arg_uint64_value(cmd, dataalignment_ARG, UINT64_C(0));

	if (pp->pva.data_alignment > UINT32_MAX) {
		log_error("Physical volume data alignment is too big.");
		return 0;
	}

	if (arg_sign_value(cmd, dataalignmentoffset_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical volume data alignment offset may not be negative.");
		return 0;
	}
	pp->pva.data_alignment_offset = arg_uint64_value(cmd, dataalignmentoffset_ARG, UINT64_C(0));

	if (pp->pva.data_alignment_offset > UINT32_MAX) {
		log_error("Physical volume data alignment offset is too big.");
		return 0;
	}

	if ((pp->pva.data_alignment + pp->pva.data_alignment_offset) &&
	    (pp->pva.pe_start != PV_PE_START_CALC)) {
		if ((pp->pva.data_alignment ? pp->pva.pe_start % pp->pva.data_alignment : pp->pva.pe_start) != pp->pva.data_alignment_offset) {
			log_warn("WARNING: Ignoring data alignment %s"
				 " incompatible with restored pe_start value %s.",
				 display_size(cmd, pp->pva.data_alignment + pp->pva.data_alignment_offset),
				 display_size(cmd, pp->pva.pe_start));
			pp->pva.data_alignment = 0;
			pp->pva.data_alignment_offset = 0;
		}
	}

	if (arg_sign_value(cmd, metadatasize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Metadata size may not be negative.");
		return 0;
	}

	if (arg_sign_value(cmd, bootloaderareasize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Bootloader area size may not be negative.");
		return 0;
	}

	pp->pva.pvmetadatasize = arg_uint64_value(cmd, metadatasize_ARG, UINT64_C(0));
	if (!pp->pva.pvmetadatasize) {
		pp->pva.pvmetadatasize = find_config_tree_int(cmd, metadata_pvmetadatasize_CFG, NULL);
		if (!pp->pva.pvmetadatasize)
			pp->pva.pvmetadatasize = get_default_pvmetadatasize_sectors();
	}

	pp->pva.pvmetadatacopies = arg_int_value(cmd, pvmetadatacopies_ARG, -1);
	if (pp->pva.pvmetadatacopies < 0)
		pp->pva.pvmetadatacopies = find_config_tree_int(cmd, metadata_pvmetadatacopies_CFG, NULL);

	pp->pva.ba_size = arg_uint64_value(cmd, bootloaderareasize_ARG, pp->pva.ba_size);

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
	if (!strcmp(cmd->name, "vgcreate")) {
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
			if (pp->setpersist_flags & (SETPR_Y | SETPR_REQUIRE | SETPR_AUTOSTART | SETPR_PTPL)) {
				log_error("Persistent reservations are disabled in lvm config (persistent_reservations=none).");
				return 0;
			}
			if (arg_is_set(cmd, persist_ARG)) {
				log_error("Persistent reservations are disabled in lvm config (persistent_reservations=none).");
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

		setpersist_on = (pp->setpersist_flags & (SETPR_Y | SETPR_REQUIRE | SETPR_AUTOSTART)) ? 1 : 0;

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
	}

	return 1;
}

/*
 * What kind of device is this: not a PV, an orphan PV, a PV used in a VG
 * (vgname is set), a PV used in an unknown VG, or an unused duplicate of
 * another device with the same PVID (lvmcache has info for the other dev).
 * Each of these valid states is returned in an arg, and one is set.
 *
 * Returns 0 only for an error: the dev has a label and pvid, but there is no
 * lvmcache info for it and it is not an unused duplicate.  That means the dev
 * was not scanned, so the caller has not met the requirement below.
 *
 * Requires a prior label_scan of the dev, which sets both the label flags
 * on the dev (DEV_LABEL_PVHDR*) and the lvmcache info (VG name, ext flags).
 * A prior label_read (label_read_pvid) is not enough: it sets only the
 * flags, so it can show that the dev is not a PV, but for a dev with a label
 * there is no lvmcache info and this returns 0.
 *
 * The device is not a PV if no label was seen when it was last read
 * (DEV_LABEL_PVHDR is set by every read of the label.)  It is used in an
 * unknown VG if the PV_EXT_USED flag was seen by the last label read
 * (DEV_LABEL_PVHDR_VGUSED) or by label_scan (lvmcache).
 */
static int _dev_get_pv_usage(struct device *dev, const char **vgname,
			     int *is_not_pv, int *is_orphan_pv, int *is_vg_pv,
			     int *is_used_unknown_pv, int *is_unused_dup_dev)
{
	struct lvmcache_info *info;
	const char *name;

	*vgname = NULL;
	*is_not_pv = 0;
	*is_orphan_pv = 0;
	*is_vg_pv = 0;
	*is_used_unknown_pv = 0;
	*is_unused_dup_dev = 0;

	if (!(dev->flags & DEV_LABEL_PVHDR) || !dev->pvid[0]) {
		*is_not_pv = 1;
		return 1;
	}

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
		/* lvmcache has the info for this PVID from a different dev. */
		if (lvmcache_dev_is_unused_duplicate(dev)) {
			*is_unused_dup_dev = 1;
			return 1;
		}
		log_error("Failed to read lvm info for %s PVID %s.", dev_name(dev), dev->pvid);
		return 0;
	}

	name = lvmcache_vgname_from_info(info);

	if (name && !is_orphan_vg(name)) {
		*is_vg_pv = 1;
		*vgname = name;
	} else if ((dev->flags & DEV_LABEL_PVHDR_VGUSED) ||
		   (lvmcache_ext_flags(info) & PV_EXT_USED)) {
		*is_used_unknown_pv = 1;
	} else {
		*is_orphan_pv = 1;
	}

	return 1;
}

/*
 * Save the usage of the dev (from _dev_get_pv_usage) when it is first checked,
 * so that later checks can tell if the dev changed.
 */
static int _device_arg_save_usage(struct cmd_context *cmd, struct device_arg *da,
				  const char *vgname, int is_not_pv, int is_orphan_pv,
				  int is_vg_pv, int is_used_unknown_pv, int is_unused_dup_dev)
{
	da->usage_saved = 1;
	da->is_not_pv = is_not_pv;
	da->is_orphan_pv = is_orphan_pv;
	da->is_vg_pv = is_vg_pv;
	da->is_used_unknown_pv = is_used_unknown_pv;
	da->is_unused_dup_dev = is_unused_dup_dev;

	if (vgname && !(da->vg_name = dm_pool_strdup(cmd->mem, vgname)))
		return_0;

	memset(da->pvid, 0, sizeof(da->pvid));
	if (!is_not_pv)
		memcpy(da->pvid, da->dev->pvid, ID_LEN);

	return 1;
}

/*
 * Returns 1 if the usage, vg name or pvid of the dev is not the same as when
 * it was saved.  Each of the usages is exclusive.
 */
static int _device_arg_usage_changed(struct device_arg *da, const char *vgname,
				     int is_not_pv, int is_orphan_pv, int is_vg_pv,
				     int is_used_unknown_pv, int is_unused_dup_dev)
{
	if (is_vg_pv) {
		if (!da->is_vg_pv) {
			/* Before, it was not in a VG. */
			return 1;
		}

		if (strcmp(da->vg_name, vgname)) {
			/* Before, it was in a different VG. */
			return 1;
		}
	} else if (is_used_unknown_pv) {
		if (!da->is_used_unknown_pv) {
			/* Before, it had a different usage. */
			return 1;
		}
	} else if (is_orphan_pv) {
		if (!da->is_orphan_pv) {
			/* Before, it had a different usage. */
			return 1;
		}
	} else if (is_unused_dup_dev) {
		if (!da->is_unused_dup_dev) {
			/* Before, it had a different usage. */
			return 1;
		}
	} else if (is_not_pv) {
		if (!da->is_not_pv) {
			/* Before, it was a PV. */
			return 1;
		}
	}

	/* A PV is the same PV. */
	if (!is_not_pv && memcmp(da->pvid, da->dev->pvid, ID_LEN))
		return 1;

	return 0;
}

/*
 * Check that the devs can be used by pvremove.  The first call saves the usage
 * of each dev, and later calls fail the command if a dev changed, also with
 * -ff.  A dev that cannot be used is marked failed.
 *
 *   not a PV, or an unused duplicate: an error unless -f (nothing to remove)
 *   orphan PV: ok
 *   PV used by a VG, or by an unknown VG: an error unless -ff, which is
 *     confirmed by pvremove_user_prompt
 */
int dev_args_accepted_for_pvremove(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	force_t force = arg_force_value(cmd);
	const char *vgname;
	int is_not_pv, is_orphan_pv, is_vg_pv, is_used_unknown_pv, is_unused_dup_dev;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		log_debug("Checking %s for pvremove %.32s.",
			  da->name, da->dev->pvid[0] ? da->dev->pvid : "");

		if (!_dev_get_pv_usage(da->dev, &vgname, &is_not_pv, &is_orphan_pv,
				       &is_vg_pv, &is_used_unknown_pv, &is_unused_dup_dev)) {
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (!da->usage_saved) {
			if (!_device_arg_save_usage(cmd, da, vgname, is_not_pv, is_orphan_pv,
						    is_vg_pv, is_used_unknown_pv, is_unused_dup_dev))
				return_0;
		} else if (_device_arg_usage_changed(da, vgname, is_not_pv, is_orphan_pv,
						     is_vg_pv, is_used_unknown_pv, is_unused_dup_dev)) {
			log_error("Cannot use device %s: it changed during preparation.", da->name);
			_dev_args_fail_all(devs);
			return 0;
		}

		/*
		 * An unused duplicate has no lvmcache info, so it is handled
		 * here as not found; pvremove -ff clears duplicates.
		 */
		if (is_not_pv || is_unused_dup_dev) {
			if (!force) {
				log_error("No PV found on device %s.", da->name);
				da->failed = 1;
				ret = 0;
				continue;
			}

			/* There is no PV label to remove from a dev that is not a PV. */
			if (is_not_pv)
				da->skip_write = 1;
			continue;
		}

		if (is_orphan_pv)
			continue;

		/* The dev is used by a VG, or by an unknown VG. */
		if (force != DONT_PROMPT_OVERRIDE) {
			if (is_used_unknown_pv)
				log_error("PV %s is used by a VG but its metadata is missing.", da->name);
			else
				log_error("PV %s is used by VG %s so please use vgreduce first.", da->name, vgname);
			log_error("(If you are certain you need pvremove, then confirm by using --force twice.)");
			log_error("%s: physical volume label not removed.", da->name);
			da->failed = 1;
			ret = 0;
		}
	}

	return ret;
}

/*
 * Ask for confirmation to remove a PV used by a VG (pvremove -ff, unless -y).
 * The global lock is released while waiting for the answers, then reacquired,
 * the devs are dropped (dev_args_label_drop), and *repeat is set so that the
 * command scans and checks the devs again.  The answers are saved, so the
 * second call asks nothing and does not set *repeat.  A dev that is not
 * confirmed is marked failed.
 */
int pvremove_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat)
{
	struct device_arg *da;
	force_t force = arg_force_value(cmd);
	int yes = arg_count(cmd, yes_ARG);
	int ask = 0;

	*repeat = 0;

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->prompt_answered)
			continue;

		/* Without -ff devs used by a VG are not accepted. */
		if (!da->is_vg_pv && !da->is_used_unknown_pv)
			continue;
		if (force != DONT_PROMPT_OVERRIDE)
			continue;

		if (yes) {
			log_warn("WARNING: PV %s is used by VG %s.", da->name,
				 da->vg_name ? da->vg_name : "<unknown>");
			log_warn("WARNING: Wiping physical volume label from %s of volume group \"%s\".",
				 da->name, da->vg_name ? da->vg_name : "<unknown>");
			da->prompt_answered = 1;
		} else {
			ask++;
		}
	}

	if (!ask)
		return 1;

	/*
	 * Prompts require asking the user and may take some time, during which
	 * we don't want to block other commands.
	 */
	(void) lockf_global(cmd, "un");

	dm_list_iterate_items(da, devs) {
		const char *vgname = da->vg_name ? da->vg_name : "<unknown>";

		if (da->failed || da->prompt_answered)
			continue;

		if (!da->is_vg_pv && !da->is_used_unknown_pv)
			continue;

		log_warn("WARNING: PV %s is used by VG %s.", da->name, vgname);

		if (yes_no_prompt("Really WIPE LABELS from physical volume \"%s\" of volume group \"%s\" [y/n]? ",
				  da->name, vgname) == 'n') {
			log_error("%s: physical volume label not removed.", da->name);
			da->failed = 1;
		} else {
			log_warn("WARNING: Wiping physical volume label from %s of volume group \"%s\".",
				 da->name, vgname);
			da->prompt_answered = 1;
		}

		if (sigint_caught())
			return_0;
	}

	if (dev_args_all_failed(devs))
		return 1;

	/*
	 * Reacquire the lock released above, with a non-blocking request to avoid
	 * potential deadlock since this is not the normal locking sequence.
	 */
	if (!lockf_global_nonblock(cmd, "ex")) {
		log_error("Failed to reacquire global lock after prompt.");
		return 0;
	}

	dev_args_label_drop(cmd, devs);
	*repeat = 1;

	return 1;
}

/*
 * Rules for which devs can be used.  Each dev's usage is determined by
 * _dev_get_pv_usage() and the rules are applied by dev_args_accepted_for_<command>()
 * (which reject devs) and <command>_user_prompt() (which asks about the rest,
 * according to -f/-y/-ff).
 *
 *   dev usage              pvcreate            vgcreate/vgextend     pvremove
 *   ---------              --------            -----------------     --------
 *
 *   not a PV               ok                  ok (create PV)        error, unless -f
 *                                                                    (nothing to remove)
 *
 *   orphan PV              ok                  ok (use the PV)       ok
 *
 *   PV in a VG, or PV      error;              error                 error;
 *   in an unknown VG       -ff prompts;                              -ff prompts;
 *                          -ff -y: no prompt                         -ff -y: no prompt
 *
 *   unused duplicate       error               error                 error, unless -f;
 *                                                                    -ff clears it
 *
 *   old pv_header ext      error with PR/GL    error with PR/GL      no check
 *   (version < 2)          protection;         protection
 *                          -ff bypasses
 *
 * vgcreate and vgextend cannot be forced (-ff has no effect.)
 *
 * Rules that -f/-y/-ff do not change: the dev must pass filters, a --uuid must
 * not be in use on another dev, block sizes must be consistent (vgcreate), with
 * PR enabled the dev must support PR (vgcreate), and a dev must be acceptable
 * for the VG (vgextend).
 *
 * Other questions asked by pvcreate_user_prompt (and vgcreate/vgextend):
 * a --setphysicalvolumesize that does not match the dev size (no question with
 * -y, which warns), and signatures that will be wiped (no question with -y or -f.)
 *
 * If a PV is in a VG, and pvcreate or pvremove is run on it:
 *
 * pvcreate|pvremove -f      : fails
 * pvcreate|pvremove -y      : fails
 * pvcreate|pvremove -f -y   : fails
 * pvcreate|pvremove -ff     : get y/n prompt
 * pvcreate|pvremove -ff -y  : succeeds
 *
 * FIXME: there are a lot of various phrasings used depending on the
 * command and specific case.  Find some similar way to phrase these.
 */

/*
 * Check that the devs can be used to create PVs (pvcreate, vgcreate, vgextend),
 * according to pp->force.  The first call saves the usage of each dev, and
 * later calls fail the command if a dev changed, also with -ff.  A dev that
 * cannot be used is marked failed.
 *
 *   not a PV: ok
 *   orphan PV: ok (pp->preserve_existing: the PV is used as it is, and the
 *     write steps skip it)
 *   PV used by a VG, or by an unknown VG: an error unless -ff, which is
 *     confirmed by the *_user_prompt
 *   a dev with duplicates: an error
 *   an old pv_header extension (version < 2), with PR/GL protection: fails the
 *     command unless -ff
 *   --uuid in use by a PV on another dev: an error
 */
static int _dev_args_accepted_for_create(struct cmd_context *cmd, struct dm_list *devs,
					 struct pvcreate_params *pp)
{
	struct device_arg *da;
	struct device *uuid_dev = NULL;
	const char *vgname;
	int is_not_pv, is_orphan_pv, is_vg_pv, is_used_unknown_pv, is_unused_dup_dev;
	int ret = 1;

	/*
	 * An old pv_header extension has no PV_EXT_USED flag, so a PV used
	 * by a VG could look like an orphan, which the protection of
	 * orphan PVs relies on.  -ff bypasses this.
	 */
	if (lock_global_devs_is_enabled(cmd) && (pp->force != DONT_PROMPT_OVERRIDE) && dev_args_old_ext_version(devs)) {
		_dev_args_fail_all(devs);
		return 0;
	}

	/*
	 * If the user specified a uuid for the new PV, check
	 * if a PV on another dev is already using that uuid.
	 */
	if (pp->uuid_str)
		uuid_dev = lvmcache_device_from_pv_id(cmd, &pp->pva.id, NULL);

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		log_debug("Checking %s for %s %.32s.", da->name, command_name(cmd),
			  da->dev->pvid[0] ? da->dev->pvid : "");

		if (!_dev_get_pv_usage(da->dev, &vgname, &is_not_pv, &is_orphan_pv,
				       &is_vg_pv, &is_used_unknown_pv, &is_unused_dup_dev)) {
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (!da->usage_saved) {
			if (!_device_arg_save_usage(cmd, da, vgname, is_not_pv, is_orphan_pv,
						    is_vg_pv, is_used_unknown_pv, is_unused_dup_dev))
				return_0;
		} else if (_device_arg_usage_changed(da, vgname, is_not_pv, is_orphan_pv,
						     is_vg_pv, is_used_unknown_pv, is_unused_dup_dev)) {
			log_error("Cannot use device %s: it changed during preparation.", da->name);
			_dev_args_fail_all(devs);
			return 0;
		}

		if (uuid_dev && (da->dev != uuid_dev)) {
			log_error("UUID %s already in use on \"%s\".", pp->uuid_str, dev_name(uuid_dev));
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (is_not_pv)
			continue;

		/* Don't allow using a device with duplicates. */
		if (lvmcache_pvid_in_unused_duplicates(da->dev->pvid)) {
			log_error("Cannot use device %s with duplicates.", da->name);
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (is_orphan_pv) {
			/* An existing orphan PV is used as it is. */
			if (pp->preserve_existing)
				da->skip_write = 1;
			continue;
		}

		/* The dev is used by a VG, or by an unknown VG. */
		if (pp->force != DONT_PROMPT_OVERRIDE) {
			if (is_used_unknown_pv) {
				log_error("PV %s is used by a VG but its metadata is missing.", da->name);
				log_error("Can't initialize PV '%s' without -ff.", da->name);
			} else if (!strcmp(command_name(cmd), "pvcreate")) {
				log_error("Can't initialize physical volume \"%s\" of volume group \"%s\" without -ff",
					  da->name, vgname);
			} else {
				log_error("Physical volume '%s' is already in volume group '%s'", da->name, vgname);
				log_error("Unable to add physical volume '%s' to volume group '%s'", da->name, vgname);
			}
			log_error("%s: physical volume not initialized.", da->name);
			da->failed = 1;
			ret = 0;
		}
	}

	return ret;
}

int dev_args_accepted_for_pvcreate(struct cmd_context *cmd, struct dm_list *devs,
				   struct pvcreate_params *pp)
{
	return _dev_args_accepted_for_create(cmd, devs, pp);
}

/* Check that the devs have consistent logical block sizes. */
static int _dev_args_check_block_sizes(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	unsigned int physical_block_size, logical_block_size;
	unsigned int prev_pbs = 0, prev_lbs = 0;

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		logical_block_size = 0;
		physical_block_size = 0;

		if (!dev_get_direct_block_sizes(da->dev, &physical_block_size, &logical_block_size)) {
			log_warn("WARNING: Unknown block size for device %s.", da->name);
			continue;
		}

		if (!logical_block_size) {
			log_warn("WARNING: Unknown logical_block_size for device %s.", da->name);
			continue;
		}

		if (!prev_lbs) {
			prev_lbs = logical_block_size;
			prev_pbs = physical_block_size;
			continue;
		}

		if (prev_lbs == logical_block_size) {
			/* Require lbs to match, just warn about unmatching pbs. */
			if (!cmd->allow_mixed_block_sizes && prev_pbs && physical_block_size &&
			    (prev_pbs != physical_block_size))
				log_warn("WARNING: Devices have inconsistent physical block sizes (%u and %u).",
					 prev_pbs, physical_block_size);
			continue;
		}

		if (!cmd->allow_mixed_block_sizes) {
			log_error("Devices have inconsistent logical block sizes (%u and %u).",
				  prev_lbs, logical_block_size);
			log_print("See lvm.conf allow_mixed_block_sizes.");
			return 0;
		}
	}

	return 1;
}

/*
 * When PR will be used, check that the devs can use it, so that the command
 * fails before PR is started.
 */
static int _dev_args_check_pr(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_arg *da;
	char *local_key = (char *)find_config_tree_str(cmd, local_pr_key_CFG, NULL);
	int local_host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);
	int key_count;

	if (!local_key && !local_host_id) {
		log_error("A local pr_key or host_id is required to use PR (see lvmlocal.conf).");
		return 0;
	}

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		if (!dev_allow_pr(cmd, da->dev)) {
			log_error("Persistent reservation not supported for device %s.", dev_name(da->dev));
			return 0;
		}
	}

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		/* find_key is just being used here to test if the dev supports PR commands. */
		if (!dev_find_key(cmd, da->dev, 1, 0, NULL, 0, NULL, 1, &key_count, NULL)) {
			log_error("Failed to access persistent reservation on %s.", da->name);
			return 0;
		}
	}

	return 1;
}

/*
 * vgcreate is all-or-nothing, so a dev that is not accepted fails the command.
 * Besides the checks for creating PVs: the devs must have consistent block
 * sizes, and if the command uses PR (--persist or --setpersist), the devs
 * must support PR.
 */
int dev_args_accepted_for_vgcreate(struct cmd_context *cmd, struct dm_list *devs,
				   struct pvcreate_params *pp)
{
	int setpersist_on = 0;

	if (!_dev_args_accepted_for_create(cmd, devs, pp))
		return 0;

	if (!_dev_args_check_block_sizes(cmd, devs)) {
		_dev_args_fail_all(devs);
		return 0;
	}

	/*
	 * Resolve a setpersist setting that came from persistent_reservations=full:
	 * the VG is created with PR enabled only when all the devs support PR,
	 * otherwise the default is dropped and the VG is created without PR
	 * (an explicit --setpersist always wins over the default, and is validated
	 * by the PR check below.)
	 */
	if (pp->setpersist_from_full) {
		struct device_arg *da;
		int all_pr = 1;

		dm_list_iterate_items(da, devs) {
			if (!da->dev || da->failed)
				continue;
			if (!dev_allow_pr(cmd, da->dev))
				all_pr = 0;
		}

		if (!all_pr) {
			pp->setpersist_flags = 0;
			pp->setpersist_from_full = 0;

			/*
			 * "start" is the only accepted --persist value in vgcreate.
			 * (It's permitted to do 'vgcreate --persist start' to start PR
			 * on the devs, without setting PR in the VG config, e.g. --setpersist.)
			 */
			pp->start_pr = arg_is_set(cmd, persist_ARG);
		}
	}

	setpersist_on = (pp->setpersist_flags & (SETPR_Y | SETPR_REQUIRE | SETPR_AUTOSTART)) ? 1 : 0;

	if ((pp->start_pr || setpersist_on) && !_dev_args_check_pr(cmd, devs)) {
		_dev_args_fail_all(devs);
		return 0;
	}

	return 1;
}

/*
 * Get the PVs of the devs that are already orphan PVs and are used as they
 * are (skip_write), from lvmcache, and add them to pvs (struct pv_list).
 */
int dev_args_pv_orphans_read(struct cmd_context *cmd, struct dm_list *devs, struct dm_list *pvs)
{
	DM_LIST_INIT(orphan_pvs);
	struct device_arg *da;
	struct volume_group *orphan_vg;
	struct pv_list *pvl, *vgpvl;
	int found;
	int ret = 1;

	dm_list_iterate_items(da, devs) {
		if (!da->failed && da->skip_write && da->is_orphan_pv)
			break;
	}
	if (&da->list == devs)
		return 1;

	log_debug("Using existing orphan PVs in %s.", FMT_TEXT_ORPHAN_VG_NAME);

	if (!(orphan_vg = vg_read_orphans(cmd, FMT_TEXT_ORPHAN_VG_NAME))) {
		log_error("Cannot read orphans VG %s.", FMT_TEXT_ORPHAN_VG_NAME);
		_dev_args_fail_all(devs);
		return 0;
	}

	dm_list_iterate_items(da, devs) {
		if (da->failed || !da->skip_write || !da->is_orphan_pv)
			continue;

		found = 0;
		dm_list_iterate_items(vgpvl, &orphan_vg->pvs) {
			if (vgpvl->pv->dev == da->dev) {
				found = 1;
				break;
			}
		}

		if (!found) {
			log_error("Failed to find PV %s", da->name);
			da->failed = 1;
			ret = 0;
			continue;
		}

		if (!(pvl = dm_pool_alloc(cmd->mem, sizeof(*pvl)))) {
			log_error("alloc pvl failed.");
			da->failed = 1;
			ret = 0;
			continue;
		}

		log_debug("Using existing orphan PV %s.", pv_dev_name(vgpvl->pv));
		pvl->pv = vgpvl->pv;
		dm_list_add(&orphan_pvs, &pvl->list);
	}

	/*
	 * Keep the existing order of the PVs in the VG: the existing orphan PVs
	 * come before the new PVs (which dev_args_pv_setup_and_zero added already.)
	 */
	dm_list_splice(&orphan_pvs, pvs);
	dm_list_splice(pvs, &orphan_pvs);

	return ret;
}

/*
 * Ask for confirmation (when not prevented by -y or -f) of
 * - initializing a PV used by a VG (pvcreate -ff)
 * - a --setphysicalvolumesize that does not match the device size
 * - wiping signatures found on the dev
 * The global lock is released while waiting for the answers, then reacquired,
 * the devs are dropped (dev_args_label_drop), and *repeat is set so that the
 * command scans and checks the devs again.  The answers are saved, so the
 * second call asks nothing and does not set *repeat.  A dev that is not
 * confirmed is marked failed.
 */
static int _create_user_prompt(struct cmd_context *cmd, struct dm_list *devs,
			       force_t force, int *repeat)
{
	struct device_arg *da;
	struct dev_signature *sig;
	int yes = arg_count(cmd, yes_ARG);
	int set_size = arg_is_set(cmd, setphysicalvolumesize_ARG);
	uint64_t new_size = arg_uint64_value(cmd, setphysicalvolumesize_ARG, UINT64_C(0));
	uint64_t size = 0;
	const char *vgname;
	int need_vg, need_size, need_sigs;
	int ask = 0;
	int no;

	*repeat = 0;

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->prompt_answered)
			continue;

		need_vg = (da->is_vg_pv || da->is_used_unknown_pv) &&
			  (force == DONT_PROMPT_OVERRIDE) && !yes;

		need_size = 0;
		if (set_size) {
			if (!dev_get_size(da->dev, &size)) {
				log_error("Can't get device size of %s.", da->name);
				da->failed = 1;
				continue;
			}
			if (new_size != size) {
				if (yes)
					log_warn("WARNING: Faking size of PV %s. Don't write outside real device.", da->name);
				else
					need_size = 1;
			}
		}

		/* With --yes or --force there is nothing to ask, the wipe step wipes. */
		if (!yes && (force == PROMPT) && !da->sigs_detected) {
			if (!dev_find_known_signatures(cmd, da->dev, da->name, TYPE_LVM1_MEMBER | TYPE_LVM2_MEMBER,
						       0, force, &da->sigs)) {
				da->failed = 1;
				continue;
			}
			da->sigs_detected = 1;
		}
		need_sigs = !dm_list_empty(&da->sigs);

		if (!need_vg && !need_size && !need_sigs)
			da->prompt_answered = 1;
		else
			ask++;
	}

	if (!ask)
		return 1;

	/*
	 * Prompts require asking the user and may take some time, during which
	 * we don't want to block other commands.
	 */
	(void) lockf_global(cmd, "un");

	dm_list_iterate_items(da, devs) {
		if (da->failed || da->prompt_answered)
			continue;

		vgname = da->vg_name ? da->vg_name : "<unknown>";
		no = 0;

		if ((da->is_vg_pv || da->is_used_unknown_pv) && (force == DONT_PROMPT_OVERRIDE)) {
			if (yes_no_prompt("Really INITIALIZE physical volume \"%s\" of volume group \"%s\" [y/n]? ",
					  da->name, vgname) == 'n')
				no = 1;
			else
				log_warn("WARNING: Forcing physical volume creation on %s of volume group \"%s\"",
					 da->name, vgname);
		}

		if (!no && set_size) {
			if (!dev_get_size(da->dev, &size)) {
				log_error("Can't get device size of %s.", da->name);
				no = 1;
			} else if (new_size != size) {
				if (yes_no_prompt("WARNING: %s: device size %s does not match requested size %s. Proceed? [y/n]: ",
						  da->name, display_size(cmd, size), display_size(cmd, new_size)) == 'n')
					no = 1;
				else
					log_warn("WARNING: Faking size of PV %s. Don't write outside real device.", da->name);
			}
		}

		if (!no) {
			dm_list_iterate_items(sig, &da->sigs) {
				if (sig->offset) {
					if (yes_no_prompt("WARNING: %s signature detected on %s at offset %s. "
							  "Wipe it? [y/n]: ", sig->type, da->name, sig->offset) == 'n') {
						log_error("Aborted wiping of %s.", sig->type);
						no = 1;
						break;
					}
				} else {
					if (yes_no_prompt("WARNING: %s detected on %s. Wipe it? [y/n]: ",
							  sig->type, da->name) == 'n') {
						log_error("Aborted wiping of %s.", sig->type);
						no = 1;
						break;
					}
				}
			}

			if (!no && !dm_list_empty(&da->sigs))
				da->sigs_approved = 1;
		}

		if (no) {
			log_error("%s: physical volume not initialized.", da->name);
			da->failed = 1;
		} else {
			da->prompt_answered = 1;
		}

		if (sigint_caught())
			return_0;
	}

	if (dev_args_all_failed(devs))
		return 1;

	/*
	 * Reacquire the lock released above, with a non-blocking request to avoid
	 * potential deadlock since this is not the normal locking sequence.
	 */
	if (!lockf_global_nonblock(cmd, "ex")) {
		log_error("Failed to reacquire global lock after prompt.");
		return 0;
	}

	dev_args_label_drop(cmd, devs);
	*repeat = 1;

	return 1;
}

int pvcreate_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat)
{
	return _create_user_prompt(cmd, devs, arg_force_value(cmd), repeat);
}

int vgcreate_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat)
{
	return _create_user_prompt(cmd, devs, arg_force_value(cmd), repeat);
}

/*
 * vgextend cannot be forced (pp->force is PROMPT).  The checks before the devs
 * are protected do not use the VG, which is not known yet.
 */
int dev_args_accepted_for_vgextend_early(struct cmd_context *cmd, struct dm_list *devs,
					 struct pvcreate_params *pp)
{
	return _dev_args_accepted_for_create(cmd, devs, pp);
}

/*
 * The checks of the devs for extending this VG, after they are protected and
 * reread, using the VG that was just read under the VG lock (nothing has been
 * written to the devs yet.)  The same VG checks that vg_extend_each_pv makes
 * on the new PVs are made here on the devs so that they fail before the devs
 * are wiped or written.
 */
int dev_args_accepted_for_vgextend(struct cmd_context *cmd, struct dm_list *devs,
				   struct volume_group *vg, struct pvcreate_params *pp)
{
	DM_LIST_INIT(new_devs);
	struct device_arg *da;
	struct pv_list *pvl;
	uint32_t new_count = 0;

	if (!_dev_args_accepted_for_create(cmd, devs, pp))
		return 0;

	if (vg_bad_status_bits(vg, RESIZEABLE_VG)) {
		_dev_args_fail_all(devs);
		return 0;
	}

	dm_list_iterate_items(da, devs) {
		if (da->failed)
			continue;

		dm_list_iterate_items(pvl, &vg->pvs) {
			if ((pvl->pv->dev == da->dev) ||
			    ((da->dev->flags & DEV_LABEL_PVHDR) && da->dev->pvid[0] &&
			     !memcmp(&pvl->pv->id.uuid, da->dev->pvid, ID_LEN))) {
				log_error("Physical volume %s is already in volume group %s.",
					  da->name, vg->name);
				_dev_args_fail_all(devs);
				return 0;
			}
		}

		new_count++;
	}

	if (vg->pv_count && vg->max_pv && (vg->pv_count + new_count > vg->max_pv)) {
		log_error("No space for %u new physical volume(s) - volume group %s "
			  "holds max %u physical volume(s).", new_count, vg->name, vg->max_pv);
		_dev_args_fail_all(devs);
		return 0;
	}

	if (!dev_args_to_device_list(cmd, devs, &new_devs, 1)) {
		_dev_args_fail_all(devs);
		return_0;
	}

	if (!vg_check_devs_block_sizes(vg, &new_devs)) {
		_dev_args_fail_all(devs);
		return 0;
	}

	return 1;
}

int vgextend_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat)
{
	/* vgextend cannot be forced. */
	return _create_user_prompt(cmd, devs, PROMPT, repeat);
}
