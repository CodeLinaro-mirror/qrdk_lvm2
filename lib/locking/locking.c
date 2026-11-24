/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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
#include "lib/locking/locking.h"
#include "lib/locking/lvmlockd.h"
#include "locking_types.h"
#include "lib/metadata/metadata.h"
#include "lib/config/config.h"
#include "lib/misc/lvm-string.h"
#include "lib/activate/activate.h"
#include "lib/commands/toolcontext.h"
#include "lib/mm/memlock.h"
#include "lib/config/defaults.h"
#include "lib/cache/lvmcache.h"
#include "lib/device/persist.h"
#include "lib/display/display.h"
#include "lib/misc/lvm-signal.h"

#include <assert.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static struct locking_type _locking;

static int _vg_lock_count = 0;		/* Number of locks held */
static int _vg_write_lock_held = 0;	/* VG write lock held? */
static int _blocking_supported = 0;
static int _file_locking_readonly = 0;
static int _file_locking_sysinit = 0;
static int _file_locking_ignorefail = 0;
static int _file_locking_failed = 0;

static void _unblock_signals(void)
{
	/* Don't unblock signals while any locks are held */
	if (!_vg_lock_count)
		unblock_signals();
}

void reset_locking(void)
{
	int was_locked = _vg_lock_count;

	/* file locking disabled */
	if (!_locking.flags)
		return;

	_vg_lock_count = 0;
	_vg_write_lock_held = 0;

	if (_locking.reset_locking)
		_locking.reset_locking();

	if (was_locked)
		_unblock_signals();

	memlock_reset();
}

static void _update_vg_lock_count(const char *resource, uint32_t flags)
{
	/* Ignore locks not associated with updating VG metadata */
	if (!strcmp(resource, VG_GLOBAL))
		return;

	if ((flags & LCK_TYPE_MASK) == LCK_UNLOCK)
		_vg_lock_count--;
	else
		_vg_lock_count++;

	/* We don't bother to reset this until all VG locks are dropped */
	if ((flags & LCK_TYPE_MASK) == LCK_WRITE)
		_vg_write_lock_held = 1;
	else if (!_vg_lock_count)
		_vg_write_lock_held = 0;
}

/*
 * A mess of options have been introduced over time to override
 * or tweak the behavior of file locking, and indirectly other
 * behaviors.  These options are allowed in different but
 * overlapping sets of commands (see command-lines.in)
 *
 * --nolocking
 *
 * Command won't try to set up or use file locks at all.
 *
 * --readonly
 *
 * Command will grant any read lock request, without trying
 * to acquire an actual file lock.  Command will refuse any
 * write lock request.  (Activation, which uses a write lock,
 * is not allowed.)
 *
 * --ignorelockingfailure
 *
 * Command tries to set up file locks and will use them
 * (both read and write) if successful.  If command fails
 * to set up file locks it falls back to readonly behavior
 * above, while allowing activation.
 *
 * --sysinit
 *
 * The same as ignorelockingfailure.
 *
 * --sysinit --readonly
 *
 * The combination of these two flags acts like --readonly,
 * refusing write lock requests, but makes an exception to
 * allow activation.
 *
 * global/metadata_read_only
 *
 * The command acquires actual read locks and refuses
 * write lock requests.
 */

int init_locking(struct cmd_context *cmd,
		 int file_locking_sysinit, int file_locking_readonly, int file_locking_ignorefail)
{
	int suppress_messages = 0;

	if (file_locking_sysinit || getenv("LVM_SUPPRESS_LOCKING_FAILURE_MESSAGES"))
		suppress_messages = 1;

	_blocking_supported = find_config_tree_bool(cmd, global_wait_for_locks_CFG, NULL);
	_file_locking_readonly = file_locking_readonly;
	_file_locking_sysinit = file_locking_sysinit;
	_file_locking_ignorefail = file_locking_ignorefail;

	log_debug("File locking settings: readonly:%d sysinit:%d ignorelockingfailure:%d global/metadata_read_only:%d global/wait_for_locks:%d.",
		  _file_locking_readonly, _file_locking_sysinit, _file_locking_ignorefail,
		  cmd->metadata_read_only, _blocking_supported);

	if (!init_file_locking(&_locking, cmd, suppress_messages)) {
		log_error_suppress(suppress_messages, "File locking initialisation failed.");

		_file_locking_failed = 1;

		if (file_locking_sysinit || file_locking_ignorefail)
			return 1;

		return 0;
	}

	return 1;
}

void fin_locking(struct cmd_context *cmd)
{
	/* file locking disabled */
	if (!_locking.flags)
		return;

	/*
	 * These may be automatically released when the
	 * command ends, without an explicit unlock call,
	 * in which case these flags would not be cleared.
	 */
	cmd->lockf_global_ex = 0;
	memset(&cmd->global_devs, 0, sizeof(cmd->global_devs));

	_locking.fin_locking();
}

/*
 * VG locking is by VG name.
 * FIXME This should become VG uuid.
 */
static int _lock_vol(struct cmd_context *cmd, const char *resource, uint32_t flags)
{
	int ret = 0;

	block_signals();

	if ((ret = _locking.lock_resource(cmd, resource, flags, NULL)))
		/* ensure signals are blocked while VG_GLOBAL lock is held */
		_update_vg_lock_count(resource, flags);
	else
		stack;

	_unblock_signals();

	return ret;
}

int lock_vol(struct cmd_context *cmd, const char *vol, uint32_t flags, const struct logical_volume *lv)
{
	char resource[258] __attribute__((aligned(8)));
	uint32_t lck_type = flags & LCK_TYPE_MASK;
	int is_global = !strcmp(vol, VG_GLOBAL);

	if (is_orphan_vg(vol))
		return 1;

	if (!_blocking_supported)
		flags |= LCK_NONBLOCK;

	if (!_dm_strncpy(resource, vol, sizeof(resource))) {
		log_error(INTERNAL_ERROR "Resource name %s is too long.", vol);
		return 0;
	}

	/*
	 * File locking is disabled by --nolocking.
	 */
	if (!_locking.flags)
		goto out_hold;

	/*
	 * When file locking could not be initialized, --ignorelockingfailure
	 * and --sysinit behave like --readonly, but allow activation.
	 */
	if (_file_locking_failed && (_file_locking_sysinit || _file_locking_ignorefail)) {
		if (lck_type != LCK_WRITE)
			goto out_hold;

		if (cmd->is_activating && !is_global)
			goto out_hold;

		goto out_fail;
	}

	/*
	 * When --readonly is set, grant read lock requests without trying to
	 * acquire an actual lock, and refuse write lock requests.
	 */
	if (_file_locking_readonly && !_file_locking_sysinit) {
		if (lck_type != LCK_WRITE)
			goto out_hold;

		log_error("Operation prohibited while --readonly is set.");
		goto out_fail;
	}

	/*
	 * When --readonly and --sysinit are set, grant read lock requests without
	 * trying to acquire an actual lock, and refuse write lock requests except
	 * in the case of activation which is permitted.
	 */
	if (_file_locking_readonly && _file_locking_sysinit) {
		if (lck_type != LCK_WRITE)
			goto out_hold;

		if (cmd->is_activating) {
			log_warn("Allowing activation with --readonly --sysinit.");
			goto out_hold;
		}

		log_error("Operation prohibited while --readonly is set.");
		goto out_fail;
	}

	/*
	 * When global/metadata_read_only is set, acquire actual read locks and
	 * refuse write lock requests.
	 */
	if (cmd->metadata_read_only) {
		if (lck_type == LCK_WRITE) {
			log_error("Operation prohibited while global/metadata_read_only is set.");
			goto out_fail;
		}

		/* continue and acquire a read file lock */
	}

	if (!_lock_vol(cmd, resource, flags))
		goto out_fail;

out_hold:
	if (is_global)
		return 1;

	/*
	 * FIXME: other parts of the code want to check if a VG is
	 * locked by looking in lvmcache.  They shouldn't need to
	 * do that, and we should be able to remove this.
	 */
	if (lck_type != LCK_UNLOCK)
		lvmcache_lock_vgname(resource, lck_type == LCK_READ);
	else
		lvmcache_unlock_vgname(resource);

	return 1;

out_fail:
	if (is_global)
		return 0;
	if (lck_type == LCK_UNLOCK)
		_update_vg_lock_count(resource, flags);
	return 0;
}

/* Lock a list of LVs */
int activate_lvs(struct cmd_context *cmd, struct dm_list *lvs, unsigned exclusive)
{
	struct dm_list *lvh;
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, lvs) {
		if (!activate_lv(cmd, lvl->lv)) {
			log_error("Failed to activate %s", display_lvname(lvl->lv));

			dm_list_uniterate(lvh, lvs, &lvl->list) {
				lvl = dm_list_item(lvh, struct lv_list);
				if (!deactivate_lv(cmd, lvl->lv))
					stack;
			}
			return 0;
		}
	}

	return 1;
}

int vg_write_lock_held(void)
{
	return _vg_write_lock_held;
}

void sync_local_dev_names(struct cmd_context *cmd)
{
	memlock_unlock(cmd);
	fs_unlock();
}

/*
 * The global flock is local to the host.  It is only taken ex, to serialize
 * commands on this host that change global state (devices not in a VG, VG
 * names).  Commands that only read are not blocked by it, and do not take it.
 *
 * The lockf_global_ex flag is set while the flock is held ex.
 */

static int _lockf_global(struct cmd_context *cmd, const char *mode, int nonblock)
{
	uint32_t flags = 0;
	int ret;

	if (nonblock)
		flags |= LCK_NONBLOCK;

	if (!strcmp(mode, "ex")) {
		flags |= LCK_WRITE;

		if (cmd->lockf_global_ex) {
			log_warn("global flock already held ex");
			return 1;
		}

		ret = lock_vol(cmd, VG_GLOBAL, flags, NULL);
		if (ret)
			cmd->lockf_global_ex = 1;

	} else if (!strcmp(mode, "un")) {
		ret = lock_vol(cmd, VG_GLOBAL, LCK_UNLOCK, NULL);
		cmd->lockf_global_ex = 0;
	} else {
		log_error(INTERNAL_ERROR "Unknown locking mode %s.", mode);
		return 0;
	}

	return ret;
}

int lockf_global(struct cmd_context *cmd, const char *mode)
{
	return _lockf_global(cmd, mode, 0);
}

int lockf_global_nonblock(struct cmd_context *cmd, const char *mode)
{
	return _lockf_global(cmd, mode, 1);
}

/*
 * Whether devs that are not owned by a VG are protected between hosts
 * (by PR or the global lock).  This is the case when lvmlockd is used,
 * where its global mode selects PR or the global lock, or when
 * persistent_reservations=full, which uses PR without lvmlockd.
 */

int lock_global_devs_is_enabled(struct cmd_context *cmd)
{
	/*
	 * lvmlockd always uses either the Global Lock or PR to protect global devs.
	 */
	if (lvmlockd_use())
		return 1;

	/*
	 * When not using lvmlockd, persistent_reservations=full means that
	 * PR is used on dev_args (if all support PR).
	 */
	return (persist_policy_get(cmd) == PR_POLICY_FULL);
}

/*
 * Protect devices from concurrent use between different hosts,
 * either using Persistent Reservations on the devices, or by
 * using a Global Lock from the lock manager (through lvmlockd.)
 *
 * Outside of the context of shared storage, these devices do
 * not need any protection at all, because there is no other
 * host that may be using them concurrently, and this function
 * is a no-op (lock_global_devs_is_enabled returns 0).
 *
 * The devices protected here do not belong to a VG, and are
 * referred to as "global devices" in the context of locking.
 *
 * In the context of specific commands that use these devices,
 * they are also referred to as "device args", or dev_args
 * (see dev_args.c) because they are the non-VG devices named
 * as args on the command line.
 *
 * When using lvmlockd, use PR on the devices, or use
 * the Global Lock (GL), depending on the lvmlockd Global Mode.
 * (Old lvm versions only used GL, so the Global Mode detects
 * when GL needs to be used to remain compatible with them.)
 *
 * When not using lvmlockd, and persistent_reservations=full in
 * the lvm config, use PR on the devices.
 * A user sets persistent_reservations=full to enable PR when
 * they know that their devices are on shared storage, and they
 * are not using lvmlockd because they are not using shared VGs.
 *
 * A command calls lock_global_devs once (once its device args are
 * known), and unlock_global_devs releases what was acquired.
 * Calling lock_global_devs a second time without an intervening
 * unlock_global_devs is a programming error.
 */

int lock_global_devs(struct cmd_context *cmd, struct dm_list *devs)
{
	enum global_devs_held held = GLOBAL_DEVS_NONE;
	int used_pr = 0;
	int ret;

	if (cmd->global_devs.locked) {
		log_error(INTERNAL_ERROR "lock_global_devs called again before unlock_global_devs.");
		return 0;
	}

	if (!cmd->lockf_global_ex && !lockf_global(cmd, "ex"))
		return 0;

	/*
	 * Nothing to do if lvmlockd is not used, and persistent_reservations
	 * are not enabled outside of lvmlockd.
	 */
	if (!lock_global_devs_is_enabled(cmd))
		return 1;

	if (lvmlockd_use()) {
		/*
		 * With lvmlockd, take PR on devs (calls persist_global_devices_lock),
		 * or acquire the Global Lock (GL) from lvmlockd.
		 * PR or GL depends on the Global Mode which is configured or detected.
		 */
		ret = lockd_global_devices_lock(cmd, devs, cmd->global_devs.bootstrap, &held);
	} else {
		/*
		 * Without lvmlockd, take PR on devs (when they all support PR.)
		 */
		ret = persist_global_devices_lock(cmd, devs, &used_pr);
		if (used_pr)
			held = GLOBAL_DEVS_PR;
	}

	if (!ret)
		return 0;

	cmd->global_devs.held = held;
	cmd->global_devs.handed_off = 0;
	cmd->global_devs.locked = 1;

	return 1;
}

/*
 * Release whatever lock_global_devs() acquired.  This is driven by the state
 * in cmd->global_devs and is safe to call on any exit path, including when
 * nothing was acquired.
 */

int unlock_global_devs(struct cmd_context *cmd, struct dm_list *devs)
{
	enum global_devs_held held = cmd->global_devs.held;
	int handed_off = cmd->global_devs.handed_off;
	int ret = 1;

	memset(&cmd->global_devs, 0, sizeof(cmd->global_devs));

	if (held == GLOBAL_DEVS_PR && !handed_off) {
		if (lvmlockd_use()) {
			if (!lockd_global_devices_unlock(cmd, devs, GLOBAL_DEVS_PR))
				ret = 0;
		} else {
			if (!persist_global_devices_release(cmd, devs))
				ret = 0;
		}
	}

	if (held == GLOBAL_DEVS_GL &&
	    !lockd_global_devices_unlock(cmd, devs, GLOBAL_DEVS_GL))
		ret = 0;

	if (cmd->lockf_global_ex && !lockf_global(cmd, "un"))
		ret = 0;

	return ret;
}
