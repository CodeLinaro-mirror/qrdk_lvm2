/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */

#include "units.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/lv.h"
#include "lib/metadata/metadata-exported.h"
#include "lib/metadata/vg.h"

struct raid_status_fixture {
	struct dm_pool *mem;
	struct cmd_context cmd;
	struct volume_group vg;
	struct logical_volume lv;
	struct logical_volume image;
	struct logical_volume split;
	struct lv_segment seg;
	struct lv_segment_area areas[6];
	struct dm_info info;
};

static void *_fixture_init(void)
{
	struct raid_status_fixture *f = calloc(1, sizeof(*f));
	unsigned i;

	if (!f || !(f->mem = dm_pool_create("raid status test", 1024))) {
		fprintf(stderr, "out of memory\n");
		free(f);
		exit(1);
	}

	f->cmd.mem = f->mem;
	f->vg.cmd = &f->cmd;
	f->vg.name = "test-vg";
	f->lv.vg = &f->vg;
	f->lv.name = "test-lv";
	f->split.status = VISIBLE_LV;
	f->seg.lv = &f->lv;
	f->seg.area_count = 6;
	f->seg.areas = f->areas;
	f->info.major = 4242;
	f->info.minor = 4242;

	for (i = 0; i < f->seg.area_count; i++) {
		f->areas[i].type = AREA_LV;
		f->areas[i].u.lv.lv = &f->image;
	}
	f->areas[1].type = AREA_UNASSIGNED;
	f->areas[3].u.lv.lv = &f->split;
	f->areas[4].type = AREA_UNASSIGNED;

	return f;
}

static void _fixture_exit(void *fixture)
{
	struct raid_status_fixture *f = fixture;

	dm_pool_destroy(f->mem);
	free(f);
}

static void _test_status_remap(void *fixture)
{
	struct raid_status_fixture *f = fixture;
	struct dm_status_raid *status = NULL;

	/* Unassigned and tracked-split slots stay '-', a missing image is 'D'. */
	T_ASSERT(raid_get_status(f->mem, &f->info,
				 &f->seg, "raid1 6 A---Da 50/100 idle 0",
				 &status));
	if (status) {
		T_ASSERT(!strcmp(status->dev_health, "A-D-Da"));
		T_ASSERT(raid_status_covers_segment(&f->seg, status));
	}

	/* A visible, writable image is not a tracked split. */
	f->split.status |= LVM_WRITE;
	T_ASSERT(raid_get_status(f->mem, &f->info,
				 &f->seg, "raid1 6 A---Da 50/100 idle 0",
				 &status));
	if (status)
		T_ASSERT(!strcmp(status->dev_health, "A-DDDa"));
}

static void _test_status_count_mismatch(void *fixture)
{
	struct raid_status_fixture *f = fixture;
	struct dm_status_raid *status = NULL;

	/* A complete kernel string need not cover the metadata areas. */
	T_ASSERT(raid_get_status(f->mem, &f->info,
				 &f->seg, "raid1 3 A-- 50/100 idle 0",
				 &status));
	if (status) {
		T_ASSERT(!strcmp(status->dev_health, "A-D"));
		T_ASSERT(!raid_status_covers_segment(&f->seg, status));
	}

	/* Never inspect an area beyond the end of the metadata array. */
	f->seg.area_count = 2;
	T_ASSERT(raid_get_status(f->mem, &f->info,
				 &f->seg, "raid1 3 A-- 50/100 idle 0",
				 &status));
	if (status) {
		T_ASSERT(!strcmp(status->dev_health, "A--"));
		T_ASSERT(!raid_status_covers_segment(&f->seg, status));
	}
}

static void _test_status_invalid(void *fixture)
{
	struct raid_status_fixture *f = fixture;
	struct dm_status_raid *status = NULL;

	T_ASSERT(!raid_get_status(f->mem, &f->info, &f->seg, "invalid",
				  &status));
	T_ASSERT(!status);
}

void raid_status_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_fixture_init, _fixture_exit);

	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	register_test(ts, "/raid/status/remap", "remap RAID slots",
		      _test_status_remap);
	register_test(ts, "/raid/status/count-mismatch", "check RAID slot count",
		      _test_status_count_mismatch);
	register_test(ts, "/raid/status/invalid", "reject invalid RAID status",
		      _test_status_invalid);
	dm_list_add(all_tests, &ts->list);
}
