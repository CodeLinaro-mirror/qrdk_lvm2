/*
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/device/dev-type.h"
#include "lib/device/filesystem.h"
#include "lib/display/display.h"
#include "lib/misc/lvm-exec.h"
#include "lib/activate/dev_manager.h"
#include "lib/commands/toolcontext.h"
#include "lib/config/config.h"

#include <dirent.h>
#include <mntent.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/fs.h>

static const char *_get_lvresize_fs_helper_path(struct cmd_context *cmd)
{
	const char *path = getenv("LVRESIZE_FS_HELPER_PATH");

	if (!path)
		path = find_config_tree_str(cmd, global_lvresize_fs_helper_executable_CFG, NULL);

	return path;
}

/*
 * Set the path of the dm-crypt device, i.e. /dev/dm-N, that is using the LV.
 */
static int _get_crypt_path(dev_t lv_devt, char *lv_path, char *crypt_path)
{
	char holders_path[PATH_MAX];
	char *holder_name;
	DIR *dr;
	struct dirent *de;
	int ret = 0;

	if (dm_snprintf(holders_path, sizeof(holders_path), "%sdev/block/%u:%u/holders",
			dm_sysfs_dir(), MAJOR(lv_devt), MINOR(lv_devt)) < 0) {
		log_error("Couldn't create holder path for %s.", lv_path);
		return 0;
	}

	/* If the crypt dev is not active, there will be no LV holder. */
	if (!(dr = opendir(holders_path))) {
		if (errno == ENOENT)
			log_error("Missing %s for %s.", holders_path, lv_path);
		else
			log_error("Cannot open %s.", holders_path);
		return 0;
	}

	while ((de = readdir(dr))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		holder_name = de->d_name;

		if (strncmp(holder_name, "dm", 2)) {
			log_error("Unrecognized holder %s of %s", holder_name, lv_path);
			ret = 0;
			break;
		}

		/* We could read the holder's dm uuid to verify it's a crypt dev. */

		if (dm_snprintf(crypt_path, PATH_MAX, "/dev/%s", holder_name) < 0) {
			ret = 0;
			stack;
			break;
		}
		ret = 1;
		break;
	}
	if (closedir(dr))
		log_sys_debug("closedir", holders_path);

	if (ret)
		log_debug("Found holder %s of %s.", crypt_path, lv_path);
	else
		log_debug("No holder in %s", holders_path);
	return ret;
}

int lv_crypt_is_active(struct cmd_context *cmd, char *lv_path)
{
	char crypt_path[PATH_MAX] = { 0 };
	struct stat st_lv;

	if (stat(lv_path, &st_lv) < 0) {
		log_error("Failed to get LV path %s", lv_path);
		return 0;
	}

	return _get_crypt_path(st_lv.st_rdev, lv_path, crypt_path);
}

/*
 * A mount table entry lvm can compare: of the given filesystem type, with
 * both the source and the mount point given as absolute paths.  A source
 * that is not a path (e.g. "tmpfs" or "proc") names no device.
 */
static int _mnt_entry_is_comparable(const struct mntent *me, const char *fstype)
{
	if (strcmp(me->mnt_type, fstype))
		return 0;
	if (me->mnt_dir[0] != '/' || me->mnt_fsname[0] != '/')
		return 0;
	return 1;
}

static int _fs_get_mnt(struct fs_info *fsi, dev_t devt)
{
	struct stat stme;
	FILE *fme = NULL;
	struct mntent me;
	char *line;
	/*
	 * st_dev of mnt_dir in btrfs is an anonymous device number,
	 * use mnt_fsname and st_rdev instead.
	 */
	int is_btrfs = !strcmp(fsi->fstype, "btrfs");

	/*
	 * Note: used swap devices are not considered as mount points,
	 * hence they're not listed in /proc/self/mounts, we'd need to read
	 * the /proc/swaps instead. We don't need it at this moment though,
	 * but if we do once, read the /proc/swaps here if fsi->fstype == "swap".
	 */
	if (!(line = malloc(MOUNTS_LINE_MAX))) {
		log_error("Failed to allocate the mount table line buffer.");
		return 0;
	}

	if (!(fme = setmntent(PROC_MOUNTS, "r"))) {
		log_error("Cannot read %s: %s.", PROC_MOUNTS, strerror(errno));
		free(line);
		return 0;
	}

	while (getmntent_r(fme, &me, line, MOUNTS_LINE_MAX)) {
		const char *stat_path = is_btrfs ? me.mnt_fsname : me.mnt_dir;

		if (!_mnt_entry_is_comparable(&me, fsi->fstype))
			continue;
		if (stat(stat_path, &stme) < 0) {
			log_sys_debug("stat", stat_path);
			continue;
		}
		if ((is_btrfs ? stme.st_rdev : stme.st_dev) != devt)
			continue;

		fsi->mounted = 1;

		log_debug("fs_get_info %s is mounted \"%s\"", fsi->fs_dev_path, me.mnt_dir);
		dm_strncpy(fsi->mount_dir, me.mnt_dir, sizeof(fsi->mount_dir));
		break; /* found mount */
	}
	endmntent(fme);
	free(line);

	return 1;
}

static int _btrfs_get_mnt(struct fs_info *fsi, dev_t fs_devt)
{
	char devices_path[PATH_MAX];
	char rdev_path[PATH_MAX];
	unsigned major, minor;
	dev_t devt;
	char buffer[16];
	char *device_name;
	DIR *dr;
	struct dirent *de;
	int ret = 1;
	int fd = -1;
	ssize_t r;
	int found = 0;

	/* For a mounted btrfs, there will be a sys dir like /sys/fs/btrfs/$uuid/devices */
	if (dm_snprintf(devices_path, sizeof(devices_path), "%sfs/btrfs/%s/devices",
			dm_sysfs_dir(), fsi->uuid) < 0) {
		log_error("Couldn't create btrfs devices path for %s.", fsi->fs_dev_path);
		return 0;
	}

	/* btrfs module is not available or the device is not mounted */
	if (!(dr = opendir(devices_path))) {
		if (errno == ENOENT) {
			fsi->mounted = 0;
			return 1;
		}
		log_sys_error("opendir", devices_path);
		return 0;
	}

	/*
	 * Here iterates entries under /sys/fs/btrfs/$uuid/devices and read devt.
	 * There is only one mnt entry per mounted fs even it's a multi-devices fs.
	 * So also call _fs_get_mnt for every devices to find a matched mount point.
	 */
	while ((de = readdir(dr))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		device_name = de->d_name;

		if (dm_snprintf(rdev_path, sizeof(rdev_path), "%s/%s/dev",
				 devices_path, device_name) < 0) {
				log_error("Couldn't create rdev path for %s.", fsi->fs_dev_path);
				ret = 0;
				break;
		}

		if ((fd = open(rdev_path, O_RDONLY)) < 0) {
			log_sys_debug("open", rdev_path);
			ret = 0;
			break;
		}

		r = read(fd, buffer, sizeof(buffer));

		if (r <= 0) {
			ret = 0;
			log_sys_debug("read", rdev_path);
		}

		if (close(fd))
			log_sys_debug("close", rdev_path);
		fd = -1;

		if (r <= 0)
			break;

		buffer[r - 1] = 0;

		if (sscanf(buffer, "%u:%u", &major, &minor) != 2) {
			ret = 0;
			log_sys_debug("sscanf", rdev_path);
			break;
		}

		devt = MKDEV(major, minor);
		if (devt == fs_devt)
			found = 1;

		if (fsi->mount_dir[0] == 0)
			_fs_get_mnt(fsi, devt);

		if (found && fsi->mount_dir[0])
			break;
	}

	if (closedir(dr))
		log_sys_debug("closedir", devices_path);

	/*
	 * The uuid read from the device is not unique (copy, snapshot or
	 * crafted superblock), and _fs_get_mnt reports the mount of any
	 * device of the mounted btrfs with that uuid.  It is the mount of
	 * this fs only if its device is one of the devices listed in sysfs.
	 */
	if (!found) {
		if (fsi->mount_dir[0])
			log_debug("fs_get_info %s is not a device of btrfs %s mounted \"%s\"",
				  fsi->fs_dev_path, fsi->uuid, fsi->mount_dir);
		fsi->mount_dir[0] = 0;
	}

	fsi->mounted = found;

	if (fsi->mounted && fsi->mount_dir[0] == 0) {
		log_error("Couldn't get mount point for %s.", fsi->fs_dev_path);
		ret = 0;
	}

	return ret;
}


int fs_get_info(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX] = { 0 };
	struct stat st_lv;
	struct stat st_crypt;
	struct stat st_top;
	struct fs_info info;
	int fd;
	int ret;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir,
			lv->vg->name, lv->name) < 0) {
		log_error("Couldn't create LV path for %s.", display_lvname(lv));
		return 0;
	}

	if (stat(lv_path, &st_lv) < 0) {
		log_error("Failed to get LV path %s", lv_path);
		return 0;
	}

	memset(&info, 0, sizeof(info));

	if (!fs_get_blkid(lv_path, &info)) {
		log_error("No file system info from blkid for %s", display_lvname(lv));
		return 0;
	}

	if (info.nofs) {
		fsi->nofs = 1;
		return 1;
	}

	/*
	 * If there's a LUKS dm-crypt layer over the LV, then
	 * return fs info from that layer, setting needs_crypt
	 * to indicate a crypt layer between the fs and LV.
	 */
	if (!strcmp(info.fstype, "crypto_LUKS")) {
		if (!_get_crypt_path(st_lv.st_rdev, lv_path, crypt_path)) {
			log_error("Cannot find active LUKS dm-crypt device using %s.",
				  display_lvname(lv));
			return 0;
		}

		memset(&info, 0, sizeof(info));

		log_print_unless_silent("Checking crypt device %s on LV %s.",
					crypt_path, display_lvname(lv));

		if ((fd = open(crypt_path, O_RDONLY)) < 0) {
			log_error("Failed to open crypt path %s.", crypt_path);
			return 0;
		}

		if ((ret = fstat(fd, &st_crypt)) < 0)
			log_sys_error("fstat", crypt_path);
		else if ((ret = ioctl(fd, BLKGETSIZE64, &info.crypt_dev_size_bytes)) < 0)
			log_error("Failed to get crypt device size %s.", crypt_path);

		if (close(fd))
			log_sys_debug("close", crypt_path);

		if (ret < 0)
			return 0;

		if (!fs_get_blkid(crypt_path, &info)) {
			log_error("No file system info from blkid for dm-crypt device %s on LV %s.",
				  crypt_path, display_lvname(lv));
			return 0;
		}
		*fsi = info;
		fsi->needs_crypt = 1;
		fsi->crypt_devt = st_crypt.st_rdev;
		memcpy(fsi->fs_dev_path, crypt_path, PATH_MAX);
		st_top = st_crypt;

		if (!get_crypt_table_offset(st_crypt.st_rdev, &fsi->crypt_offset_bytes)) {
			log_error("Failed to get crypt data offset.");
			return 0;
		}
	} else {
		*fsi = info;
		memcpy(fsi->fs_dev_path, lv_path, PATH_MAX);
		st_top = st_lv;
	}

	if (!strcmp(fsi->fstype, "btrfs"))
		ret = _btrfs_get_mnt(fsi, st_top.st_rdev);
	else
		ret = _fs_get_mnt(fsi, st_top.st_rdev);

	/* blkid FSLASTBLOCK may be incorrect for mounted xfs */
	if (fsi->mounted && !strcmp(fsi->fstype, "xfs")) {
		if (!(ret = fs_xfs_update_size_mounted(cmd, lv, lv_path, fsi)))
			stack;
	}

	/*
	 * The mount table only shows mounts that are visible to this command.
	 * An xfs that is still mounted elsewhere (e.g. in another mount
	 * namespace) has the same possibly incorrect FSLASTBLOCK, and no
	 * mount point to get the size from, so the size is not known.
	 */
	if (ret && !fsi->mounted && !strcmp(fsi->fstype, "xfs") &&
	    dm_device_has_mounted_fs(MAJOR(st_top.st_rdev), MINOR(st_top.st_rdev))) {
		log_warn("WARNING: xfs on %s is in use, but no mount point was found, file system size is unknown.",
			 fsi->fs_dev_path);
		fsi->fs_last_byte = 0;
	}

	fsi->unmounted = !fsi->mounted;
	return ret;
}

/*
 * Check that the mount table still names this LV's device the way lvm does.
 * A rename of a mounted LV leaves the old name in the table, and a table
 * that cannot be read or understood leaves no name at all, so both must
 * stop the resize instead of letting it run blind.
 *
 * Returns 0 if the check cannot be made or an entry names the device
 * differently, 1 if every entry of the device names it as lvm does.
 */
int fs_check_mount_names(struct cmd_context *cmd, struct logical_volume *lv, char *lv_path, char *fstype)
{
	/* One block for the mount table line and the paths compared with it. */
	struct mount_table_paths {
		char dm_devpath[PATH_MAX];
		char dm_devpath_resolved[PATH_MAX];
		char source_resolved[PATH_MAX];
		char line[MOUNTS_LINE_MAX];
	} *paths;
	FILE *mounts;
	struct mntent me;
	char *dm_name;
	struct stat st_lv, st_mount;
	int r = 0;

	if (stat(lv_path, &st_lv) < 0) {
		log_error("Failed to get LV path %s", lv_path);
		return 0;
	}

	if (!(dm_name = dm_build_dm_name(cmd->mem, lv->vg->name, lv->name, NULL)))
		return_0;

	if (!(paths = malloc(sizeof(*paths)))) {
		log_error("Failed to allocate the mount table buffers.");
		return 0;
	}

	if (dm_snprintf(paths->dm_devpath, sizeof(paths->dm_devpath), "%s/%s", dm_dir(), dm_name) < 0)
		goto_out;

	if (!(mounts = setmntent(PROC_MOUNTS, "r"))) {
		log_error("Cannot read %s: %s.", PROC_MOUNTS, strerror(errno));
		goto out;
	}

	/* The mount source can retain the old name after an LV rename. */
	while (getmntent_r(mounts, &me, paths->line, sizeof(paths->line))) {
		if (!_mnt_entry_is_comparable(&me, fstype))
			continue;
		if (!strcmp(me.mnt_fsname, paths->dm_devpath))
			continue;
		if (stat(me.mnt_dir, &st_mount) < 0 || st_mount.st_dev != st_lv.st_rdev)
			continue;

		/* A symlink to the current device path is not a rename. */
		if (realpath(paths->dm_devpath, paths->dm_devpath_resolved) &&
		    realpath(me.mnt_fsname, paths->source_resolved) &&
		    !strcmp(paths->dm_devpath_resolved, paths->source_resolved))
			continue;

		log_error("Cannot resize file system on %s: mount at %s still names %s.",
			  paths->dm_devpath, me.mnt_dir, me.mnt_fsname);
		goto end;
	}
	r = 1; /* Not renamed */

end:
	endmntent(mounts);
out:
	free(paths);

	return r;
}

#define FS_CMD_MAX_ARGS 16

int crypt_resize_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi)
{
	char crypt_path[PATH_MAX];
	char newsize_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (dm_snprintf(newsize_str, sizeof(newsize_str), "%llu", (unsigned long long)fsi->new_size_bytes) < 0)
		return_0;

	if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%u", MINOR(fsi->crypt_devt)) < 0)
		return_0;

	argv[0] = _get_lvresize_fs_helper_path(cmd);
	argv[++args] = "--cryptresize";
	argv[++args] = "--cryptpath";
	argv[++args] = crypt_path;
	argv[++args] = "--newsizebytes";
	argv[++args] = newsize_str;
	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to resize crypt dev with lvresize_fs_helper.");
		return 0;
	}

	return 1;
}

/*
 * The helper script does the following steps for reduce:
 * devpath = $cryptpath ? $cryptpath : $lvpath
 * if needs_unmount
 * 	umount $mountdir
 * if needs_fsck
 * 	e2fsck -f -p $devpath
 * if needs_mount
 * 	mount $devpath $tmpdir
 * if $fstype == "ext"
 * 	resize2fs $devpath $newsize_kb
 * if needs_crypt
 * 	cryptsetup resize --size $newsize_sectors $cryptpath
 *
 * Note: when a crypt layer is included, new_size_bytes is smaller
 * than newsize_bytes_lv because of the crypt header.
 */

int fs_reduce_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi, char *fsmode)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX];
	char newsize_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	char *devpath;
	int args = 0;
	int status;

	if (dm_snprintf(newsize_str, sizeof(newsize_str), "%llu", (unsigned long long)fsi->new_size_bytes) < 0)
		return_0;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir, lv->vg->name, lv->name) < 0)
		return_0;

	argv[0] = _get_lvresize_fs_helper_path(cmd);
	argv[++args] = "--fsreduce";
	argv[++args] = "--fstype";
	argv[++args] = fsi->fstype;
	argv[++args] = "--lvpath";
	argv[++args] = lv_path;

	if (fsi->new_size_bytes) {
		argv[++args] = "--newsizebytes";
		argv[++args] = newsize_str;
	}
	if (fsi->mounted) {
		argv[++args] = "--mountdir";
		argv[++args] = fsi->mount_dir;
	}

	if (fsi->needs_unmount)
		argv[++args] = "--unmount";
	if (fsi->needs_mount)
		argv[++args] = "--mount";
	if (fsi->needs_fsck)
		argv[++args] = "--fsck";

	if (fsi->needs_crypt) {
		if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%u", MINOR(fsi->crypt_devt)) < 0)
			return_0;
		argv[++args] = "--cryptresize";
		argv[++args] = "--cryptpath";
		argv[++args] = crypt_path;
	}

	/*
	 * fsmode manage means the fs should be remounted after
	 * resizing if it was unmounted.
	 */
	if (fsi->needs_unmount && !strcmp(fsmode, "manage"))
		argv[++args] = "--remount";

	argv[++args] = NULL;

	devpath = fsi->needs_crypt ? crypt_path : (char *)display_lvname(lv);

	log_print_unless_silent("Reducing file system %s to %s (%llu bytes) on %s...",
				fsi->fstype, display_size(cmd, fsi->new_size_bytes/512),
				(unsigned long long)fsi->new_size_bytes, devpath);

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to reduce file system with lvresize_fs_helper.");
		return 0;
	}

	log_print_unless_silent("Reduced file system %s on %s.", fsi->fstype, devpath);

	return 1;
}

/*
 * The helper script does the following steps for extend:
 * devpath = $cryptpath ? $cryptpath : $lvpath
 * if needs_unmount
 * 	umount $mountdir
 * if needs_fsck
 * 	e2fsck -f -p $devpath
 * if needs_crypt
 * 	cryptsetup resize $cryptpath
 * if needs_mount
 * 	mount $devpath $tmpdir
 * if $fstype == "ext"
 * 	resize2fs $devpath
 * if $fstype == "xfs"
 * 	xfs_growfs $devpath
 *
 * Note: when a crypt layer is included, new_size_bytes is smaller
 * than newsize_bytes_lv because of the crypt header.
 */

int fs_extend_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		     char *fsmode)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX];
	char newsize_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	char *devpath;
	int args = 0;
	int status;

	if (dm_snprintf(newsize_str, sizeof(newsize_str), "%llu", (unsigned long long)fsi->new_size_bytes) < 0)
		return_0;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir, lv->vg->name, lv->name) < 0)
		return_0;

	argv[0] = _get_lvresize_fs_helper_path(cmd);
	argv[++args] = "--fsextend";
	argv[++args] = "--fstype";
	argv[++args] = fsi->fstype;
	argv[++args] = "--lvpath";
	argv[++args] = lv_path;

	if (fsi->new_size_bytes) {
		argv[++args] = "--newsizebytes";
		argv[++args] = newsize_str;
	}
	if (fsi->mounted) {
		argv[++args] = "--mountdir";
		argv[++args] = fsi->mount_dir;
	}

	if (fsi->needs_unmount)
		argv[++args] = "--unmount";
	if (fsi->needs_mount)
		argv[++args] = "--mount";
	if (fsi->needs_fsck)
		argv[++args] = "--fsck";

	if (fsi->needs_crypt) {
		if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%u", MINOR(fsi->crypt_devt)) < 0)
			return_0;
		argv[++args] = "--cryptresize";
		argv[++args] = "--cryptpath";
		argv[++args] = crypt_path;
	}

	/*
	 * fsmode manage means the fs should be remounted after
	 * resizing if it was unmounted.
	 */
	if (fsi->needs_unmount && !strcmp(fsmode, "manage"))
		argv[++args] = "--remount";

	argv[++args] = NULL;

	devpath = fsi->needs_crypt ? crypt_path : (char *)display_lvname(lv);

	log_print_unless_silent("Extending file system %s to %s (%llu bytes) on %s...",
				fsi->fstype, display_size(cmd, fsi->new_size_bytes/512),
				(unsigned long long)fsi->new_size_bytes, devpath);

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to extend file system with lvresize_fs_helper.");
		return 0;
	}

	log_print_unless_silent("Extended file system %s on %s.", fsi->fstype, devpath);

	return 1;
}
