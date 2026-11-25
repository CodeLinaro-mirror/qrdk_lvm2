/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
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

#ifndef PERSIST_H
#define PERSIST_H

#define PR_STR_WE   "WE"
#define PR_STR_EA   "EA"
#define PR_STR_WERO "WERO"
#define PR_STR_EARO "EARO"
#define PR_STR_WEAR "WEAR"
#define PR_STR_EAAR "EAAR"

#define PR_TYPE_WE   1
#define PR_TYPE_EA   2
#define PR_TYPE_WERO 3
#define PR_TYPE_EARO 4
#define PR_TYPE_WEAR 5
#define PR_TYPE_EAAR 6

/* hex string. key is 8 bytes (16 hex chars) */
#define PR_KEY_BUF_SIZE 20

#define SETPR_Y			0x00000001
#define SETPR_N			0x00000002
#define SETPR_REQUIRE		0x00000004
#define SETPR_NOREQUIRE		0x00000008
#define SETPR_AUTOSTART		0x00000010
#define SETPR_NOAUTOSTART	0x00000020
#define SETPR_PTPL		0x00000040
#define SETPR_NOPTPL		0x00000080
#define MAX_SETPR_ARGS		8

/*
 * setpersist flags that enable PR for the VG: y, require, autostart.
 * (ptpl alone does not enable PR; it only sets the behavior of
 * persisting reservations through power loss.)
 */
#define SETPR_ON_MASK		(SETPR_Y | SETPR_REQUIRE | SETPR_AUTOSTART)

/*
 * setpersist flags that require the host PR policy to allow PR: any flag
 * that enables PR, and ptpl, which does not enable PR but still configures
 * the VG to persist reservations.
 */
#define SETPR_POLICY_MASK	(SETPR_ON_MASK | SETPR_PTPL)

/*
 * The host PR policy from lvm.conf global/persistent_reservations:
 *
 * auto (the default): use PR selectively as appropriate.  LVM uses PR
 *   automatically only where it is known to be useful without any user
 *   configuration: with use_lvmlockd=1, the first shared sanlock VG in
 *   a new environment (no old shared VGs visible) is created with
 *   global mode 2, using PR in place of the lvmlockd global lock.
 *   Otherwise PR is used only when explicitly enabled for a VG
 *   (--setpersist/--persist).
 *
 * full: use PR whenever possible and appropriate: commands using
 *   devices that do not belong to a VG take a PR on them, new VGs are
 *   created with PR enabled when the devices support it, and lvmlockd
 *   uses global mode 2.  Devices that do not support PR fall back to
 *   the lvmlockd global lock.
 *
 * none: never use PR.
 */

#define PR_POLICY_AUTO		0
#define PR_POLICY_FULL		1
#define PR_POLICY_NONE		2

#define PR_POLICY_NONE_MSG	"Persistent reservations are disabled in lvm config (persistent_reservations=none)."

int persist_check(struct cmd_context *cmd, struct volume_group *vg);

int persist_read(struct cmd_context *cmd, struct volume_group *vg);

int persist_start(struct cmd_context *cmd, struct volume_group *vg,
		  const char *remkey, const char *update_our_key);

int persist_stop(struct cmd_context *cmd, struct volume_group *vg);
int persist_stop_devs(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *stop_devs);
int persist_vgreduce_clear_devs(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *clear_devs);

int persist_finish_before(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *devs, char **key);
void persist_finish_after(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *devs, char *key);

int persist_remove(struct cmd_context *cmd, struct volume_group *vg, const char *remkey);

int persist_clear(struct cmd_context *cmd, struct volume_group *vg);

int persist_vgextend_begin(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *devs);
int persist_vgextend_update(struct cmd_context *cmd, struct volume_group *vg, struct dm_list *devs);
int persist_vgextend_end(struct cmd_context *cmd, struct dm_list *devs);
void persist_global_devs_hand_off(struct cmd_context *cmd);

int persist_global_devices(struct cmd_context *cmd, struct dm_list *devs);
int persist_global_devices_lock(struct cmd_context *cmd, struct dm_list *devs, int *used_pr);
int persist_global_devices_release(struct cmd_context *cmd, struct dm_list *devs);

int persist_vgcreate_begin(struct cmd_context *cmd, const char *vg_name, uint32_t set_flags, struct dm_list *devs);
int persist_vgcreate_update(struct cmd_context *cmd, struct volume_group *vg,
			    uint32_t set_flags, int start_pr);
int persist_vgcreate_end(struct cmd_context *cmd, struct dm_list *devs);

int persist_upgrade_ex(struct cmd_context *cmd, struct volume_group *vg, uint64_t *our_key_held);
int persist_upgrade_stop(struct cmd_context *cmd, struct volume_group *vg, uint64_t our_key_val);

int persist_is_started(struct cmd_context *cmd, struct volume_group *vg, int *is_error, int may_fail);
int persist_is_started_gen(struct cmd_context *cmd, struct volume_group *vg, int *is_error, int may_fail, uint64_t ls_generation, uint64_t *our_key_ret);
int persist_is_started_by_other_hosts(struct cmd_context *cmd, struct volume_group *vg, int *is_started_other);

int persist_key_update(struct cmd_context *cmd, struct volume_group *vg, uint32_t prev_gen);

void persist_key_file_remove_name(struct cmd_context *cmd, const char *vg_name);
void persist_key_file_remove(struct cmd_context *cmd, struct volume_group *vg);
void persist_key_file_rename(struct volume_group *vg, const char *old_name, const char *new_name);

int dev_read_reservation_nvme(struct cmd_context *cmd, struct device *dev, uint64_t *holder_ret, int *prtype_ret);

int dev_find_key(struct cmd_context *cmd, struct device *dev, int may_fail,
		 uint64_t find_key, int *found_key,
		 int find_host_id, uint64_t *found_host_id_key,
		 int find_all, int *found_count, uint64_t **found_all);

int dev_find_key_nvme(struct cmd_context *cmd, struct device *dev, int may_fail,
		      uint64_t find_key, int *found_key,
		      int find_host_id, uint64_t *found_host_id_key,
		      int find_all, int *found_count, uint64_t **found_all);

int vg_is_registered(struct cmd_context *cmd, struct volume_group *vg, uint64_t *our_key_ret, int *partial_ret, uint32_t sanlock_gen);

int setpersist_arg_flags(const char *str, uint32_t *flags);

/*
 * The host PR policy from lvm.conf global/persistent_reservations:
 * PR_POLICY_AUTO/FULL/NONE, or -1 on error.
 */
int persist_policy_get(struct cmd_context *cmd);

/*
 * The lvmlockd global mode from lvm.conf global/lvmlockd_global:
 * LOCKD_GLOBAL_MODE_1/2 when set explicitly, 0 for global_mode_auto,
 * -1 on error.
 */
int lockd_global_mode_parse(struct cmd_context *cmd);

int dev_allow_pr(struct cmd_context *cmd, struct device *dev);

#endif
