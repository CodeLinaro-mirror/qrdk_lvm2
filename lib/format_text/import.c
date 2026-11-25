/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2008 Red Hat, Inc. All rights reserved.
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

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/commands/toolcontext.h"
#include "import-export.h"
#include "lib/locking/lvmlockd.h"

/* FIXME Use tidier inclusion method */
static const struct text_vg_version_ops *(_text_vsn_list[2]);

static int _text_import_initialised = 0;

static void _init_text_import(void)
{
	if (_text_import_initialised)
		return;

	_text_vsn_list[0] = text_vg_vsn1_init();
	_text_vsn_list[1] = NULL;
	_text_import_initialised = 1;
}

/*
 * Section filter for the vgsummary parse.  The summary needs the VG fields
 * and the physical_volumes section.  For a sanlock VG it also needs the
 * lvmlock LV in logical_volumes, which holds the lvmlockd global state tags.
 * The VG fields, including lock_type, precede the sections in the metadata
 * text, so lock_type has already been parsed when the sections are reached.
 * LVs other than lvmlock are skipped without creating nodes, as are
 * logical_volumes and the historical LVs for VGs that are not sanlock.
 * Once the wanted sections have been seen, the rest of the VG section is
 * skipped; this does not depend on the order the sections appear in.
 */
struct vgsummary_filter_arg {
	int sanlock;		/* lock_type sanlock seen in the VG fields */
	int seen_pv;		/* physical_volumes section seen */
	int seen_lvmlock;	/* lvmlock LV seen (sanlock VGs) */
};

static enum dm_config_section_action _vgsummary_filter(const struct dm_config_node *parent,
						       const char *name, unsigned depth,
						       void *arg)
{
	struct vgsummary_filter_arg *fa = arg;
	const char *lock_type;

	if (depth == 1) {
		lock_type = dm_config_find_str(parent->child, "lock_type", NULL);
		fa->sanlock = lock_type && !strcmp(lock_type, "sanlock");
	}

	if (depth == 1 && !strcmp(name, "physical_volumes")) {
		fa->seen_pv = 1;
		return DM_CONFIG_SECTION_PARSE;
	}

	if (depth == 2 && !strcmp(parent->key, "logical_volumes")) {
		if (!strcmp(name, LOCKD_SANLOCK_LV_NAME)) {
			fa->seen_lvmlock = 1;
			return DM_CONFIG_SECTION_PARSE;
		}
		return DM_CONFIG_SECTION_SKIP;
	}

	if (depth == 1 && !strcmp(name, "logical_volumes")) {
		if (fa->sanlock)
			return DM_CONFIG_SECTION_PARSE;
		return DM_CONFIG_SECTION_SKIP;
	}

	/* The wanted sections have been seen; skip the rest of the VG section. */
	if (depth == 1 && fa->seen_pv && (!fa->sanlock || fa->seen_lvmlock))
		return DM_CONFIG_SECTION_STOP;

	return DM_CONFIG_SECTION_PARSE;
}

/*
 * Find out vgname on a given device.
 */
int text_read_metadata_summary(const struct format_type *fmt,
		       struct device *dev, dev_io_reason_t reason,
		       off_t offset, uint32_t size,
		       off_t offset2, uint32_t size2,
		       checksum_fn_t checksum_fn,
		       int checksum_only,
		       struct lvmcache_vgsummary *vgsummary)
{
	struct dm_config_tree *cft;
	struct vgsummary_filter_arg fa = { 0 };
	const struct text_vg_version_ops **vsn;
	int r = 0;

	_init_text_import();

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0)))
		return_0;

	if (dev) {
		log_debug_metadata("Reading metadata summary from %s at %llu size %u (+%u).",
				   dev_name(dev), (unsigned long long)offset,
				   size, size2);

		if (!config_file_read_fd(cft, dev, reason, offset, size,
					 offset2, size2, checksum_fn,
					 vgsummary->mda_checksum,
					 checksum_only, 1,
					 _vgsummary_filter, &fa)) {
			log_warn("WARNING: Invalid metadata text from %s at %llu.",
				 dev_name(dev), (unsigned long long)offset);
			goto out;
		}
	} else {
		if (!config_file_read_from_file(cft)) {
			log_warn("WARNING: Invalid metadata text from file.");
			goto out;
		}
	}

	if (checksum_only) {
		/* Checksum matches already-cached content - no need to reparse. */
		log_debug_metadata("Skipped parsing metadata on %s", dev_name(dev));
		r = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(*vsn)->read_vgsummary(fmt, cft, vgsummary))
			goto_out;

		r = 1;
		break;
	}

      out:
	config_destroy(cft);
	return r;
}

struct cached_vg_fmtdata {
        uint32_t cached_mda_checksum;
        size_t cached_mda_size;
};

struct volume_group *text_read_metadata(struct format_instance *fid,
				       const char *file,
				       struct cached_vg_fmtdata **vg_fmtdata,
				       unsigned *use_previous_vg,
				       struct device *dev, int primary_mda,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
				       checksum_fn_t checksum_fn,
				       uint32_t checksum,
				       time_t *when, char **desc)
{
	struct volume_group *vg = NULL;
	struct dm_config_tree *cft;
	const struct text_vg_version_ops **vsn;
	int skip_parse;

	/*
	 * This struct holds the checksum and size of the VG metadata
	 * that was read from a previous device.  When we read the VG
	 * metadata from this device, we can skip parsing it into a
	 * cft (saving time) if the checksum of the metadata buffer
	 * we read from this device matches the size/checksum saved in
	 * the mda_header/rlocn struct on this device, and matches the
	 * size/checksum from the previous device.
	 */
	if (vg_fmtdata && !*vg_fmtdata &&
	    !(*vg_fmtdata = dm_pool_zalloc(fid->mem, sizeof(**vg_fmtdata)))) {
		log_error("Failed to allocate VG fmtdata for text format.");
		return NULL;
	}

	_init_text_import();

	*desc = NULL;
	*when = 0;

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, file, 0)))
		return_NULL;

	/* Does the metadata match the already-cached VG? */
	skip_parse = vg_fmtdata &&
		     ((*vg_fmtdata)->cached_mda_checksum == checksum) &&
		     ((*vg_fmtdata)->cached_mda_size == (size + size2));


	if (dev) {
		log_debug_metadata("Reading metadata from %s at %llu size %u (+%u).",
				   dev_name(dev), (unsigned long long)offset,
				   size, size2);

		if (!config_file_read_fd(cft, dev, MDA_CONTENT_REASON(primary_mda), offset, size,
					 offset2, size2, checksum_fn, checksum,
					 skip_parse, 1, NULL, NULL)) {
			log_warn("WARNING: Couldn't read volume group metadata from %s.", dev_name(dev));
			goto out;
		}
	} else {
		if (!config_file_read_from_file(cft)) {
			log_warn("WARNING: Couldn't read volume group metadata from file.");
			goto out;
		}
	}

	if (skip_parse) {
		if (use_previous_vg)
			*use_previous_vg = 1;
		log_debug_metadata("Skipped parsing metadata on %s", dev_name(dev));
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(vg = (*vsn)->read_vg(fid->fmt->cmd, fid->fmt, fid, cft)))
			goto_out;

		if (!(*vsn)->read_desc(vg->vgmem, cft, when, desc))
			goto_out;
		vg->committed_cft = cft; /* Reuse CFT for recreation of committed VG */
		vg->buffer_size_hint = size + size2;
		cft = NULL;
		break;
	}

	if (vg && vg_fmtdata && *vg_fmtdata) {
		(*vg_fmtdata)->cached_mda_size = (size + size2);
		(*vg_fmtdata)->cached_mda_checksum = checksum;
	}

	if (use_previous_vg)
		*use_previous_vg = 0;

      out:
	if (cft)
		config_destroy(cft);
	return vg;
}

struct volume_group *text_read_metadata_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc)
{
	return text_read_metadata(fid, file, NULL, NULL, NULL, 0,
				  (off_t)0, 0, (off_t)0, 0, NULL, 0,
				  when, desc);
}

static struct volume_group *_import_vg_from_config_tree(struct cmd_context *cmd,
							struct format_instance *fid,
							const struct dm_config_tree *cft)
{
	struct volume_group *vg = NULL;
	const struct text_vg_version_ops **vsn;
	int vg_missing;

	_init_text_import();

	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;
		/*
		 * The only path to this point uses cached vgmetadata,
		 * so it can use cached PV state too.
		 */
		if (!(vg = (*vsn)->read_vg(cmd, fid->fmt, fid, cft)))
			stack;
		else {
			set_pv_devices(fid, vg);

			if ((vg_missing = vg_missing_pv_count(vg)))
				log_verbose("There are %d physical volumes missing.", vg_missing);
			vg_mark_partial_lvs(vg, 1);
			/* FIXME: move this code inside read_vg() */
		}
		break;
	}

	return vg;
}

struct volume_group *import_vg_from_config_tree(struct cmd_context *cmd,
						struct format_instance *fid,
						const struct dm_config_tree *cft)
{
	return _import_vg_from_config_tree(cmd, fid, cft);
}

struct volume_group *vg_from_config_tree(struct cmd_context *cmd, const struct dm_config_tree *cft)
{
	const struct text_vg_version_ops *ops;

	_init_text_import();

	ops = _text_vsn_list[0];

	return ops->read_vg(cmd, cmd->fmt, NULL, cft);
}

