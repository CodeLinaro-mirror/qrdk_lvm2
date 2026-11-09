/*
 * Copyright (C) 2010-2026 Red Hat, Inc. All rights reserved.
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

/*
 * _eat_space() scans a comment to the next '\n' bounded by the end of the
 * buffer, so an unterminated final comment is the case where that bound is
 * actually load-bearing.  All of these must parse, and identically.
 */
static void test_comments(void *fixture)
{
	static const struct {
		const char *conf;
		const char *expect;
	} cases[] = {
		/* Trailing comment terminated by a newline. */
		{ "id = \"a\"\n# trailing\n", "a" },
		/* Same, with no trailing newline: the scan runs into the end of the
		 * buffer and must stop there rather than past it. */
		{ "id = \"a\"\n# unterminated", "a" },
		/* Comment on the same line as the final value, no newline. */
		{ "id = \"a\" # same line", "a" },
		/* Inside a quoted value '#' is an ordinary character; a bare value
		 * ends at '#'. */
		{ "id = \"a#b\" # real comment\n", "a#b" },
		/* Blank and whitespace-only lines interleaved with comments. */
		{ "\n# one\n\n   \n# two\nid = \"b\"\n", "b" },
		/* CRLF: '\r' is whitespace, so the comment ends at the '\n'. */
		{ "id = \"c\"\r\n# comment\r\n", "c" },
	};
	struct dm_config_tree *tree;
	unsigned i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		tree = dm_config_from_string(cases[i].conf);
		T_ASSERT((long) tree);
		T_ASSERT(!strcmp(dm_config_find_str(tree->root, "id", "foo"),
				 cases[i].expect));
		dm_config_destroy(tree);
	}

	/* A config with no value in it is a parse error, comments included.
	 * Pinned here so the comment-skipping path cannot change it. */
	T_ASSERT(!dm_config_from_string("# only a comment\n"));
	T_ASSERT(!dm_config_from_string("# only a comment, no newline"));
	T_ASSERT(!dm_config_from_string(""));
}

/*
 * A NUL ends the parse: it delimits the config buffer, which is cut at the
 * first one, so nothing from there on is part of the config.  A NUL is not a
 * byte of any config we write, so it only reaches the parser in malformed or
 * deliberately hostile input, but where it appears it bounds the parse.
 *
 * dm_config_from_string() cannot express this: it stops at the NUL, so the
 * explicit-length entry point is needed to put one inside the buffer.
 */
static void test_nul_ends_parse(void *fixture)
{
	static const char no_nul[] = "id = \"a\"\n# comment\nother = \"b\"\n";
	static const char nul_in_comment[] = "id = \"a\"\n# comment\0hidden\nother = \"b\"\n";
	static const char nul_in_value[] = "id = \"a\"\0other = \"b\"\n";
	static const struct {
		const char *text;
		size_t len;
		int has_other;
	} cases[] = {
		/* Control: with no NUL, the key after the comment is parsed. */
		{ no_nul, sizeof(no_nul) - 1, 1 },
		/* A NUL inside a comment stops the parse there. */
		{ nul_in_comment, sizeof(nul_in_comment) - 1, 0 },
		/* And a NUL outside a comment does the same. */
		{ nul_in_value, sizeof(nul_in_value) - 1, 0 },
	};
	struct dm_config_tree *tree;
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(cases); i++) {
		tree = dm_config_create();
		T_ASSERT(tree);
		T_ASSERT(dm_config_parse_without_dup_node_check(tree, cases[i].text,
							       cases[i].text + cases[i].len));
		T_ASSERT(dm_config_find_str(tree->root, "id", NULL));
		T_ASSERT_EQUAL(!!dm_config_find_str(tree->root, "other", NULL),
			       cases[i].has_other);
		dm_config_destroy(tree);
	}
}

/* Long enough for "Parse error at byte <n> (line <n>): <reason>\n". */
#define PARSE_ERR_LEN 256

static char _log_buf[PARSE_ERR_LEN];
static size_t _log_used;

__attribute__((format(printf, 5, 6)))
static void _capture_log(int level, const char *file, int line,
			 int dm_errno_or_class, const char *f, ...)
{
	va_list ap;
	int n;

	if (_log_used >= sizeof(_log_buf) - 1)
		return;

	va_start(ap, f);
	n = vsnprintf(_log_buf + _log_used, sizeof(_log_buf) - _log_used, f, ap);
	va_end(ap);

	if (n > 0) {
		_log_used += (size_t) n;
		if (_log_used >= sizeof(_log_buf))
			_log_used = sizeof(_log_buf) - 1;
	}
}

/*
 * Run one parse and return what it logged.  A parse error's line number is
 * only visible through log_error(), so install a log callback for the call
 * and restore the default afterwards.  The parse result itself is not
 * enough: a comment-skipping bug that drops the trailing newline still
 * yields a correct tree, it just reports the wrong line to the user.
 */
static void _capture_parse_error(const char *conf_text, char *buf, size_t buflen)
{
	size_t n;

	if (!buflen)
		return;

	_log_used = 0;
	_log_buf[0] = 0;

	dm_log_with_errno_init(_capture_log);
	dm_config_from_string(conf_text);
	dm_log_with_errno_init(NULL);

	n = _log_used < buflen - 1 ? _log_used : buflen - 1;
	memcpy(buf, _log_buf, n);
	buf[n] = 0;
}

static void test_comment_line_numbers(void *fixture)
{
	char buf[PARSE_ERR_LEN];

	/* A trailing comment whose newline must still be counted.  The error is
	 * reported at line 4, not 3, because the final newline is consumed
	 * before the unexpected end of input is seen. */
	_capture_parse_error("id = \"a\"\n# comment\nbad\n", buf, sizeof(buf));
	T_ASSERT(strstr(buf, "line 4") != NULL);

	/* Several comment lines: every one of them advances the count. */
	_capture_parse_error("# one\n# two\n# three\nbad\n", buf, sizeof(buf));
	T_ASSERT(strstr(buf, "line 5") != NULL);

	/* Comment on the same line as a value: the newline is still counted. */
	_capture_parse_error("id = \"a\" # trailing\nbad\n", buf, sizeof(buf));
	T_ASSERT(strstr(buf, "line 3") != NULL);
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
	T("comments", "comment and end-of-buffer handling", test_comments);
	T("comment-lines", "line numbering across comments", test_comment_line_numbers);
	T("comment-nul", "a NUL ends the parse, even inside a comment", test_nul_ends_parse);
	T("clone", "duplicating a config tree", test_clone);
	T("cascade", "cascade", test_cascade);
	T("filtered-sanlock", "filtered parse keeping a section", test_filtered_sanlock);
	T("filtered-stop", "filtered parse stopping at a section", test_filtered_stop);
	T("filtered-stop-no-nl", "filtered stop degrading to skip without trailing newline",
	  test_filtered_stop_no_trailing_newline);
	T("only-section", "parse only a section", test_only_section);

	dm_list_add(all_tests, &ts->list);
}
