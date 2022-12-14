/*
 * Linux Security Module for Chromium OS
 *
 * Copyright 2011 Google Inc. All Rights Reserved
 *
 * Authors:
 *      Stephan Uphoff  <ups@google.com>
 *      Kees Cook       <keescook@chromium.org>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "Chromium OS LSM: " fmt

#include <asm/syscall.h>
#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/lsm_hooks.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>	/* for nameidata_get_total_link_count */
#include <linux/path.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/sched.h>	/* current and other task related stuff */
#include <linux/security.h>
#include <linux/shmem_fs.h>
#include <linux/uaccess.h>
#include <uapi/linux/fs.h>
#include <uapi/linux/nvme_ioctl.h>
#include "inode_mark.h"
#include "utils.h"

#define ADMIN_GET_LOG_PAGE	0x02
#define ADMIN_IDENTITY		0x06
#define ADMIN_SET_FEATURE	0x09
#define ADMIN_GET_FEATURE	0x0A
#define ADMIN_ASYNC_EVENT	0x0C
#define ADMIN_FW_COMMIT	0x10
#define ADMIN_FW_DL		0x11
#define ADMIN_SELF_TEST	0x14
#define ADMIN_KEEP_ALIVE	0x18

#if defined(CONFIG_SECURITY_CHROMIUMOS_NO_UNPRIVILEGED_UNSAFE_MOUNTS) || \
	defined(CONFIG_SECURITY_CHROMIUMOS_NO_SYMLINK_MOUNT)
static void report(const char *origin, const struct path *path, char *operation)
{
	char *alloced = NULL, *cmdline;
	char *pathname; /* Pointer to either static string or "alloced". */

	if (!path)
		pathname = "<unknown>";
	else {
		/* We will allow 11 spaces for ' (deleted)' to be appended */
		alloced = pathname = kmalloc(PATH_MAX+11, GFP_KERNEL);
		if (!pathname)
			pathname = "<no_memory>";
		else {
			pathname = d_path(path, pathname, PATH_MAX+11);
			if (IS_ERR(pathname))
				pathname = "<too_long>";
			else {
				pathname = printable(pathname, PATH_MAX+11);
				kfree(alloced);
				alloced = pathname;
			}
		}
	}

	cmdline = printable_cmdline(current);

	pr_notice("%s %s obj=%s pid=%d cmdline=%s\n", origin,
		  operation, pathname, task_pid_nr(current), cmdline);

	kfree(cmdline);
	kfree(alloced);
}
#endif

static int chromiumos_security_sb_mount(const char *dev_name,
					const struct path *path,
					const char *type, unsigned long flags,
					void *data)
{
#ifdef CONFIG_SECURITY_CHROMIUMOS_NO_SYMLINK_MOUNT
	if (nameidata_get_total_link_count()) {
		report("sb_mount", path, "Mount path with symlinks prohibited");
		pr_notice("sb_mount dev=%s type=%s flags=%#lx\n",
			  dev_name, type, flags);
		return -ELOOP;
	}
#endif

#ifdef CONFIG_SECURITY_CHROMIUMOS_NO_UNPRIVILEGED_UNSAFE_MOUNTS
	if ((!(flags & (MS_BIND | MS_MOVE | MS_SHARED | MS_PRIVATE | MS_SLAVE |
			MS_UNBINDABLE)) ||
	     ((flags & MS_REMOUNT) && (flags & MS_BIND))) &&
	    !capable(CAP_SYS_ADMIN)) {
		int required_mnt_flags = MNT_NOEXEC | MNT_NOSUID | MNT_NODEV;

		if (flags & MS_REMOUNT) {
			/*
			 * If this is a remount, we only require that the
			 * requested flags are a superset of the original mount
			 * flags. In addition, using nosymfollow is not
			 * initially required, but remount is not allowed to
			 * remove it.
			 */
			required_mnt_flags |= MNT_NOSYMFOLLOW;
			required_mnt_flags &= path->mnt->mnt_flags;
		}
		/*
		 * The three flags we are interested in disallowing in
		 * unprivileged user namespaces (MS_NOEXEC, MS_NOSUID, MS_NODEV)
		 * cannot be modified when doing a bind-mount. The kernel
		 * attempts to dispatch calls to do_mount() within
		 * fs/namespace.c in the following order:
		 *
		 * * If the MS_REMOUNT flag is present, it calls do_remount().
		 *   When MS_BIND is also present, it only allows to modify the
		 *   per-mount flags, which are copied into
		 *   |required_mnt_flags|.  Otherwise it bails in the absence of
		 *   the CAP_SYS_ADMIN in the init ns.
		 * * If the MS_BIND flag is present, the only other flag checked
		 *   is MS_REC.
		 * * If any of the mount propagation flags are present
		 *   (MS_SHARED, MS_PRIVATE, MS_SLAVE, MS_UNBINDABLE),
		 *   flags_to_propagation_type() filters out any additional
		 *   flags.
		 * * If MS_MOVE flag is present, all other flags are ignored.
		 */
		if ((required_mnt_flags & MNT_NOEXEC) && !(flags & MS_NOEXEC)) {
			report("sb_mount", path,
			       "Mounting a filesystem with 'exec' flag requires CAP_SYS_ADMIN in init ns");
			pr_notice("sb_mount dev=%s type=%s flags=%#lx\n",
				  dev_name, type, flags);
			return -EPERM;
		}
		if ((required_mnt_flags & MNT_NOSUID) && !(flags & MS_NOSUID)) {
			report("sb_mount", path,
			       "Mounting a filesystem with 'suid' flag requires CAP_SYS_ADMIN in init ns");
			pr_notice("sb_mount dev=%s type=%s flags=%#lx\n",
				  dev_name, type, flags);
			return -EPERM;
		}
		if ((required_mnt_flags & MNT_NODEV) && !(flags & MS_NODEV) &&
		    strcmp(type, "devpts")) {
			report("sb_mount", path,
			       "Mounting a filesystem with 'dev' flag requires CAP_SYS_ADMIN in init ns");
			pr_notice("sb_mount dev=%s type=%s flags=%#lx\n",
				  dev_name, type, flags);
			return -EPERM;
		}
	}
#endif

	return 0;
}

/*
 * NOTE: The WARN() calls will emit a warning in cases of blocked symlink
 * traversal attempts. These will show up in kernel warning reports
 * collected by the crash reporter, so we have some insight on spurious
 * failures that need addressing.
 */
static int chromiumos_security_inode_follow_link(struct dentry *dentry,
						 struct inode *inode, bool rcu)
{
	static char accessed_path[PATH_MAX];
	enum chromiumos_inode_security_policy policy;

	policy = chromiumos_get_inode_security_policy(
		dentry, inode,
		CHROMIUMOS_SYMLINK_TRAVERSAL);

	WARN(policy == CHROMIUMOS_INODE_POLICY_BLOCK,
	     "Blocked symlink traversal for path %x:%x:%s (see https://goo.gl/8xICW6 for context and rationale)\n",
	     MAJOR(dentry->d_sb->s_dev), MINOR(dentry->d_sb->s_dev),
	     dentry_path(dentry, accessed_path, PATH_MAX));

	return policy == CHROMIUMOS_INODE_POLICY_BLOCK ? -EACCES : 0;
}

static int chromiumos_security_file_ioctl(
	struct file *file, unsigned int cmd, unsigned long arg)
{
	static char accessed_path[PATH_MAX];
	enum chromiumos_inode_security_policy policy;
	struct dentry *dentry = file->f_path.dentry;
	bool should_check = false;

	if (!S_ISCHR(file->f_inode->i_mode) &&
	    !S_ISBLK(file->f_inode->i_mode)) {
		return 0;
	}

	// Filter direct IO related ioctls
	if (cmd == NVME_IOCTL_SUBMIT_IO || cmd == NVME_IOCTL_IO_CMD)
		should_check = true;

	// Filter IO opcodes in admin cmd
	if (cmd == NVME_IOCTL_ADMIN_CMD) {
		struct nvme_passthru_cmd __user *ucmd = (void __user *)arg;
		struct nvme_passthru_cmd cmd;

		if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
			return -EFAULT;

		// Allow list of admin op codes.
		if (cmd.opcode != ADMIN_GET_LOG_PAGE &&
		    cmd.opcode != ADMIN_IDENTITY &&
		    cmd.opcode != ADMIN_SET_FEATURE &&
		    cmd.opcode != ADMIN_GET_FEATURE &&
		    cmd.opcode != ADMIN_ASYNC_EVENT &&
		    cmd.opcode != ADMIN_FW_COMMIT &&
		    cmd.opcode != ADMIN_FW_DL &&
		    cmd.opcode != ADMIN_SELF_TEST &&
		    cmd.opcode != ADMIN_KEEP_ALIVE) {
			should_check = true;
		}
	}

	if (!should_check)
		return 0;

	policy = chromiumos_get_inode_security_policy(
		dentry, dentry->d_inode,
		CHROMIUMOS_NVME_IOCTL_IO_ACCESS);

	/*
	 * Emit a warning in cases of blocked chrdev ioctl attempts. These will
	 * show up in kernel warning reports collected by the crash reporter,
	 * so we have some insight on spurious failures that need addressing.
	 */
	WARN(policy == CHROMIUMOS_INODE_POLICY_BLOCK,
	     "Blocked ioctl '%d' access for path %x:%x:%s\n (see https://goo.gl/8xICW6 for context and rationale)\n",
	     cmd, MAJOR(dentry->d_sb->s_dev), MINOR(dentry->d_sb->s_dev),
	     dentry_path(dentry, accessed_path, PATH_MAX));

	return policy == CHROMIUMOS_INODE_POLICY_BLOCK ? -EACCES : 0;
}

static int chromiumos_security_file_open(struct file *file)
{
	static char accessed_path[PATH_MAX];
	enum chromiumos_inode_security_policy policy;
	struct dentry *dentry = file->f_path.dentry;

	if (S_ISFIFO(file->f_inode->i_mode)) {
		policy = chromiumos_get_inode_security_policy(
			dentry, dentry->d_inode,
			CHROMIUMOS_FIFO_ACCESS);
	} else if (S_ISBLK(file->f_inode->i_mode)) {
		policy = chromiumos_get_inode_security_policy(
			dentry, dentry->d_inode,
			CHROMIUMOS_BLKDEV_ACCESS);
	} else {
		return 0;
	}

	/*
	 * Emit a warning in cases of blocked fifo access attempts. These will
	 * show up in kernel warning reports collected by the crash reporter,
	 * so we have some insight on spurious failures that need addressing.
	 */
	WARN(policy == CHROMIUMOS_INODE_POLICY_BLOCK,
	     "Blocked fifo access for path %x:%x:%s\n (see https://goo.gl/8xICW6 for context and rationale)\n",
	     MAJOR(dentry->d_sb->s_dev), MINOR(dentry->d_sb->s_dev),
	     dentry_path(dentry, accessed_path, PATH_MAX));

	return policy == CHROMIUMOS_INODE_POLICY_BLOCK ? -EACCES : 0;
}

/*
 * This hook inspects the string pointed to by the first parameter, looking for
 * the "nosymfollow" mount option.  Since the behavior is now implemented with
 * the MS_NOSYMFOLLOW flag, the "nosymfollow" option will be stripped from the
 * mount string if it is encountered.
 */
static int chromiumos_sb_copy_data(char *orig, char *copy)
{
	char *orig_copy;
	char *orig_copy_cur;
	char *option;
	size_t offset = 0;
	bool found = false;

	if (!orig || *orig == 0)
		return 0;

	orig_copy = alloc_secdata();
	if (!orig_copy)
		return -ENOMEM;
	strncpy(orig_copy, orig, PAGE_SIZE);

	memset(orig, 0, strlen(orig));

	orig_copy_cur = orig_copy;
	while (orig_copy_cur) {
		option = strsep(&orig_copy_cur, ",");
		/*
		 * Remove the option so that filesystems won't see it.
		 * do_mount() has already forced the MS_NOSYMFOLLOW flag on
		 * if it found this option, so no other action is needed.
		 */
		if (strcmp(option, "nosymfollow") == 0) {
			if (found) /* Found multiple times. */
				return -EINVAL;
			found = true;
		} else {
			if (offset > 0) {
				orig[offset] = ',';
				offset++;
			}
			strcpy(orig + offset, option);
			offset += strlen(option);
		}
	}

	if (found)
		pr_notice("nosymfollow option should be changed to MS_NOSYMFOLLOW flag.");

	free_secdata(orig_copy);
	return 0;
}

static int chromiumos_bprm_creds_for_exec(struct linux_binprm *bprm)
{
	struct file *file = bprm->file;

	if (shmem_file(file))
		return -EACCES;

	return 0;
}

static int chromiumos_bpf(int cmd, union bpf_attr *attr, unsigned int size)
{
	return -EACCES;
}

static struct security_hook_list chromiumos_security_hooks[] = {
	LSM_HOOK_INIT(sb_mount, chromiumos_security_sb_mount),
	LSM_HOOK_INIT(inode_follow_link, chromiumos_security_inode_follow_link),
	LSM_HOOK_INIT(file_open, chromiumos_security_file_open),
	LSM_HOOK_INIT(file_ioctl, chromiumos_security_file_ioctl),
	LSM_HOOK_INIT(sb_copy_data, chromiumos_sb_copy_data),
	LSM_HOOK_INIT(bprm_creds_for_exec, chromiumos_bprm_creds_for_exec),
	LSM_HOOK_INIT(bpf, chromiumos_bpf),
};

static int __init chromiumos_security_init(void)
{
	security_add_hooks(chromiumos_security_hooks,
			   ARRAY_SIZE(chromiumos_security_hooks), "chromiumos");

	pr_info("enabled");

	return 0;
}
security_initcall(chromiumos_security_init);
