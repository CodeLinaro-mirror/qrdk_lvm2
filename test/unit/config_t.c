/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "units.h"
#include "libdm/libdevmapper.h"

static void *_mem_init(void)
{
	struct dm_pool *mem = dm_pool_create("config test", 1024);
	if (!mem) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	return mem;
}

static void _mem_exit(void *mem)
{
	dm_pool_destroy(mem);
}

static const char *conf =
	"id = \"yada-yada\"\n"
	"seqno = 15\n"
	"status = [\"READ\", \"WRITE\"]\n"
	"flags = []\n"
	"extent_size = 8192\n"
	"physical_volumes {\n"
	"    pv0 {\n"
	"        id = \"abcd-efgh\"\n"
	"    }\n"
	"    pv1 {\n"
	"        id = \"bbcd-efgh\"\n"
	"    }\n"
	"    pv2 {\n"
	"        id = \"cbcd-efgh\"\n"
	"    }\n"
	"}\n";

static const char *overlay =
	"id = \"yoda-soda\"\n"
	"flags = [\"FOO\"]\n"
	"physical_volumes {\n"
	"    pv1 {\n"
	"        id = \"hgfe-dcba\"\n"
	"    }\n"
	"    pv3 {\n"
	"        id = \"dbcd-efgh\"\n"
	"    }\n"
	"}\n";

static void test_parse(void *fixture)
{
	struct dm_config_tree *tree = dm_config_from_string(conf);
	const struct dm_config_value *value;

	T_ASSERT((long) tree);
	T_ASSERT(dm_config_has_node(tree->root, "id"));
	T_ASSERT(dm_config_has_node(tree->root, "physical_volumes"));
	T_ASSERT(dm_config_has_node(tree->root, "physical_volumes/pv0"));
	T_ASSERT(dm_config_has_node(tree->root, "physical_volumes/pv0/id"));

	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "id", "foo"), "yada-yada"));
	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "idt", "foo"), "foo"));

	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "physical_volumes/pv0/bb", "foo"), "foo"));
	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "physical_volumes/pv0/id", "foo"), "abcd-efgh"));

	T_ASSERT(!dm_config_get_uint32(tree->root, "id", NULL));
	T_ASSERT(dm_config_get_uint32(tree->root, "extent_size", NULL));

	/* FIXME: Currently everything parses as a list, even if it's not */
	// T_ASSERT(!dm_config_get_list(tree->root, "id", NULL));
	// T_ASSERT(!dm_config_get_list(tree->root, "extent_size", NULL));

	T_ASSERT(dm_config_get_list(tree->root, "flags", &value));
	T_ASSERT(value->next == NULL); /* an empty list */
	T_ASSERT(dm_config_get_list(tree->root, "status", &value));
	T_ASSERT(value->next != NULL); /* a non-empty list */

	dm_config_destroy(tree);
}

static void test_clone(void *fixture)
{
	struct dm_config_tree *tree = dm_config_from_string(conf);
	struct dm_config_node *n;
	const struct dm_config_value *value;

	T_ASSERT(tree);

	n = dm_config_clone_node(tree, tree->root, 1);

	T_ASSERT(n);

	/* Check that the nodes are actually distinct. */
	T_ASSERT(n != tree->root);
	T_ASSERT(n->sib != tree->root->sib);
	T_ASSERT(dm_config_find_node(n, "physical_volumes") != NULL);
	T_ASSERT(dm_config_find_node(tree->root, "physical_volumes") != NULL);
	T_ASSERT(dm_config_find_node(n, "physical_volumes") != dm_config_find_node(tree->root, "physical_volumes"));

	T_ASSERT(dm_config_has_node(n, "id"));
	T_ASSERT(dm_config_has_node(n, "physical_volumes"));
	T_ASSERT(dm_config_has_node(n, "physical_volumes/pv0"));
	T_ASSERT(dm_config_has_node(n, "physical_volumes/pv0/id"));

	T_ASSERT(!strcmp(dm_config_find_str(n, "id", "foo"), "yada-yada"));
	T_ASSERT(!strcmp(dm_config_find_str(n, "idt", "foo"), "foo"));

	T_ASSERT(!strcmp(dm_config_find_str(n, "physical_volumes/pv0/bb", "foo"), "foo"));
	T_ASSERT(!strcmp(dm_config_find_str(n, "physical_volumes/pv0/id", "foo"), "abcd-efgh"));

	T_ASSERT(!dm_config_get_uint32(n, "id", NULL));
	T_ASSERT(dm_config_get_uint32(n, "extent_size", NULL));

	/* FIXME: Currently everything parses as a list, even if it's not */
	// T_ASSERT(!dm_config_get_list(tree->root, "id", NULL));
	// T_ASSERT(!dm_config_get_list(tree->root, "extent_size", NULL));

	T_ASSERT(dm_config_get_list(n, "flags", &value));
	T_ASSERT(value->next == NULL); /* an empty list */
	T_ASSERT(dm_config_get_list(n, "status", &value));
	T_ASSERT(value->next != NULL); /* a non-empty list */

	dm_config_destroy(tree);
}

static void test_cascade(void *fixture)
{
	struct dm_config_tree *t1 = dm_config_from_string(conf),
		              *t2 = dm_config_from_string(overlay),
			      *tree;

	T_ASSERT(t1);
	T_ASSERT(t2);

	tree = dm_config_insert_cascaded_tree(t2, t1);

	T_ASSERT(tree);

	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "id", "foo"), "yoda-soda"));
	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "idt", "foo"), "foo"));

	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "physical_volumes/pv0/bb", "foo"), "foo"));
	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "physical_volumes/pv1/id", "foo"), "hgfe-dcba"));
	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "physical_volumes/pv3/id", "foo"), "dbcd-efgh"));

	dm_config_destroy(t1);
	dm_config_destroy(t2);
}

static const char *vgtext =
	"vg0 {\n"
	"\tid = \"vgid\"\n"
	"\tlock_type = \"%s\"\n"
	"\tphysical_volumes {\n"
	"\t\tpv0 {\n"
	"\t\t\tid = \"pvid\"\n"
	"\t\t}\n"
	"\t}\n"
	"\tlogical_volumes {\n"
	"\t\tlv1 {\n"
	"\t\t\tid = \"lv1id\"\n"
	"\t\t\tsegment1 {\n"
	"\t\t\t\ttype = \"striped\"\n"
	"\t\t\t}\n"
	"\t\t}\n"
	"\t\tlvmlock {\n"
	"\t\t\ttags = [\"T1\"]\n"
	"\t\t}\n"
	"\t\tlv2 {\n"
	"\t\t\tid = \"lv2id\"\n"
	"\t\t}\n"
	"\t}\n"
	"}\n"
	"creation_host = \"host\"\n";

static enum dm_config_section_action _filter(const struct dm_config_node *parent,
					     const char *name, unsigned depth, void *arg)
{
	const char *lock_type;

	if (depth == 1 && !strcmp(name, "logical_volumes")) {
		lock_type = dm_config_find_str(parent->child, "lock_type", NULL);
		return (lock_type && !strcmp(lock_type, "sanlock")) ?
			DM_CONFIG_SECTION_PARSE : DM_CONFIG_SECTION_STOP;
	}

	if (depth == 2 && !strcmp(parent->key, "logical_volumes") && strcmp(name, "lvmlock"))
		return DM_CONFIG_SECTION_SKIP;

	return DM_CONFIG_SECTION_PARSE;
}

static struct dm_config_tree *_parse_vg(const char *lock_type, dm_config_section_filter_fn fn)
{
	struct dm_config_tree *cft = dm_config_create();
	char buf[1024];

	T_ASSERT(cft);
	snprintf(buf, sizeof(buf), vgtext, lock_type);
	T_ASSERT(dm_config_parse_filtered(cft, buf, buf + strlen(buf), fn, NULL));

	return cft;
}

static void test_filtered_sanlock(void *fixture)
{
	struct dm_config_tree *cft = _parse_vg("sanlock", _filter);
	const struct dm_config_node *vg = cft->root;

	T_ASSERT(!strcmp(vg->key, "vg0"));
	T_ASSERT(!strcmp(dm_config_find_str(vg->child, "lock_type", ""), "sanlock"));
	T_ASSERT(!strcmp(dm_config_find_str(vg->child, "physical_volumes/pv0/id", ""), "pvid"));
	T_ASSERT(dm_config_has_node(vg->child, "logical_volumes/lvmlock"));
	T_ASSERT(!dm_config_has_node(vg->child, "logical_volumes/lv1"));
	T_ASSERT(!dm_config_has_node(vg->child, "logical_volumes/lv2"));
	T_ASSERT(!strcmp(dm_config_find_str(cft->root, "creation_host", ""), "host"));

	dm_config_destroy(cft);
}

static void test_filtered_stop(void *fixture)
{
	struct dm_config_tree *cft = _parse_vg("none", _filter);
	const struct dm_config_node *vg = cft->root;

	T_ASSERT(!strcmp(dm_config_find_str(vg->child, "physical_volumes/pv0/id", ""), "pvid"));
	T_ASSERT(!dm_config_has_node(vg->child, "logical_volumes"));
	T_ASSERT(!strcmp(dm_config_find_str(cft->root, "creation_host", ""), "host"));

	dm_config_destroy(cft);
}

static void test_only_section(void *fixture)
{
	struct dm_config_tree *cft = dm_config_create();
	char buf[1024];

	snprintf(buf, sizeof(buf), vgtext, "none");
	T_ASSERT(dm_config_parse_only_section(cft, buf, buf + strlen(buf), "physical_volumes"));
	T_ASSERT(dm_config_has_node(cft->root->child, "physical_volumes/pv0"));
	T_ASSERT(!dm_config_has_node(cft->root->child, "logical_volumes"));
	T_ASSERT(!strcmp(dm_config_find_str(cft->root, "creation_host", ""), "host"));

	dm_config_destroy(cft);
}

static enum dm_config_section_action _stop_at_depth1(const struct dm_config_node *parent,
						     const char *name, unsigned depth,
						     void *arg)
{
	if (depth == 1)
		return DM_CONFIG_SECTION_STOP;

	return DM_CONFIG_SECTION_PARSE;
}

/*
 * Without a newline after the final '}' the stop search cannot find
 * "\n}\n" and DM_CONFIG_SECTION_STOP degrades to DM_CONFIG_SECTION_SKIP,
 * so the stopped sections are skipped but the enclosing section and the
 * values preceding them are still parsed.  The empty sub0 section also
 * checks that the failed stop search does not disturb the parser state
 * before the section is skipped.
 */
static void test_filtered_stop_no_trailing_newline(void *fixture)
{
	struct dm_config_tree *cft = dm_config_create();
	static const char buf[] =
		"vg0 {\n"
		"\tid = \"vgid\"\n"
		"\tsub0 {}\n"
		"\tsub1 {\n"
		"\t\ta = 1\n"
		"\t}\n"
		"\tsub2 {\n"
		"\t\tb = 2\n"
		"\t}\n"
		"}";
	const struct dm_config_node *vg;

	T_ASSERT(cft);
	T_ASSERT(dm_config_parse_filtered(cft, buf, buf + strlen(buf), _stop_at_depth1, NULL));

	vg = cft->root;
	T_ASSERT(!strcmp(vg->key, "vg0"));
	T_ASSERT(!strcmp(dm_config_find_str(vg->child, "id", ""), "vgid"));
	T_ASSERT(!dm_config_has_node(vg->child, "sub0"));
	T_ASSERT(!dm_config_has_node(vg->child, "sub1"));
	T_ASSERT(!dm_config_has_node(vg->child, "sub2"));

	dm_config_destroy(cft);
}

#define T(path, desc, fn) register_test(ts, "/metadata/config/" path, desc, fn)

void config_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_mem_init, _mem_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("parse", "parsing various", test_parse);
	T("clone", "duplicating a config tree", test_clone);
	T("cascade", "cascade", test_cascade);
	T("filtered-sanlock", "filtered parse keeping a section", test_filtered_sanlock);
	T("filtered-stop", "filtered parse stopping at a section", test_filtered_stop);
	T("filtered-stop-no-nl", "filtered stop degrading to skip without trailing newline",
	  test_filtered_stop_no_trailing_newline);
	T("only-section", "parse only a section", test_only_section);

	dm_list_add(all_tests, &ts->list);
}
