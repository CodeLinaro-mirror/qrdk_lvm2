/*
 * Copyright (C) 2005-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libdm/misc/dmlib.h"
#include "libdm-common.h"

int dm_get_status_snapshot(struct dm_pool *mem, const char *params,
			   struct dm_status_snapshot **status)
{
	struct dm_status_snapshot *s;
	int r;

	if (!params) {
		log_error("Failed to parse invalid snapshot params.");
		return 0;
	}

	if (!(s = dm_pool_zalloc(mem, sizeof(*s)))) {
		log_error("Failed to allocate snapshot status structure.");
		return 0;
	}

	r = sscanf(params, FMTu64 "/" FMTu64 " " FMTu64,
		   &s->used_sectors, &s->total_sectors,
		   &s->metadata_sectors);

	if (r == 3 || r == 2)
		s->has_metadata_sectors = (r == 3);
	else if (!strcmp(params, "Invalid"))
		s->invalid = 1;
	else if (!strcmp(params, "Merge failed"))
		s->merge_failed = 1;
	else if (!strcmp(params, "Overflow"))
		s->overflow = 1;
	else {
		dm_pool_free(mem, s);
		log_error("Failed to parse snapshot params: %s.", params);
		return 0;
	}

	*status = s;

	return 1;
}

/*
 * Skip nr fields each delimited by a single space.
 * FIXME Don't assume single space.
 */
static const char *_skip_fields(const char *p, unsigned nr)
{
	while (p && nr-- && (p = strchr(p, ' ')))
		p++;

	return p;
}

/*
 * Count number of single-space delimited fields.
 * Number of fields is number of spaces plus one.
 */
static unsigned _count_fields(const char *p)
{
	unsigned nr = 1;

	if (!p || !*p)
		return 0;

	while ((p = _skip_fields(p, 1)))
		nr++;

	return nr;
}

/*
 * Various RAID status versions include:
 * Versions < 1.5.0 (4 fields):
 *   <raid_type> <#devs> <health_str> <sync_ratio>
 * Versions 1.5.0+  (6 fields):
 *   <raid_type> <#devs> <health_str> <sync_ratio> <sync_action> <mismatch_cnt>
 * Versions 1.9.0+  (7 fields):
 *   <raid_type> <#devs> <health_str> <sync_ratio> <sync_action> <mismatch_cnt> <data_offset>
 */
DM_EXPORT_NEW_SYMBOL(int, dm_get_status_raid, 1_02_218)
	(struct dm_pool *mem, const char *params,
	 struct dm_status_raid **status)
{
	int i;
	unsigned num_fields;
	const char *p, *pp, *msg_fields = "";
	struct dm_status_raid *s = NULL;
	unsigned a = 0;

	if ((num_fields = _count_fields(params)) < 4)
		goto_bad;

	/* Second field holds the device count */
	msg_fields = "<#devs> ";
	if (!(pp = _skip_fields(params, 1)) || (sscanf(pp, "%d", &i) != 1) || !(p = _skip_fields(pp, 1)))
		goto_bad;

	msg_fields = "";
	if (!(s = dm_pool_zalloc(mem, sizeof(struct dm_status_raid))))
		goto_bad;

	msg_fields = "<raid_type> <#devices> <health_chars> and <sync_ratio> ";
	if (!(s->raid_type = dm_pool_strndup(mem, params, pp - params - 1)))
		goto_bad; /* memory is freed when pool is destroyed */

	if (!(pp = _skip_fields(p, 1)))
		goto_bad;

	s->advertised_dev_count = i;

	/* dm-raid 1.9.0 and 1.9.1 emitted health chars by iterating its internal
	 * rdev list, omitting failed components removed from that list. For a
	 * three-slot table with "- -" at slot 0, it could report
	 * "raid1 3 AA ..." rather than the slot-preserving "raid1 3 -AA ...".
	 * Kernel commit c63ede3b4211 (upstream Linux 4.11, target 1.9.2) fixed this
	 * by retaining the failed slots in status output. The status text has no
	 * slot identifiers, so libdm cannot reconstruct omitted positions; preserve
	 * the actual length for callers that know the RAID component layout.
	 */
	if (i > (pp - p - 1))
		i = pp - p - 1;

	if (!(s->dev_health = dm_pool_strndup(mem, p, i))) /* health chars */
		goto_bad;
	p = pp;

	s->dev_count = i;
	if (sscanf(p, FMTu64 "/" FMTu64, &s->insync_regions, &s->total_regions) != 2)
		goto_bad;

	/*
	 * All pre-1.5.0 version parameters are read.  Now we check
	 * for additional 1.5.0+ parameters (i.e. num_fields at least 6).
	 *
	 * Note that 'sync_action' will be NULL (and mismatch_count
	 * will be 0) if the kernel returns a pre-1.5.0 status.
	 */
	if (num_fields < 6)
		goto out;

	msg_fields = "<sync_action> and <mismatch_cnt> ";

	/* Skip pre-1.5.0 params */
	if (!(pp = _skip_fields(params, 4)) || !(p = _skip_fields(pp, 1)))
		goto_bad;

	if (!(s->sync_action = dm_pool_strndup(mem, pp, p - pp - 1)))
		goto_bad;

	if (sscanf(p, FMTu64, &s->mismatch_count) != 1)
		goto_bad;

	if (num_fields < 7)
		goto out;

	/*
	 * All pre-1.9.0 version parameters are read.  Now we check
	 * for additional 1.9.0+ parameters (i.e. nr_fields at least 7).
	 *
	 * Note that data_offset will be 0 if the
	 * kernel returns a pre-1.9.0 status.
	 */
	msg_fields = "<data_offset>";
	if (!(p = _skip_fields(params, 6))) /* skip pre-1.9.0 params */
		goto_bad;
	if (sscanf(p, FMTu64, &s->data_offset) != 1)
		goto_bad;

	/* <journal_char>  - 'A' - active write-through journal device.
	 *                 - 'a' - active write-back journal device.
	 *                 - 'D' - dead journal device.
	 *                 - '-' - no journal device.
	 */

out:
	*status = s;

	while (i-- > 0)
		if (s->dev_health[i] == 'a')
			a++; /* Count number of 'a' */

	if (a && s->sync_action) {
		if ((a < s->dev_count) &&  /* SOME legs are in 'a' */
		    /* FIXME: kernel gives misleading info here
		     * Trying to recognize a true state */
		    (s->insync_regions == s->total_regions) &&
		    (!strcasecmp(s->sync_action, "recover") ||
		     !strcasecmp(s->sync_action, "idle"))) {
			/* Kernel may possibly start some action
			 * in near-by future, do not report 100% */
			s->insync_regions--;
		}
		if ((a == s->dev_count) && /* all legs are in 'a' */
		    (!strcasecmp(s->sync_action, "resync") ||
		     !strcasecmp(s->sync_action, "idle"))) {
			/* Mark 1st. leg in sync */
			s->dev_health[0] = 'A';
		}
	}

	return 1;

bad:
	log_error("Failed to parse %sraid params: %s", msg_fields, params);

	if (s)
		dm_pool_free(mem, s);

	*status = NULL;

	return 0;
}

/*
 * Put the health characters of a short status back at the slots of the
 * raid table line @params.  Called only when @s reports fewer characters
 * than devices; see dm_get_status_raid_aligned().
 *
 * Succeeds only when every slot takes exactly one character and none is
 * left over, so the caller can index the result by slot; a slot whose data
 * device the table shows as '-' takes '-'.  Returns 0 when the table does
 * not describe the slots the status counted, when the characters do not
 * line up with them, or on allocation failure.
 */
static int _raid_health_from_table(struct dm_pool *mem,
				   struct dm_status_raid *s,
				   const char *params,
				   const char *devname)
{
	char *health;
	const char *p, *h;
	unsigned num_params, num_devs, i;
	int used = 0, missing;

	/* Table: <raid_type> <#raid_params> [params...] <#devs> [<meta> <data>]... */
	if (sscanf(params, "%*s %u", &num_params) != 1 ||
	    !(p = _skip_fields(params, 2 + num_params)) ||
	    sscanf(p, " %u%n", &num_devs, &used) != 1) {
		log_error("Failed to parse the raid table of %s.", devname);
		return 0;
	}
	p += used;

	/* The table must describe the slots the target counted in <#devs>. */
	if (num_devs != s->advertised_dev_count) {
		log_error("Raid table of %s has %u devices, its status reports %u; "
			  "cannot align the health characters.",
			  devname, num_devs, s->advertised_dev_count);
		return 0;
	}

	if (!(health = dm_pool_zalloc(mem, num_devs + 1))) {
		log_error("Failed to allocate RAID health string for %s.", devname);
		return 0;
	}

	/* p is at the space after <#devs>, which the first skip uses up. */
	for (i = 0, h = s->dev_health; i < num_devs; i++) {
		/*
		 * Only the data device tells whether the slot holds a
		 * device: a missing metadata device alone is normal for
		 * raid0, which lvm2 builds as "- <data>", so a slot whose
		 * data device is missing takes '-' and gives its character
		 * to no slot.  dm_get_status_raid_aligned() says what a
		 * slot filled with '-' means.
		 */
		if (!(p = _skip_fields(p, 1)))	/* metadata device */
			goto bad;

		if (!(p = _skip_fields(p, 1)))	/* data device */
			goto bad;

		missing = (*p == '-');

		if (!missing && !*h)
			goto bad;

		health[i] = missing ? '-' : *h;

		if (!missing)
			h++;
	}

	if (*h)		/* a character found no slot */
		goto bad;

	log_debug_activation("Kernel dm-raid health %s of %s does not fit its "
			     "table, using %s.",
			     s->dev_health, devname, health);

	s->dev_health = health;
	s->dev_count = num_devs;

	return 1;
bad:
	log_error("Raid health characters of %s do not line up with its table.",
		  devname);
	dm_pool_free(mem, health);

	return 0;
}

/*
 * dm-raid before 1.9.2 (Linux 4.11) builds the health string from the list
 * of devices it kept rather than from the slots of the table, so a slot
 * with no device contributes no character and the string is shorter than
 * the device count of the table.  The characters still follow the slots
 * that have a device, in slot order, so walking the device tuples puts
 * every character back at its own slot and fills the other slots.  The
 * table is the target's rendering of its current state, because
 * DM_DEVICE_TABLE is a status call with the table flag, so its slots
 * describe the array the status was taken from.  The table is read here
 * and not by the caller: it must hold a single raid target, every caller
 * needs that same check, and the need for the table is known only after
 * the status is parsed.
 *
 * A slot with no device is either one userspace passed as '- -' or one
 * whose device the constructor released after failing to read its
 * superblock.  The release drops the device from the status list and from
 * the rendering, so a target before 1.9.2 prints the same '- -' for both
 * and neither the status nor the table can say which one a gap is.  Such
 * a slot takes '-', the character 1.9.2 and later print for a slot with
 * no device, and a caller that knows which slots it passed as '- -' can
 * name the released ones.  When the characters already cover every slot
 * they are already at their slot, so no correction is needed there.
 *
 * Success means one character per slot, so a caller can index dev_health
 * by slot without checking the counts.  When the table cannot be read or
 * does not line up with the characters, the parsed status is released and
 * the call fails: a caller that needs positions cannot use such a status.
 */
int dm_get_status_raid_aligned(struct dm_pool *mem,
				const struct dm_info *dinfo,
				const char *params,
				struct dm_status_raid **status)
{
	struct dm_status_raid *s;
	struct dm_task *dmt = NULL;
	const char *devname;
	uint64_t start, length;
	char *type, *tparams;
	void *next;
	int r = 0;

	if (status)
		*status = NULL;

	if (!dm_get_status_raid(mem, params, &s))
		return 0;

	/*
	 * One character per device, or no device whose table could map a
	 * short one: hand back what the parser produced.
	 */
	if (!dinfo || s->dev_count == s->advertised_dev_count) {
		if (status)
			*status = s;
		return 1;
	}

	if (!(dmt = dm_task_create(DM_DEVICE_TABLE))) {
		log_error("Failed to create a task to read the table of %u:%u.",
			  dinfo->major, dinfo->minor);
		goto out;
	}

	if (!dm_task_set_major_minor(dmt, dinfo->major, dinfo->minor, 1) ||
	    !dm_task_no_open_count(dmt) ||
	    !dm_task_no_flush(dmt) ||
	    !dm_task_run(dmt)) {
		log_error("Failed to read the table of %u:%u.",
			  dinfo->major, dinfo->minor);
		goto out;
	}

	next = dm_get_next_target(dmt, NULL, &start, &length, &type, &tparams);
	if (next) {
		log_error("More than one table line found for %u:%u.",
			  dinfo->major, dinfo->minor);
		goto out;
	}

	if (!type || !tparams || strcmp(type, "raid")) {
		log_error("Expected raid segment type but got %s instead.",
			  type ? type : "NULL");
		goto out;
	}

	devname = dm_task_get_name(dmt);
	if (!_raid_health_from_table(mem, s, tparams,
				     devname ? devname : "raid device"))
		goto out;

	if (status)
		*status = s;
	r = 1;
out:
	if (dmt)
		dm_task_destroy(dmt);

	if (!r)
		dm_pool_free(mem, s);

	return r;
}

/*
 * <metadata block size> <#used metadata blocks>/<#total metadata blocks>
 * <cache block size> <#used cache blocks>/<#total cache blocks>
 * <#read hits> <#read misses> <#write hits> <#write misses>
 * <#demotions> <#promotions> <#dirty> <#features> <features>*
 * <#core args> <core args>* <policy name> <#policy args> <policy args>*
 *
 * metadata block size      : Fixed block size for each metadata block in
 *                            sectors
 * #used metadata blocks    : Number of metadata blocks used
 * #total metadata blocks   : Total number of metadata blocks
 * cache block size         : Configurable block size for the cache device
 *                            in sectors
 * #used cache blocks       : Number of blocks resident in the cache
 * #total cache blocks      : Total number of cache blocks
 * #read hits               : Number of times a READ bio has been mapped
 *                            to the cache
 * #read misses             : Number of times a READ bio has been mapped
 *                            to the origin
 * #write hits              : Number of times a WRITE bio has been mapped
 *                            to the cache
 * #write misses            : Number of times a WRITE bio has been
 *                            mapped to the origin
 * #demotions               : Number of times a block has been removed
 *                            from the cache
 * #promotions              : Number of times a block has been moved to
 *                            the cache
 * #dirty                   : Number of blocks in the cache that differ
 *                            from the origin
 * #feature args            : Number of feature args to follow
 * feature args             : 'writethrough' (optional)
 * #core args               : Number of core arguments (must be even)
 * core args                : Key/value pairs for tuning the core
 *                            e.g. migration_threshold
 *			     *policy name              : Name of the policy
 * #policy args             : Number of policy arguments to follow (must be even)
 * policy args              : Key/value pairs
 *                            e.g. sequential_threshold
 */
int dm_get_status_cache(struct dm_pool *mem, const char *params,
			struct dm_status_cache **status)
{
	int i, feature_argc;
	char *str;
	const char *p, *pp;
	struct dm_status_cache *s;

	if (!(s = dm_pool_zalloc(mem, sizeof(struct dm_status_cache))))
		return_0;

	if (strstr(params, "Error")) {
		s->error = 1;
		s->fail = 1; /*  This is also I/O fail state */
		goto out;
	}

	if (strstr(params, "Fail")) {
		s->fail = 1;
		goto out;
	}

	/* Read in args that have definitive placement */
	if (sscanf(params,
		   " " FMTu32
		   " " FMTu64 "/" FMTu64
		   " " FMTu32
		   " " FMTu64 "/" FMTu64
		   " " FMTu64 " " FMTu64
		   " " FMTu64 " " FMTu64
		   " " FMTu64 " " FMTu64
		   " " FMTu64
		   " %d",
		   &s->metadata_block_size,
		   &s->metadata_used_blocks, &s->metadata_total_blocks,
		   &s->block_size, /* AKA, chunk_size */
		   &s->used_blocks, &s->total_blocks,
		   &s->read_hits, &s->read_misses,
		   &s->write_hits, &s->write_misses,
		   &s->demotions, &s->promotions,
		   &s->dirty_blocks,
		   &feature_argc) != 14)
		goto bad;

	/* Now jump to "features" section */
	if (!(p = _skip_fields(params, 12)))
		goto bad;

	/* Read in features */
	for (i = 0; i < feature_argc; i++) {
		if (!strncmp(p, "writethrough ", 13))
			s->feature_flags |= DM_CACHE_FEATURE_WRITETHROUGH;
		else if (!strncmp(p, "writeback ", 10))
			s->feature_flags |= DM_CACHE_FEATURE_WRITEBACK;
		else if (!strncmp(p, "passthrough ", 12))
			s->feature_flags |= DM_CACHE_FEATURE_PASSTHROUGH;
		else if (!strncmp(p, "metadata2 ", 10))
			s->feature_flags |= DM_CACHE_FEATURE_METADATA2;
		else if (!strncmp(p, "no_discard_passdown ", 20))
			s->feature_flags |= DM_CACHE_FEATURE_NO_DISCARD_PASSDOWN;
		else
			log_error("Unknown feature in status: %s", params);

		if (!(p = _skip_fields(p, 1)))
			goto bad;
	}

	/* Read in core_args. */
	if (sscanf(p, "%d ", &s->core_argc) != 1)
		goto bad;
	if ((s->core_argc > 0) &&
	    (!(s->core_argv = dm_pool_zalloc(mem, sizeof(char *) * s->core_argc)) ||
	     !(p = _skip_fields(p, 1)) ||
	     !(str = dm_pool_strdup(mem, p)) ||
	     !(p = _skip_fields(p, (unsigned) s->core_argc)) ||
	     (dm_split_words(str, s->core_argc, 0, s->core_argv) != s->core_argc)))
		goto bad;

	/* Read in policy args */
	pp = p;
	if (!(p = _skip_fields(p, 1)))
		goto_bad;

	i = p - pp;
	if ((i < 1) ||
	    !(s->policy_name = dm_pool_zalloc(mem, i)))
		goto_bad;

	dm_strncpy(s->policy_name, pp, i);

	if (sscanf(p, "%d", &s->policy_argc) != 1)
		goto bad;

	if (s->policy_argc &&
	    (!(s->policy_argv = dm_pool_zalloc(mem, sizeof(char *) * s->policy_argc)) ||
	     !(p = _skip_fields(p, 1)) ||
	     !(str = dm_pool_strdup(mem, p)) ||
	     (dm_split_words(str, s->policy_argc, 0, s->policy_argv) != s->policy_argc)))
		goto bad;

	/* TODO: improve this parser */
	if (strstr(p, " ro"))
		s->read_only = 1;

	if (strstr(p, " needs_check"))
		s->needs_check = 1;
out:
	*status = s;
	return 1;

bad:
	log_error("Failed to parse cache params: %s", params);
	dm_pool_free(mem, s);
	*status = NULL;

	return 0;
}

/*
 * From linux/Documentation/device-mapper/writecache.txt
 *
 * Status:
 * 1. error indicator - 0 if there was no error, otherwise error number
 * 2. the number of blocks
 * 3. the number of free blocks
 * 4. the number of blocks under writeback
 */

int dm_get_status_writecache(struct dm_pool *mem, const char *params,
			     struct dm_status_writecache **status)
{
	struct dm_status_writecache *s;

	if (!(s = dm_pool_zalloc(mem, sizeof(struct dm_status_writecache))))
		return_0;

	if (sscanf(params, "%llu %llu %llu %llu",
		   (unsigned long long *)&s->error,
		   (unsigned long long *)&s->total_blocks,
		   (unsigned long long *)&s->free_blocks,
		   (unsigned long long *)&s->writeback_blocks) != 4) {
		log_error("Failed to parse writecache params: %s.", params);
		dm_pool_free(mem, s);
		return 0;
	}

	*status = s;

	return 1;
}

int dm_get_status_integrity(struct dm_pool *mem, const char *params,
			     struct dm_status_integrity **status)
{
	struct dm_status_integrity *s;
	char recalc_str[16] = { 0 };

	if (!(s = dm_pool_zalloc(mem, sizeof(*s))))
		return_0;

	if (sscanf(params, "%llu %llu %15s",
		   (unsigned long long *)&s->number_of_mismatches,
		   (unsigned long long *)&s->provided_data_sectors,
		   recalc_str) != 3) {
		log_error("Failed to parse integrity params: %s.", params);
		dm_pool_free(mem, s);
		return 0;
	}

	if (recalc_str[0] == '-')
		s->recalc_sector = 0;
	else
		s->recalc_sector = strtoull(recalc_str, NULL, 0);

	*status = s;

	return 1;
}

int parse_thin_pool_status(const char *params, struct dm_status_thin_pool *s)
{
	int pos;

	memset(s, 0, sizeof(*s));

	if (!params) {
		log_error("Failed to parse invalid thin params.");
		return 0;
	}

	if (strstr(params, "Error")) {
		s->error = 1;
		s->fail = 1; /*  This is also I/O fail state */
		return 1;
	}

	if (strstr(params, "Fail")) {
		s->fail = 1;
		return 1;
	}

	/* FIXME: add support for held metadata root */
	if (sscanf(params, FMTu64 " " FMTu64 "/" FMTu64 " " FMTu64 "/" FMTu64 "%n",
		   &s->transaction_id,
		   &s->used_metadata_blocks,
		   &s->total_metadata_blocks,
		   &s->used_data_blocks,
		   &s->total_data_blocks, &pos) < 5) {
		log_error("Failed to parse thin pool params: %s.", params);
		return 0;
	}

	/* New status flags */
	if (strstr(params + pos, "no_discard_passdown"))
		s->discards = DM_THIN_DISCARDS_NO_PASSDOWN;
	else if (strstr(params + pos, "ignore_discard"))
		s->discards = DM_THIN_DISCARDS_IGNORE;
	else /* default discard_passdown */
		s->discards = DM_THIN_DISCARDS_PASSDOWN;

	/* Default is 'writable' (rw) data */
	if (strstr(params + pos, "out_of_data_space"))
		s->out_of_data_space = 1;
	else if (strstr(params + pos, "ro "))
		s->read_only = 1;

	/* Default is 'queue_if_no_space' */
	if (strstr(params + pos, "error_if_no_space"))
		s->error_if_no_space = 1;

	if (strstr(params + pos, "needs_check"))
		s->needs_check = 1;

	return 1;
}

int dm_get_status_thin_pool(struct dm_pool *mem, const char *params,
			    struct dm_status_thin_pool **status)
{
	struct dm_status_thin_pool *s;

	if (!(s = dm_pool_alloc(mem, sizeof(struct dm_status_thin_pool)))) {
		log_error("Failed to allocate thin_pool status structure.");
		return 0;
	}

	if (!parse_thin_pool_status(params, s)) {
		dm_pool_free(mem, s);
		return_0;
	}

	*status = s;

	return 1;
}

int dm_get_status_thin(struct dm_pool *mem, const char *params,
		       struct dm_status_thin **status)
{
	struct dm_status_thin *s;

	if (!(s = dm_pool_zalloc(mem, sizeof(struct dm_status_thin)))) {
		log_error("Failed to allocate thin status structure.");
		return 0;
	}

	if (strchr(params, '-')) {
		/* nothing to parse */
	} else if (strstr(params, "Fail")) {
		s->fail = 1;
	} else if (sscanf(params, FMTu64 " " FMTu64,
		   &s->mapped_sectors,
		   &s->highest_mapped_sector) != 2) {
		dm_pool_free(mem, s);
		log_error("Failed to parse thin params: %s.", params);
		return 0;
	}

	*status = s;

	return 1;
}

static dm_status_mirror_health_t _get_health(char c)
{
	switch (c) {
	case 'A': return DM_STATUS_MIRROR_ALIVE;
	case 'F': return DM_STATUS_MIRROR_FLUSH_FAILED;
	case 'D': return DM_STATUS_MIRROR_WRITE_FAILED;
	case 'S': return DM_STATUS_MIRROR_SYNC_FAILED;
	case 'R': return DM_STATUS_MIRROR_READ_FAILED;
	default:
		log_warn("WARNING: Unknown mirror health status char: %c", c);
		/* fall through */
	case 'U': return DM_STATUS_MIRROR_UNCLASSIFIED;
	}
}

/*
 * dm core params:	     0 409600 mirror
 * Mirror core params:	     2 253:4 253:5 400/400
 * New-style failure params: 1 AA
 * New-style log params:     3 cluster 253:3 A
 *			 or  3 disk 253:3 A
 *			 or  1 core
 */
#define DM_MIRROR_MAX_IMAGES 8 /* limited by kernel DM_KCOPYD_MAX_REGIONS */

int dm_get_status_mirror(struct dm_pool *mem, const char *params,
			 struct dm_status_mirror **status)
{
	struct dm_status_mirror *s;
	const char *p, *pos = params;
	unsigned num_devs, argc, i;
	int used;

	if (!(s = dm_pool_zalloc(mem, sizeof(*s)))) {
		log_error("Failed to alloc mem pool to parse mirror status.");
		return 0;
	}

	if (sscanf(pos, "%u %n", &num_devs, &used) != 1)
		goto_out;
	pos += used;

	if (num_devs > DM_MIRROR_MAX_IMAGES) {
		log_error("More than " DM_TO_STRING(DM_MIRROR_MAX_IMAGES)
			  " devices reported in mirror status.");
		goto out;
	}

	if (!(s->devs = dm_pool_alloc(mem, num_devs * sizeof(*(s->devs))))) {
		log_error("Allocation of devs failed.");
		goto out;
	}

	for (i = 0; i < num_devs; ++i, pos += used)
		if (sscanf(pos, "%u:%u %n",
			   &(s->devs[i].major), &(s->devs[i].minor), &used) != 2)
			goto_out;

	if (sscanf(pos, FMTu64 "/" FMTu64 "%n",
		   &s->insync_regions, &s->total_regions, &used) != 2)
		goto_out;
	pos += used;

	if (sscanf(pos, "%u %n", &argc, &used) != 1)
		goto_out;
	pos += used;

	/* One health char per image */
	if (strlen(pos) < num_devs)
		goto_out;

	for (i = 0; i < num_devs ; ++i)
		s->devs[i].health = _get_health(pos[i]);

	if (!(pos = _skip_fields(pos, argc)))
		goto_out;

	if (strncmp(pos, "userspace", 9) == 0) {
		pos += 9;
		/* FIXME: support status of userspace mirror implementation */
	}

	if (sscanf(pos, "%u %n", &argc, &used) != 1)
		goto_out;
	pos += used;

	if (argc == 1) {
		/* core, cluster-core */
		if (!(s->log_type = dm_pool_strdup(mem, pos))) {
			log_error("Allocation of log type string failed.");
			goto out;
		}
	} else {
		if (!(p = _skip_fields(pos, 1)))
			goto_out;

		/* disk, cluster-disk */
		if (!(s->log_type = dm_pool_strndup(mem, pos, p - pos - 1))) {
			log_error("Allocation of log type string failed.");
			goto out;
		}
		pos = p;

		if ((argc > 2) && !strcmp(s->log_type, "disk")) {
			s->log_count = argc - 2;

			if (!(s->logs = dm_pool_alloc(mem, s->log_count * sizeof(*(s->logs))))) {
				log_error("Allocation of logs failed.");
				goto out;
			}

			for (i = 0; i < s->log_count; ++i, pos += used)
				if (sscanf(pos, "%u:%u %n",
					   &s->logs[i].major, &s->logs[i].minor, &used) != 2)
					goto_out;

			/* One health char per log device */
			if (strlen(pos) < s->log_count)
				goto_out;

			for (i = 0; i < s->log_count; ++i)
				s->logs[i].health = _get_health(pos[i]);
		}
	}

	s->dev_count = num_devs;
	*status = s;

	return 1;
out:
	log_error("Failed to parse mirror status %s.", params);
	dm_pool_free(mem, s);
	*status = NULL;

	return 0;
}

#if defined(GNU_SYMVER)
/*
 * Binaries built before struct dm_status_raid gained advertised_dev_count
 * keep working: libdm allocates the struct and returns a pointer, and the
 * new field is only appended, so their offsets are unchanged.
 */
DM_EXPORT_SYMBOL_BASE(dm_get_status_raid)
int dm_get_status_raid_base(struct dm_pool *mem, const char *params,
			    struct dm_status_raid **status);
int dm_get_status_raid_base(struct dm_pool *mem, const char *params,
			    struct dm_status_raid **status)
{
	return dm_get_status_raid(mem, params, status);
}
#endif
