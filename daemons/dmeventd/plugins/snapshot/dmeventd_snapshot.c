/*
 * Copyright (C) 2007-2015 Red Hat, Inc. All rights reserved.
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
#include "daemons/dmeventd/plugins/lvm2/dmeventd_lvm.h"
#include "daemons/dmeventd/libdevmapper-event.h"
#include "daemons/dmeventd/dmeventd_percent.h"
#include "daemons/dmeventd/dmeventd_retry.h"

#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <mntent.h>
#include <stdarg.h>
#include <pthread.h>

/* First warning when snapshot is 80% full. */
#define WARNING_THRESH	(DM_PERCENT_1 * 80)
/* Run a check every 5%. */
#define CHECK_STEP	(DM_PERCENT_1 *  5)
/* Do not bother checking snapshots less than 50% full. */
#define CHECK_MINIMUM	(DM_PERCENT_1 * 50)

#define UMOUNT_COMMAND "/bin/umount"

struct dso_state {
	struct dm_pool *mem;
	dm_percent_t percent_check;
	uint64_t known_size;
	struct dmeventd_policy_retry policy_retry;
	char cmd_lvextend[512];
};

DM_EVENT_LOG_FN("snap")

/*
 * Run a command described by a NULL-terminated argument vector, argv[0] is
 * the command.  Return 1 when the child ran and exited successfully, 0 when
 * fork, wait or the command itself failed.
 */
static int _run(const char *const argv[])
{
        pid_t pid = fork();
        int status;

        if (pid == 0) { /* child */
                execvp(argv[0], (char **) argv);
                log_sys_error("exec", argv[0]);
                exit(127);
        }

        if (pid > 0) { /* parent */
                if (waitpid(pid, &status, 0) != pid)
                        return 0; /* waitpid failed */
                if (!WIFEXITED(status) || WEXITSTATUS(status))
                        return 0; /* the child failed */
        }

        if (pid < 0)
                return 0; /* fork failed */

        return 1; /* all good */
}

static int _extend(const char *cmd)
{
	log_debug("Extending snapshot via %s.", cmd);
	return dmeventd_lvm2_run_with_lock(cmd);
}

#ifdef SNAPSHOT_REMOVE
/* Remove invalid snapshot from dm-table */
/* Experimental for now and not used by default */
static int _remove(const char *uuid)
{
	int r = 1;
	uint32_t cookie = 0;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_REMOVE)))
		return 0;

	if (!dm_task_set_uuid(dmt, uuid)) {
		r = 0;
		goto_out;
	}

	dm_task_retry_remove(dmt);

	if (!dm_task_set_cookie(dmt, &cookie, 0)) {
		r = 0;
		goto_out;
	}

	if (!dm_task_run(dmt)) {
		r = 0;
		goto_out;
	}
out:
	dm_task_destroy(dmt);

	return r;
}
#endif /* SNAPSHOT_REMOVE */

static void _umount(const char *device, int major, int minor)
{
	FILE *mounts;
	struct mntent mebuf;
	char *buffer;
	struct stat st;

	/* Line buffer for getmntent_r(), one mount table entry at a time.
	 * It comes from the heap: dmeventd runs a thread per monitored
	 * device on a stack of THREAD_STACK_SIZE. */
	if (!(buffer = malloc(MOUNTS_LINE_MAX))) {
		log_error("Failed to allocate the mount table line buffer.");
		return;
	}

	if (!(mounts = setmntent(PROC_MOUNTS, "r"))) {
		log_error("Not umounting %s: cannot read %s: %s.", device,
			  PROC_MOUNTS, strerror(errno));
		free(buffer);
		return;
	}

	/* getmntent_r() decodes the octal escapes of the mount table, so a
	 * mount point holding a blank reaches umount as a usable path and
	 * not as the escaped form, which umount cannot match.  The entry
	 * goes to mebuf and the strings to the line buffer, both owned by
	 * this call and not shared between the threads. */
	while (getmntent_r(mounts, &mebuf, buffer, MOUNTS_LINE_MAX)) {
		/* find the major/minor of the device */
		if (stat(mebuf.mnt_fsname, &st))
			continue; /* can't stat, skip this one */

		if (S_ISBLK(st.st_mode) &&
		    (int) major(st.st_rdev) == major &&
		    (int) minor(st.st_rdev) == minor) {
			const char *umount_argv[] = { UMOUNT_COMMAND, "-fl", mebuf.mnt_dir, NULL };

			log_error("Unmounting invalid snapshot %s at mount point %s.", device, mebuf.mnt_dir);
			if (!_run(umount_argv))
				log_error("Failed to umount snapshot %s at mount point %s: %s.",
					  device, mebuf.mnt_dir, strerror(errno));
		}
	}

	endmntent(mounts);
	free(buffer);
}

/* Kernel-reported Invalid, Overflow, or unusable zero-sized status: nothing
 * the monitor can do will revive the snapshot.  Stop monitoring, unmount it
 * and request deregistration.  With SNAPSHOT_REMOVE the kernel dm-table is
 * dropped too, but the LVM metadata LV always remains and needs a manual
 * 'lvremove'.  Numeric 100% full status alone is not terminal. */
static void _snapshot_terminal(struct dso_state *state, struct dm_task *dmt,
			       const char *device)
{
	struct dm_info info;
	int ret;

	state->percent_check = 0;
	if (dm_task_get_info(dmt, &info))
		_umount(device, info.major, info.minor);
#ifdef SNAPSHOT_REMOVE
	/* Experimental for now and not used by default */
	_remove(dm_task_get_uuid(dmt));
#endif
	if ((ret = pthread_kill(pthread_self(), SIGALRM)) && (ret != ESRCH))
		log_error("pthread_kill self failed: %s.", strerror(ret));
}

/*
 * At an armed usage step: apply retry backoff, warn, extend, and advance.
 */
static void _snapshot_handle_percent_check(struct dso_state *state,
					   const char *device,
					   int percent)
{
	int retrying;

	if (percent < state->percent_check)
		return;

	retrying = state->policy_retry.fails ? 1 : 0;

	if (dmeventd_policy_retry_should_postpone(&state->policy_retry, "extension"))
		return;

	if (!retrying && (percent >= WARNING_THRESH))
		log_warn("WARNING: Snapshot %s is now %.2f%% full.",
			 device, dm_percent_to_round_float(percent, 2));

	if (!_extend(state->cmd_lvextend)) {
		log_error("Failed to extend snapshot %s.", device);
		state->policy_retry.fails = 1;
		return;
	}

	dmeventd_policy_retry_after_success(&state->policy_retry);

	dmeventd_advance_percent_check_after_action(percent,
						    &state->percent_check,
						    CHECK_STEP);
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask evmask __attribute__((unused)),
		   void **user)
{
	struct dso_state *state = *user;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	struct dm_status_snapshot *status = NULL;
	const char *device = dm_task_get_name(dmt);
	int percent;
	int ret;

	/* No longer monitoring, waiting for remove */
	if (!state->percent_check)
		return;

	dm_get_next_target(dmt, next, &start, &length, &target_type, &params);
	if (!target_type || strcmp(target_type, "snapshot")) {
		log_error("Target %s is not snapshot.", target_type ?: "???");
		return;
	}

	if (!dm_get_status_snapshot(state->mem, params, &status)) {
		log_error("Cannot parse snapshot %s state: %s.", device, params);
		return;
	}

	/*
	 * If the snapshot has been invalidated or we failed to parse
	 * the status string. Report the full status string to syslog.
	 */
	if (status->invalid || status->overflow || !status->total_sectors) {
		log_warn("WARNING: Snapshot %s changed state to: %s and should be removed.",
			 device, params);
		_snapshot_terminal(state, dmt, device);
		goto out;
	}

	if (length <= (status->used_sectors - status->metadata_sectors)) {
		/* TODO eventually recognize earlier when room is enough */
		log_info("Dropping monitoring of fully provisioned snapshot %s.",
			 device);
		if ((ret = pthread_kill(pthread_self(), SIGALRM)) && (ret != ESRCH))
			log_error("pthread_kill self failed: %s.", strerror(ret));
		goto out;
	}

	/* Snapshot size had changed. Clear the threshold. */
	if (state->known_size != status->total_sectors) {
		state->percent_check = CHECK_MINIMUM;
		state->known_size = status->total_sectors;
		dmeventd_policy_retry_after_success(&state->policy_retry);
	}

	percent = dm_make_percent(status->used_sectors, status->total_sectors);

	_snapshot_handle_percent_check(state, device, percent);
out:
	dm_pool_free(state->mem, status);
}

int register_device(const char *device_name,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **user)
{
	struct dso_state *state;

	if (!dmeventd_lvm2_init_with_pool("snapshot_state", state))
		goto_bad;

	if (!dmeventd_lvm2_command(state->mem, state->cmd_lvextend,
				   sizeof(state->cmd_lvextend),
				   "lvextend --use-policies", device_name))
		goto_bad;

	state->percent_check = CHECK_MINIMUM;
	dmeventd_policy_retry_after_success(&state->policy_retry);
	*user = state;

	log_info("Monitoring snapshot %s.", device_name);

	return 1;
bad:
	log_error("Failed to monitor snapshot %s.", device_name);

	if (state)
		dmeventd_lvm2_exit_with_pool(state);

	return 0;
}

int unregister_device(const char *device_name,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **user)
{
	struct dso_state *state = *user;

	dmeventd_lvm2_exit_with_pool(state);
	log_info("No longer monitoring snapshot %s.", device_name);

	return 1;
}
