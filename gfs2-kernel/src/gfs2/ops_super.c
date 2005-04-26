/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/vmalloc.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>

#include "gfs2.h"
#include "dio.h"
#include "glock.h"
#include "inode.h"
#include "lm.h"
#include "log.h"
#include "ops_super.h"
#include "page.h"
#include "proc.h"
#include "quota.h"
#include "recovery.h"
#include "rgrp.h"
#include "super.h"

/**
 * gfs2_write_inode - Make sure the inode is stable on the disk
 * @inode: The inode
 * @sync: synchronous write flag
 *
 * Returns: errno
 */

static int
gfs2_write_inode(struct inode *inode, int sync)
{
	ENTER(G2FN_WRITE_INODE)
	struct gfs2_inode *ip = get_v2ip(inode);

	atomic_inc(&ip->i_sbd->sd_ops_super);

	if (ip && sync)
		gfs2_log_flush_glock(ip->i_gl);

	RETURN(G2FN_WRITE_INODE, 0);
}

/**
 * gfs2_put_inode - put an inode
 * @inode: The inode
 *
 * If i_nlink is zero, any dirty data for the inode is thrown away.
 * If a process on another machine has the file open, it may need that
 * data.  So, sync it out.
 */

static void
gfs2_put_inode(struct inode *inode)
{
	ENTER(G2FN_PUT_INODE)
	struct gfs2_sbd *sdp = get_v2sdp(inode->i_sb);
	struct gfs2_inode *ip = get_v2ip(inode);

	atomic_inc(&sdp->sd_ops_super);

	if (ip &&
	    !inode->i_nlink &&
	    S_ISREG(inode->i_mode) &&
	    !sdp->sd_args.ar_localcaching)
		gfs2_sync_page_i(inode, DIO_START | DIO_WAIT);

	RET(G2FN_PUT_INODE);
}

/**
 * gfs2_put_super - Unmount the filesystem
 * @sb: The VFS superblock
 *
 */

static void
gfs2_put_super(struct super_block *sb)
{
	ENTER(G2FN_PUT_SUPER)
	struct gfs2_sbd *sdp = get_v2sdp(sb);
	int error;

        if (!sdp)
                RET(G2FN_PUT_SUPER);

	atomic_inc(&sdp->sd_ops_super);

	gfs2_proc_fs_del(sdp);

	/*  Unfreeze the filesystem, if we need to  */

	down(&sdp->sd_freeze_lock);
	if (sdp->sd_freeze_count)
		gfs2_glock_dq_uninit(&sdp->sd_freeze_gh);
	up(&sdp->sd_freeze_lock);

	/*  Kill off the inode thread  */
	down(&sdp->sd_thread_lock);
	clear_bit(SDF_INODED_RUN, &sdp->sd_flags);
	wake_up_process(sdp->sd_inoded_process);
	up(&sdp->sd_thread_lock);
	wait_for_completion(&sdp->sd_thread_completion);

	/*  Kill off the quota thread  */
	down(&sdp->sd_thread_lock);
	clear_bit(SDF_QUOTAD_RUN, &sdp->sd_flags);
	wake_up_process(sdp->sd_quotad_process);
	up(&sdp->sd_thread_lock);
	wait_for_completion(&sdp->sd_thread_completion);

	/*  Kill off the log thread  */
	down(&sdp->sd_thread_lock);
	clear_bit(SDF_LOGD_RUN, &sdp->sd_flags);
	wake_up_process(sdp->sd_logd_process);
	up(&sdp->sd_thread_lock);
	wait_for_completion(&sdp->sd_thread_completion);

	/*  Kill off the recoverd thread  */
	down(&sdp->sd_thread_lock);
	clear_bit(SDF_RECOVERD_RUN, &sdp->sd_flags);
	wake_up_process(sdp->sd_recoverd_process);
	up(&sdp->sd_thread_lock);
	wait_for_completion(&sdp->sd_thread_completion);

	/*  Kill off the glockd threads  */
	clear_bit(SDF_GLOCKD_RUN, &sdp->sd_flags);
	wake_up(&sdp->sd_reclaim_wq);
	while (sdp->sd_glockd_num--)
		wait_for_completion(&sdp->sd_thread_completion);

	/*  Kill off the scand thread  */
	down(&sdp->sd_thread_lock);
	clear_bit(SDF_SCAND_RUN, &sdp->sd_flags);
	wake_up_process(sdp->sd_scand_process);
	up(&sdp->sd_thread_lock);
	wait_for_completion(&sdp->sd_thread_completion);

	if (!test_bit(SDF_ROFS, &sdp->sd_flags)) {
		gfs2_quota_sync(sdp);

		error = gfs2_make_fs_ro(sdp);
		if (error)
			gfs2_io_error(sdp);
	}

	/*  At this point, we're through modifying the disk  */

	/*  Release stuff  */

	gfs2_inode_put(sdp->sd_master_dir);
	gfs2_inode_put(sdp->sd_jindex);
	gfs2_inode_put(sdp->sd_inum_inode);
	gfs2_inode_put(sdp->sd_rindex);
	gfs2_inode_put(sdp->sd_quota_inode);
	gfs2_inode_put(sdp->sd_root_inode);

	gfs2_glock_put(sdp->sd_trans_gl);
	gfs2_glock_put(sdp->sd_rename_gl);

	if (!sdp->sd_args.ar_spectator) {
		gfs2_glock_dq_uninit(&sdp->sd_journal_gh);
		gfs2_glock_dq_uninit(&sdp->sd_jinode_gh);
		gfs2_glock_dq_uninit(&sdp->sd_ir_gh);
		gfs2_glock_dq_uninit(&sdp->sd_ut_gh);
		gfs2_glock_dq_uninit(&sdp->sd_qc_gh);
		gfs2_inode_put(sdp->sd_ir_inode);
		gfs2_inode_put(sdp->sd_ut_inode);
		gfs2_inode_put(sdp->sd_qc_inode);
	}

	gfs2_glock_dq_uninit(&sdp->sd_live_gh);

	gfs2_clear_rgrpd(sdp);
	gfs2_jindex_free(sdp);

	/*  Take apart glock structures and buffer lists  */
	gfs2_gl_hash_clear(sdp, WAIT);

	/*  Unmount the locking protocol  */
	gfs2_lm_unmount(sdp);

	/*  At this point, we're through participating in the lockspace  */

	/*  Get rid of any extra inodes  */
	while (invalidate_inodes(sb))
		yield();

	vfree(sdp);

	set_v2sdp(sb, NULL);

	RET(G2FN_PUT_SUPER);
}

/**
 * gfs2_write_super - disk commit all incore transactions
 * @sb: the filesystem
 *
 * This function is called every time sync(2) is called.
 * After this exits, all dirty buffers and synced.
 */

static void
gfs2_write_super(struct super_block *sb)
{
	ENTER(G2FN_WRITE_SUPER)
	struct gfs2_sbd *sdp = get_v2sdp(sb);
	atomic_inc(&sdp->sd_ops_super);
	gfs2_log_flush(sdp);
	RET(G2FN_WRITE_SUPER);
}

/**
 * gfs2_write_super_lockfs - prevent further writes to the filesystem
 * @sb: the VFS structure for the filesystem
 *
 */

static void
gfs2_write_super_lockfs(struct super_block *sb)
{
	ENTER(G2FN_WRITE_SUPER_LOCKFS)
	struct gfs2_sbd *sdp = get_v2sdp(sb);
	int error;

	atomic_inc(&sdp->sd_ops_super);

	for (;;) {
		error = gfs2_freeze_fs(sdp);
		if (!error)
			break;

		switch (error) {
		case -EBUSY:
			printk("GFS2: fsid=%s: waiting for recovery before freeze\n",
			       sdp->sd_fsname);
			break;

		default:
			printk("GFS2: fsid=%s: error freezing FS: %d\n",
			       sdp->sd_fsname, error);
			break;
		}

		printk("GFS2: fsid=%s: retrying...\n", sdp->sd_fsname);

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ);
	}

	RET(G2FN_WRITE_SUPER_LOCKFS);
}

/**
 * gfs2_unlockfs - reallow writes to the filesystem
 * @sb: the VFS structure for the filesystem
 *
 */

static void
gfs2_unlockfs(struct super_block *sb)
{
	ENTER(G2FN_UNLOCKFS)
	struct gfs2_sbd *sdp = get_v2sdp(sb);

	atomic_inc(&sdp->sd_ops_super);
	gfs2_unfreeze_fs(sdp);

	RET(G2FN_UNLOCKFS);
}

/**
 * gfs2_statfs - Gather and return stats about the filesystem
 * @sb: The superblock
 * @statfsbuf: The buffer
 *
 * Returns: 0 on success or error code
 */

static int
gfs2_statfs(struct super_block *sb, struct kstatfs *buf)
{
	ENTER(G2FN_STATFS)
	struct gfs2_sbd *sdp = get_v2sdp(sb);
	struct gfs2_statfs sg;
	int error;

	atomic_inc(&sdp->sd_ops_super);

	error = gfs2_statfs_i(sdp, &sg, TRUE);
	if (error)
		RETURN(G2FN_STATFS, error);

	memset(buf, 0, sizeof(struct kstatfs));

	buf->f_type = GFS2_MAGIC;
	buf->f_bsize = sdp->sd_sb.sb_bsize;
	buf->f_blocks = sg.sg_total;
	buf->f_bfree = sg.sg_free;
	buf->f_bavail = sg.sg_free;
	buf->f_files = sg.sg_dinodes + sg.sg_free;
	buf->f_ffree = sg.sg_free;
	buf->f_namelen = GFS2_FNAMESIZE;

	RETURN(G2FN_STATFS, 0);
}

/**
 * gfs2_remount_fs - called when the FS is remounted
 * @sb:  the filesystem
 * @flags:  the remount flags
 * @data:  extra data passed in (not used right now)
 *
 * Returns: errno
 */

static int
gfs2_remount_fs(struct super_block *sb, int *flags, char *data)
{
	ENTER(G2FN_REMOUNT_FS)
	struct gfs2_sbd *sdp = get_v2sdp(sb);
	int error = 0;

	atomic_inc(&sdp->sd_ops_super);

	if (*flags & (MS_NOATIME | MS_NODIRATIME))
		set_bit(SDF_NOATIME, &sdp->sd_flags);
	else
		clear_bit(SDF_NOATIME, &sdp->sd_flags);

	if (sdp->sd_args.ar_spectator)
		*flags |= MS_RDONLY;
	else {
		if (*flags & MS_RDONLY) {
			if (!test_bit(SDF_ROFS, &sdp->sd_flags))
				error = gfs2_make_fs_ro(sdp);
		} else if (!(*flags & MS_RDONLY) &&
			   test_bit(SDF_ROFS, &sdp->sd_flags)) {
			error = gfs2_make_fs_rw(sdp);
		}
	}

	/*  Don't let the VFS update atimes.  GFS2 handles this itself. */
	*flags |= MS_NOATIME | MS_NODIRATIME;

	RETURN(G2FN_REMOUNT_FS, error);
}

/**
 * gfs2_clear_inode - Deallocate an inode when VFS is done with it
 * @inode: The VFS inode
 *
 * If there's a GFS2 incore inode structure attached to the VFS inode:
 * --  Detach them from one another.
 * --  Schedule reclaim of GFS2 inode struct, the glock protecting it, and
 *     the associated iopen glock.
 */

static void
gfs2_clear_inode(struct inode *inode)
{
	ENTER(G2FN_CLEAR_INODE)
	struct gfs2_inode *ip = get_v2ip(inode);

	atomic_inc(&get_v2sdp(inode->i_sb)->sd_ops_super);

	if (ip) {
		spin_lock(&ip->i_lock);
		ip->i_vnode = NULL;
		set_v2ip(inode, NULL);
		spin_unlock(&ip->i_lock);

		gfs2_glock_schedule_for_reclaim(ip->i_gl);
		gfs2_inode_put(ip);
	}

	RET(G2FN_CLEAR_INODE);
}

/**
 * gfs2_show_options - Show mount options for /proc/mounts
 * @s: seq_file structure
 * @mnt: vfsmount
 *
 * Returns: 0 on success or error code
 */

static int
gfs2_show_options(struct seq_file *s, struct vfsmount *mnt)
{
	ENTER(G2FN_SHOW_OPTIONS)
	struct gfs2_sbd *sdp = get_v2sdp(mnt->mnt_sb);
	struct gfs2_args *args = &sdp->sd_args;

	atomic_inc(&sdp->sd_ops_super);

	if (args->ar_lockproto[0]) {
		seq_printf(s, ",lockproto=");
		seq_puts(s, args->ar_lockproto);
	}
	if (args->ar_locktable[0]) {
		seq_printf(s, ",locktable=");
		seq_puts(s, args->ar_locktable);
	}
	if (args->ar_hostdata[0]) {
		seq_printf(s, ",hostdata=");
		seq_puts(s, args->ar_hostdata);
	}
	if (args->ar_spectator)
		seq_printf(s, ",spectator");
	if (args->ar_ignore_local_fs)
		seq_printf(s, ",ignore_local_fs");
	if (args->ar_localflocks)
		seq_printf(s, ",localflocks");
	if (args->ar_localcaching)
		seq_printf(s, ",localcaching");
	if (args->ar_oopses_ok)
		seq_printf(s, ",oopses_ok");
	if (args->ar_debug)
		seq_printf(s, ",debug");
	if (args->ar_upgrade)
		seq_printf(s, ",upgrade");
	if (args->ar_num_glockd != GFS2_GLOCKD_DEFAULT)
		seq_printf(s, ",num_glockd=%u", args->ar_num_glockd);
	if (args->ar_posix_acl)
		seq_printf(s, ",acl");
	if (args->ar_quota != GFS2_QUOTA_DEFAULT) {
		char *state;
		switch (args->ar_quota) {
		case GFS2_QUOTA_OFF:
			state = "off";
			break;
		case GFS2_QUOTA_ACCOUNT:
			state = "account";
			break;
		case GFS2_QUOTA_ON:
			state = "on";
			break;
		default:
			state = "unknown";
			break;
		}
		seq_printf(s, ",quota=%s", state);
	}
	if (args->ar_suiddir)
		seq_printf(s, ",suiddir");
	if (args->ar_data != GFS2_DATA_DEFAULT) {
		char *state;
		switch (args->ar_data) {
		case GFS2_DATA_WRITEBACK:
			state = "writeback";
			break;
		case GFS2_DATA_ORDERED:
			state = "ordered";
			break;
		default:
			state = "unknown";
			break;
		}
		seq_printf(s, ",data=%s", state);
	}

	RETURN(G2FN_SHOW_OPTIONS, 0);
}

struct super_operations gfs2_super_ops = {
	.write_inode = gfs2_write_inode,
	.put_inode = gfs2_put_inode,
	.put_super = gfs2_put_super,
	.write_super = gfs2_write_super,
	.write_super_lockfs = gfs2_write_super_lockfs,
	.unlockfs = gfs2_unlockfs,
	.statfs = gfs2_statfs,
	.remount_fs = gfs2_remount_fs,
	.clear_inode = gfs2_clear_inode,
	.show_options = gfs2_show_options,
};
