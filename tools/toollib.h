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

#ifndef LVM_TOOLLIB_H
#define LVM_TOOLLIB_H

#include "lib/metadata/metadata-exported.h"
#include "lib/report/report.h"

int become_daemon(struct cmd_context *cmd, int skip_lvm);

/*
 * The element of the devs list created by dev_args_list_create(_available) and
 * processed by the devs_* functions.
 *
 * The devs_* functions that check devs mark a dev that fails with "failed" and
 * log why, then return 0 after checking all devs.  A command that requires all
 * devs fails.  A command that can use some of the devs continues with the devs
 * that have not failed.  The devs_* functions skip failed devs.  A failed dev
 * stays in the list so that it can still be released by unlock_global_devs.
 * (dev is NULL for an arg that did not resolve to a device.)
 */
struct device_arg {
	struct dm_list list;
	const char *name;
	struct device *dev;
	unsigned failed : 1;

	/*
	 * The write steps (dev_args_wipe_and_zero, dev_args_pv_setup_and_zero,
	 * dev_args_pv_write) have nothing to do for this dev, e.g. it is not a PV
	 * and is being removed, or it is already a PV and will be used as is.
	 */
	unsigned skip_write : 1;

	/*
	 * The usage of the dev, saved by the first dev_args_accepted_for_* call.
	 * Later calls fail the command if the usage, vg name or pvid changed.
	 */
	unsigned usage_saved : 1;
	unsigned is_not_pv : 1;		/* not a PV */
	unsigned is_orphan_pv : 1;	/* an orphan PV */
	unsigned is_vg_pv : 1;		/* a PV in a VG (vg_name) */
	unsigned is_used_unknown_pv : 1; /* a PV used by an unknown VG */
	unsigned is_unused_dup_dev : 1;	/* a duplicate of a PV on another dev */
	char pvid[ID_LEN + 1];
	const char *vg_name;

	/*
	 * *_user_prompt: signatures found on the dev that will be wiped
	 * (struct dev_signature), and the user's answers.  If the user
	 * agreed to wipe the signatures, dev_args_wipe_and_zero does not ask again.
	 */
	struct dm_list sigs;
	unsigned sigs_detected : 1;
	unsigned sigs_approved : 1;
	unsigned prompt_answered : 1;
};

int dev_args_list_create(struct cmd_context *cmd, int argc, char **argv,
			 struct dm_list *devs);
int dev_args_list_create_unique(struct cmd_context *cmd, int argc, char **argv,
				struct dm_list *devs);
int dev_args_list_create_available(struct cmd_context *cmd, int argc, char **argv,
				   struct dm_list *devs);

/*
 * The "struct processing_handle" is used as a handle for processing
 * functions (process_each_* and related).
 *
 * The "custom_handle" is any handle used to pass custom data into
 * process_each_* and related functions.
 *
 * The "internal_report_for_select=0" makes processing function to
 * skip checking the report/selection criteria (if given on cmd line)
 * before executing the action on the item.
 *
 * The "selection_handle" is only used if "internal_report_for_select=1".
 *
 * Some important notes about selection:
 * =====================================
 * In case we're processing for display, the selection is directly
 * a part of reporting for the display on output so we don't need to
 * report the item in memory to get the selection result, then dropping
 * the report and then reporting the same thing again for it to be
 * displayed on output.
 * For example, compare these code paths:
 *
 *   - when reporting for display on output:
 *      _report -> process_each_* -> ... -> dm_report_object
 *      (Here the dm_report_object does both selection and
 *       reporting for display on output.)
 *
 *   - for any other processing and reporting for selection:
 *      process_each_* -> _select_match_* -> ... -> dm_report_object_is_selected
 *                                                            |
 *                                                            --> (selection result) --> ...
 *      (Here the dm_report_object_is_selected just gets
 *       the selection result and it drops reporting buffer
 *       immediately. Then based on the selection result,
 *       the process_each_* action on the item is executed
 *       or not...)
 *
 * Simply, we want to avoid this double reporting when reporting
 * for display on output:
 *     _report -> process_each_* -> _select_match_* -> ... -> dm_report_object_is_selected
 *                                                                      |
 *                                                                      --> (selection result) -> dm_report_object
 *
 * So whenever the processing action is "to display item on output", use
 * "internal_report_for_select=0" as report/selection is already
 * a part of that reporting for display (dm_report_object).
 */
struct processing_handle {
	struct processing_handle *parent;
	int internal_report_for_select;
	int include_historical_lvs;
	struct selection_handle *selection_handle;
	void *custom_handle;
	log_report_t saved_log_report_state;
};

typedef int (*process_single_vg_fn_t)(struct cmd_context *cmd,
				      const char *vg_name,
				      struct volume_group *vg,
				      struct processing_handle *handle);
typedef int (*process_single_pv_fn_t)(struct cmd_context *cmd,
				      struct volume_group *vg,
				      struct physical_volume *pv,
				      struct processing_handle *handle);
typedef int (*process_single_label_fn_t)(struct cmd_context *cmd,
					 struct label *label,
					 struct processing_handle *handle);
typedef int (*process_single_lv_fn_t)(struct cmd_context *cmd,
				      struct logical_volume *lv,
				      struct processing_handle *handle);
typedef int (*process_single_seg_fn_t)(struct cmd_context *cmd,
				       struct lv_segment *seg,
				       struct processing_handle *handle);
typedef int (*process_single_pvseg_fn_t)(struct cmd_context *cmd,
					 struct volume_group *vg,
					 struct pv_segment *pvseg,
					 struct processing_handle *handle);

/*
 * Called prior to process_single_lv() to decide if the LV should be
 * processed.  If this returns 0, the LV is not processed.
 *
 * This can evaluate the combination of command definition and
 * the LV object to decide if the combination is allowed.
 */
typedef int (*check_single_lv_fn_t)(struct cmd_context *cmd,
				    struct logical_volume *lv,
				    struct processing_handle *handle,
				    int lv_is_named_arg);

int process_each_vg(struct cmd_context *cmd,
		    int argc, char **argv,
		    const char *one_vgname,
		    struct dm_list *use_vgnames,
		    uint32_t read_flags,
		    int include_internal,
		    struct processing_handle *handle,
		    process_single_vg_fn_t process_single_vg);

int process_each_pv(struct cmd_context *cmd, int argc, char **argv,
		    const char *only_this_vgname,
		    int all_is_set, uint32_t read_flags,
		    struct processing_handle *handle,
		    process_single_pv_fn_t process_single_pv);

int process_each_label(struct cmd_context *cmd, int argc, char **argv,
		       struct processing_handle *handle,
		       process_single_label_fn_t process_single_label);

int process_each_segment_in_pv(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       struct processing_handle *handle,
			       process_single_pvseg_fn_t process_single_pvseg);

int process_each_lv(struct cmd_context *cmd, int argc, char **argv,
		    const char *one_vgname, const char *one_lvname,
		    uint32_t read_flags, struct processing_handle *handle,
		    check_single_lv_fn_t check_single_lv,
		    process_single_lv_fn_t process_single_lv);


int process_each_segment_in_lv(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       struct processing_handle *handle,
			       process_single_seg_fn_t process_single_seg);

int process_each_pv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct processing_handle *handle,
			  process_single_pv_fn_t process_single_pv);


int process_each_lv_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			  struct dm_list *arg_lvnames, const struct dm_list *tags_in,
			  int stop_on_error, struct processing_handle *handle,
			  check_single_lv_fn_t check_single_lv,
			  process_single_lv_fn_t process_single_lv);

struct processing_handle *init_processing_handle(struct cmd_context *cmd, struct processing_handle *parent_handle);
int init_selection_handle(struct cmd_context *cmd, struct processing_handle *handle,
			  unsigned initial_report_type);
void destroy_processing_handle(struct cmd_context *cmd, struct processing_handle *handle);

int select_match_vg(struct cmd_context *cmd, struct processing_handle *handle,
		    struct volume_group *vg);
int select_match_lv(struct cmd_context *cmd, struct processing_handle *handle,
		    struct volume_group *vg, struct logical_volume *lv);
int select_match_pv(struct cmd_context *cmd, struct processing_handle *handle,
		    struct volume_group *vg, struct physical_volume *pv);

const char *extract_vgname(struct cmd_context *cmd, const char *lv_name);
const char *skip_dev_dir(struct cmd_context *cmd, const char *vg_name,
			 unsigned *dev_dir_found);

int opt_in_list_is_set(struct cmd_context *cmd, const uint16_t *opts, int count,
		       int *match_count, int *unmatch_count);

void opt_array_to_str(struct cmd_context *cmd, const uint16_t *opts, int count,
		      char *buf, int len);

int pvcreate_params_from_args(struct cmd_context *cmd, struct pvcreate_params *pp);
void dev_args_filter_clear(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_filter_nodata(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_filter_read(struct cmd_context *cmd, struct dm_list *devs);
int dev_passes_filter_nodata(struct cmd_context *cmd, struct device *dev);
int dev_args_old_ext_version(struct dm_list *devs);
int dev_args_label_scan(struct cmd_context *cmd, struct dm_list *devs);
int dev_open_excl(struct device *dev);
int dev_args_any_failed(struct dm_list *devs);
int dev_args_all_failed(struct dm_list *devs);
void dev_args_label_drop(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_label_scan_excl(struct cmd_context *cmd, struct dm_list *devs);
void dev_args_close(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_accepted_for_pvremove(struct cmd_context *cmd, struct dm_list *devs);
int pvremove_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat);
int dev_args_accepted_for_pvcreate(struct cmd_context *cmd, struct dm_list *devs,
				   struct pvcreate_params *pp);
int pvcreate_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat);
int dev_args_wipe_and_zero(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_label_remove(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_pv_setup_and_zero(struct cmd_context *cmd, struct dm_list *devs,
			       struct pv_create_args *pva, struct dm_list *pvs);
int dev_args_pv_write(struct cmd_context *cmd, struct dm_list *devs, struct dm_list *pvs);
int dev_args_update_devices_file(struct cmd_context *cmd, struct dm_list *devs,
				 struct dm_list *pvs, int remove);
int dev_args_accepted_for_vgcreate(struct cmd_context *cmd, struct dm_list *devs,
				   struct pvcreate_params *pp);
int vgcreate_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat);
int dev_args_pv_orphans_read(struct cmd_context *cmd, struct dm_list *devs, struct dm_list *pvs);
int dev_args_to_device_list(struct cmd_context *cmd, struct dm_list *args,
			    struct dm_list *devs, int skip_failed);
int dev_args_all_pv_orphans(struct dm_list *devs);
int dev_args_all_vg_members(struct dm_list *devs);
int lock_global_dev_args(struct cmd_context *cmd, struct dm_list *devs);
int unlock_global_dev_args(struct cmd_context *cmd, struct dm_list *devs);
int dev_args_accepted_for_vgextend_early(struct cmd_context *cmd, struct dm_list *devs,
					 struct pvcreate_params *pp);
int dev_args_accepted_for_vgextend(struct cmd_context *cmd, struct dm_list *devs,
				   struct volume_group *vg, struct pvcreate_params *pp);
int vgextend_user_prompt(struct cmd_context *cmd, struct dm_list *devs, int *repeat);

int vgcreate_params_set_defaults(struct cmd_context *cmd,
				 struct vgcreate_params *vp_def,
				 struct volume_group *vg);
int vgcreate_params_set_from_args(struct cmd_context *cmd,
				  struct vgcreate_params *vp_new,
				  struct vgcreate_params *vp_def,
				  struct pvcreate_params *pp);
int lv_change_activate(struct cmd_context *cmd, struct logical_volume *lv,
		       activation_change_t activate);
int lv_refresh(struct cmd_context *cmd, struct logical_volume *lv);
int vg_refresh_visible(struct cmd_context *cmd, struct volume_group *vg);
void lv_spawn_background_polling(struct cmd_context *cmd,
				 struct logical_volume *lv);

int get_activation_monitoring_mode(struct cmd_context *cmd,
				   int *monitoring_mode);

int get_pool_params(struct cmd_context *cmd,
		    const struct segment_type *segtype,
		    int *pool_data_vdo,
		    uint64_t *pool_metadata_size,
		    int *pool_metadata_spare,
		    uint32_t *chunk_size,
		    thin_discards_t *discards,
		    thin_zero_t *zero_new_blocks);

int get_stripe_params(struct cmd_context *cmd, const struct segment_type *segtype,
		      uint32_t *stripes, uint32_t *stripe_size,
		      unsigned *stripes_supplied, unsigned *stripe_size_supplied);

int get_cache_params(struct cmd_context *cmd,
		     uint32_t *chunk_size,
		     cache_metadata_format_t *cache_metadata_format,
		     cache_mode_t *cache_mode,
		     const char **name,
		     struct dm_config_tree **settings);

#define VDO_CHANGE_ONLINE  1
#define VDO_CHANGE_OFFLINE 2
int get_vdo_settings(struct cmd_context *cmd,
		     struct dm_vdo_target_params *vtp,
		     int *updated);

int get_writecache_settings(struct cmd_context *cmd, struct dm_writecache_settings *settings,
			    uint32_t *block_size_sectors);

int get_integrity_settings(struct cmd_context *cmd, struct dm_integrity_settings *settings);

int change_tag(struct cmd_context *cmd, struct volume_group *vg,
	       struct logical_volume *lv, struct physical_volume *pv, int arg);

int get_and_validate_major_minor(const struct cmd_context *cmd,
				 const struct format_type *fmt,
				 int32_t *major, int32_t *minor);

int validate_lvname_param(struct cmd_context *cmd, const char **vg_name,
			  const char **lv_name);
int validate_restricted_lvname_param(struct cmd_context *cmd, const char **vg_name,
				     const char **lv_name);

int lvremove_single(struct cmd_context *cmd, struct logical_volume *lv,
		    struct processing_handle *handle);

int get_lvt_enum(struct logical_volume *lv);

int get_rootvg_dev_uuid(struct cmd_context *cmd, char **dm_uuid_out);

int persist_start_include(struct cmd_context *cmd, struct volume_group *vg,
			  int autoactivate, int autolockstart, const char *remkey);

#endif
