/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "xfs.h"
#include "xfs_bit.h"
#include "xfs_log.h"
#include "xfs_clnt.h"
#include "xfs_inum.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_dir2.h"
#include "xfs_alloc.h"
#include "xfs_dmapi.h"
#include "xfs_quota.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_dir2_sf.h"
#include "xfs_attr_sf.h"
#include "xfs_dinode.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_ialloc.h"
#include "xfs_bmap.h"
#include "xfs_rtalloc.h"
#include "xfs_error.h"
#include "xfs_itable.h"
#include "xfs_fsops.h"
#include "xfs_rw.h"
#include "xfs_acl.h"
#include "xfs_attr.h"
#include "xfs_buf_item.h"
#include "xfs_utils.h"
#include "xfs_vnodeops.h"
#include "xfs_vfsops.h"
#include "xfs_version.h"
#include "xfs_log_priv.h"
#include "xfs_trans_priv.h"
#include "xfs_filestream.h"
#include "xfs_da_btree.h"
#include "xfs_dir2_trace.h"
#include "xfs_extfree_item.h"
#include "xfs_mru_cache.h"
#include "xfs_inode_item.h"

#include <linux/namei.h>
#include <linux/init.h>
#include <linux/mount.h>
#include <linux/mempool.h>
#include <linux/writeback.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/parser.h>

static struct quotactl_ops xfs_quotactl_operations;
static struct super_operations xfs_super_operations;
static kmem_zone_t *xfs_vnode_zone;
static kmem_zone_t *xfs_ioend_zone;
mempool_t *xfs_ioend_pool;

STATIC struct xfs_mount_args *
xfs_args_allocate(
	struct super_block	*sb,
	int			silent)
{
	struct xfs_mount_args	*args;

	args = kzalloc(sizeof(struct xfs_mount_args), GFP_KERNEL);
	if (!args)
		return NULL;

	args->logbufs = args->logbufsize = -1;
	strncpy(args->fsname, sb->s_id, MAXNAMELEN);

	/* Copy the already-parsed mount(2) flags we're interested in */
	if (sb->s_flags & MS_DIRSYNC)
		args->flags |= XFSMNT_DIRSYNC;
	if (sb->s_flags & MS_SYNCHRONOUS)
		args->flags |= XFSMNT_WSYNC;
	if (silent)
		args->flags |= XFSMNT_QUIET;
	args->flags |= XFSMNT_32BITINODES;

	return args;
}

#define MNTOPT_LOGBUFS	"logbufs"	/* number of XFS log buffers */
#define MNTOPT_LOGBSIZE	"logbsize"	/* size of XFS log buffers */
#define MNTOPT_LOGDEV	"logdev"	/* log device */
#define MNTOPT_RTDEV	"rtdev"		/* realtime I/O device */
#define MNTOPT_BIOSIZE	"biosize"	/* log2 of preferred buffered io size */
#define MNTOPT_WSYNC	"wsync"		/* safe-mode nfs compatible mount */
#define MNTOPT_INO64	"ino64"		/* force inodes into 64-bit range */
#define MNTOPT_NOALIGN	"noalign"	/* turn off stripe alignment */
#define MNTOPT_SWALLOC	"swalloc"	/* turn on stripe width allocation */
#define MNTOPT_SUNIT	"sunit"		/* data volume stripe unit */
#define MNTOPT_SWIDTH	"swidth"	/* data volume stripe width */
#define MNTOPT_NOUUID	"nouuid"	/* ignore filesystem UUID */
#define MNTOPT_MTPT	"mtpt"		/* filesystem mount point */
#define MNTOPT_GRPID	"grpid"		/* group-ID from parent directory */
#define MNTOPT_NOGRPID	"nogrpid"	/* group-ID from current process */
#define MNTOPT_BSDGROUPS    "bsdgroups"    /* group-ID from parent directory */
#define MNTOPT_SYSVGROUPS   "sysvgroups"   /* group-ID from current process */
#define MNTOPT_ALLOCSIZE    "allocsize"    /* preferred allocation size */
#define MNTOPT_NORECOVERY   "norecovery"   /* don't run XFS recovery */
#define MNTOPT_BARRIER	"barrier"	/* use writer barriers for log write and
					 * unwritten extent conversion */
#define MNTOPT_NOBARRIER "nobarrier"	/* .. disable */
#define MNTOPT_OSYNCISOSYNC "osyncisosync" /* o_sync is REALLY o_sync */
#define MNTOPT_64BITINODE   "inode64"	/* inodes can be allocated anywhere */
#define MNTOPT_IKEEP	"ikeep"		/* do not free empty inode clusters */
#define MNTOPT_NOIKEEP	"noikeep"	/* free empty inode clusters */
#define MNTOPT_LARGEIO	   "largeio"	/* report large I/O sizes in stat() */
#define MNTOPT_NOLARGEIO   "nolargeio"	/* do not report large I/O sizes
					 * in stat(). */
#define MNTOPT_ATTR2	"attr2"		/* do use attr2 attribute format */
#define MNTOPT_NOATTR2	"noattr2"	/* do not use attr2 attribute format */
#define MNTOPT_FILESTREAM  "filestreams" /* use filestreams allocator */
#define MNTOPT_QUOTA	"quota"		/* disk quotas (user) */
#define MNTOPT_NOQUOTA	"noquota"	/* no quotas */
#define MNTOPT_USRQUOTA	"usrquota"	/* user quota enabled */
#define MNTOPT_GRPQUOTA	"grpquota"	/* group quota enabled */
#define MNTOPT_PRJQUOTA	"prjquota"	/* project quota enabled */
#define MNTOPT_UQUOTA	"uquota"	/* user quota (IRIX variant) */
#define MNTOPT_GQUOTA	"gquota"	/* group quota (IRIX variant) */
#define MNTOPT_PQUOTA	"pquota"	/* project quota (IRIX variant) */
#define MNTOPT_UQUOTANOENF "uqnoenforce"/* user quota limit enforcement */
#define MNTOPT_GQUOTANOENF "gqnoenforce"/* group quota limit enforcement */
#define MNTOPT_PQUOTANOENF "pqnoenforce"/* project quota limit enforcement */
#define MNTOPT_QUOTANOENF  "qnoenforce"	/* same as uqnoenforce */
#define MNTOPT_DMAPI	"dmapi"		/* DMI enabled (DMAPI / XDSM) */
#define MNTOPT_XDSM	"xdsm"		/* DMI enabled (DMAPI / XDSM) */
#define MNTOPT_DMI	"dmi"		/* DMI enabled (DMAPI / XDSM) */

/*
 * Table driven mount option parser.
 *
 * Currently only used for remount, but it will be used for mount
 * in the future, too.
 */
enum {
	Opt_barrier, Opt_nobarrier, Opt_err
};

static match_table_t tokens = {
	{Opt_barrier, "barrier"},
	{Opt_nobarrier, "nobarrier"},
	{Opt_err, NULL}
};


STATIC unsigned long
suffix_strtoul(char *s, char **endp, unsigned int base)
{
	int	last, shift_left_factor = 0;
	char	*value = s;

	last = strlen(value) - 1;
	if (value[last] == 'K' || value[last] == 'k') {
		shift_left_factor = 10;
		value[last] = '\0';
	}
	if (value[last] == 'M' || value[last] == 'm') {
		shift_left_factor = 20;
		value[last] = '\0';
	}
	if (value[last] == 'G' || value[last] == 'g') {
		shift_left_factor = 30;
		value[last] = '\0';
	}

	return simple_strtoul((const char *)s, endp, base) << shift_left_factor;
}

STATIC int
xfs_parseargs(
	struct xfs_mount	*mp,
	char			*options,
	struct xfs_mount_args	*args,
	int			update)
{
	char			*this_char, *value, *eov;
	int			dsunit, dswidth, vol_dsunit, vol_dswidth;
	int			iosize;
	int			dmapi_implies_ikeep = 1;

	args->flags |= XFSMNT_BARRIER;
	args->flags2 |= XFSMNT2_COMPAT_IOSIZE;

	if (!options)
		goto done;

	iosize = dsunit = dswidth = vol_dsunit = vol_dswidth = 0;

	while ((this_char = strsep(&options, ",")) != NULL) {
		if (!*this_char)
			continue;
		if ((value = strchr(this_char, '=')) != NULL)
			*value++ = 0;

		if (!strcmp(this_char, MNTOPT_LOGBUFS)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			args->logbufs = simple_strtoul(value, &eov, 10);
		} else if (!strcmp(this_char, MNTOPT_LOGBSIZE)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			args->logbufsize = suffix_strtoul(value, &eov, 10);
		} else if (!strcmp(this_char, MNTOPT_LOGDEV)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			strncpy(args->logname, value, MAXNAMELEN);
		} else if (!strcmp(this_char, MNTOPT_MTPT)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			strncpy(args->mtpt, value, MAXNAMELEN);
		} else if (!strcmp(this_char, MNTOPT_RTDEV)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			strncpy(args->rtname, value, MAXNAMELEN);
		} else if (!strcmp(this_char, MNTOPT_BIOSIZE)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			iosize = simple_strtoul(value, &eov, 10);
			args->flags |= XFSMNT_IOSIZE;
			args->iosizelog = (uint8_t) iosize;
		} else if (!strcmp(this_char, MNTOPT_ALLOCSIZE)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			iosize = suffix_strtoul(value, &eov, 10);
			args->flags |= XFSMNT_IOSIZE;
			args->iosizelog = ffs(iosize) - 1;
		} else if (!strcmp(this_char, MNTOPT_GRPID) ||
			   !strcmp(this_char, MNTOPT_BSDGROUPS)) {
			mp->m_flags |= XFS_MOUNT_GRPID;
		} else if (!strcmp(this_char, MNTOPT_NOGRPID) ||
			   !strcmp(this_char, MNTOPT_SYSVGROUPS)) {
			mp->m_flags &= ~XFS_MOUNT_GRPID;
		} else if (!strcmp(this_char, MNTOPT_WSYNC)) {
			args->flags |= XFSMNT_WSYNC;
		} else if (!strcmp(this_char, MNTOPT_OSYNCISOSYNC)) {
			args->flags |= XFSMNT_OSYNCISOSYNC;
		} else if (!strcmp(this_char, MNTOPT_NORECOVERY)) {
			args->flags |= XFSMNT_NORECOVERY;
		} else if (!strcmp(this_char, MNTOPT_INO64)) {
			args->flags |= XFSMNT_INO64;
#if !XFS_BIG_INUMS
			cmn_err(CE_WARN,
				"XFS: %s option not allowed on this system",
				this_char);
			return EINVAL;
#endif
		} else if (!strcmp(this_char, MNTOPT_NOALIGN)) {
			args->flags |= XFSMNT_NOALIGN;
		} else if (!strcmp(this_char, MNTOPT_SWALLOC)) {
			args->flags |= XFSMNT_SWALLOC;
		} else if (!strcmp(this_char, MNTOPT_SUNIT)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			dsunit = simple_strtoul(value, &eov, 10);
		} else if (!strcmp(this_char, MNTOPT_SWIDTH)) {
			if (!value || !*value) {
				cmn_err(CE_WARN,
					"XFS: %s option requires an argument",
					this_char);
				return EINVAL;
			}
			dswidth = simple_strtoul(value, &eov, 10);
		} else if (!strcmp(this_char, MNTOPT_64BITINODE)) {
			args->flags &= ~XFSMNT_32BITINODES;
#if !XFS_BIG_INUMS
			cmn_err(CE_WARN,
				"XFS: %s option not allowed on this system",
				this_char);
			return EINVAL;
#endif
		} else if (!strcmp(this_char, MNTOPT_NOUUID)) {
			args->flags |= XFSMNT_NOUUID;
		} else if (!strcmp(this_char, MNTOPT_BARRIER)) {
			args->flags |= XFSMNT_BARRIER;
		} else if (!strcmp(this_char, MNTOPT_NOBARRIER)) {
			args->flags &= ~XFSMNT_BARRIER;
		} else if (!strcmp(this_char, MNTOPT_IKEEP)) {
			args->flags |= XFSMNT_IKEEP;
		} else if (!strcmp(this_char, MNTOPT_NOIKEEP)) {
			dmapi_implies_ikeep = 0;
			args->flags &= ~XFSMNT_IKEEP;
		} else if (!strcmp(this_char, MNTOPT_LARGEIO)) {
			args->flags2 &= ~XFSMNT2_COMPAT_IOSIZE;
		} else if (!strcmp(this_char, MNTOPT_NOLARGEIO)) {
			args->flags2 |= XFSMNT2_COMPAT_IOSIZE;
		} else if (!strcmp(this_char, MNTOPT_ATTR2)) {
			args->flags |= XFSMNT_ATTR2;
		} else if (!strcmp(this_char, MNTOPT_NOATTR2)) {
			args->flags &= ~XFSMNT_ATTR2;
			args->flags |= XFSMNT_NOATTR2;
		} else if (!strcmp(this_char, MNTOPT_FILESTREAM)) {
			args->flags2 |= XFSMNT2_FILESTREAMS;
		} else if (!strcmp(this_char, MNTOPT_NOQUOTA)) {
			args->flags &= ~(XFSMNT_UQUOTAENF|XFSMNT_UQUOTA);
			args->flags &= ~(XFSMNT_GQUOTAENF|XFSMNT_GQUOTA);
		} else if (!strcmp(this_char, MNTOPT_QUOTA) ||
			   !strcmp(this_char, MNTOPT_UQUOTA) ||
			   !strcmp(this_char, MNTOPT_USRQUOTA)) {
			args->flags |= XFSMNT_UQUOTA | XFSMNT_UQUOTAENF;
		} else if (!strcmp(this_char, MNTOPT_QUOTANOENF) ||
			   !strcmp(this_char, MNTOPT_UQUOTANOENF)) {
			args->flags |= XFSMNT_UQUOTA;
			args->flags &= ~XFSMNT_UQUOTAENF;
		} else if (!strcmp(this_char, MNTOPT_PQUOTA) ||
			   !strcmp(this_char, MNTOPT_PRJQUOTA)) {
			args->flags |= XFSMNT_PQUOTA | XFSMNT_PQUOTAENF;
		} else if (!strcmp(this_char, MNTOPT_PQUOTANOENF)) {
			args->flags |= XFSMNT_PQUOTA;
			args->flags &= ~XFSMNT_PQUOTAENF;
		} else if (!strcmp(this_char, MNTOPT_GQUOTA) ||
			   !strcmp(this_char, MNTOPT_GRPQUOTA)) {
			args->flags |= XFSMNT_GQUOTA | XFSMNT_GQUOTAENF;
		} else if (!strcmp(this_char, MNTOPT_GQUOTANOENF)) {
			args->flags |= XFSMNT_GQUOTA;
			args->flags &= ~XFSMNT_GQUOTAENF;
		} else if (!strcmp(this_char, MNTOPT_DMAPI)) {
			args->flags |= XFSMNT_DMAPI;
		} else if (!strcmp(this_char, MNTOPT_XDSM)) {
			args->flags |= XFSMNT_DMAPI;
		} else if (!strcmp(this_char, MNTOPT_DMI)) {
			args->flags |= XFSMNT_DMAPI;
		} else if (!strcmp(this_char, "ihashsize")) {
			cmn_err(CE_WARN,
	"XFS: ihashsize no longer used, option is deprecated.");
		} else if (!strcmp(this_char, "osyncisdsync")) {
			/* no-op, this is now the default */
			cmn_err(CE_WARN,
	"XFS: osyncisdsync is now the default, option is deprecated.");
		} else if (!strcmp(this_char, "irixsgid")) {
			cmn_err(CE_WARN,
	"XFS: irixsgid is now a sysctl(2) variable, option is deprecated.");
		} else {
			cmn_err(CE_WARN,
				"XFS: unknown mount option [%s].", this_char);
			return EINVAL;
		}
	}

	if (args->flags & XFSMNT_NORECOVERY) {
		if ((mp->m_flags & XFS_MOUNT_RDONLY) == 0) {
			cmn_err(CE_WARN,
				"XFS: no-recovery mounts must be read-only.");
			return EINVAL;
		}
	}

	if ((args->flags & XFSMNT_NOALIGN) && (dsunit || dswidth)) {
		cmn_err(CE_WARN,
	"XFS: sunit and swidth options incompatible with the noalign option");
		return EINVAL;
	}

	if ((args->flags & XFSMNT_GQUOTA) && (args->flags & XFSMNT_PQUOTA)) {
		cmn_err(CE_WARN,
			"XFS: cannot mount with both project and group quota");
		return EINVAL;
	}

	if ((args->flags & XFSMNT_DMAPI) && *args->mtpt == '\0') {
		printk("XFS: %s option needs the mount point option as well\n",
			MNTOPT_DMAPI);
		return EINVAL;
	}

	if ((dsunit && !dswidth) || (!dsunit && dswidth)) {
		cmn_err(CE_WARN,
			"XFS: sunit and swidth must be specified together");
		return EINVAL;
	}

	if (dsunit && (dswidth % dsunit != 0)) {
		cmn_err(CE_WARN,
	"XFS: stripe width (%d) must be a multiple of the stripe unit (%d)",
			dswidth, dsunit);
		return EINVAL;
	}

	/*
	 * Applications using DMI filesystems often expect the
	 * inode generation number to be monotonically increasing.
	 * If we delete inode chunks we break this assumption, so
	 * keep unused inode chunks on disk for DMI filesystems
	 * until we come up with a better solution.
	 * Note that if "ikeep" or "noikeep" mount options are
	 * supplied, then they are honored.
	 */
	if ((args->flags & XFSMNT_DMAPI) && dmapi_implies_ikeep)
		args->flags |= XFSMNT_IKEEP;

	if ((args->flags & XFSMNT_NOALIGN) != XFSMNT_NOALIGN) {
		if (dsunit) {
			args->sunit = dsunit;
			args->flags |= XFSMNT_RETERR;
		} else {
			args->sunit = vol_dsunit;
		}
		dswidth ? (args->swidth = dswidth) :
			  (args->swidth = vol_dswidth);
	} else {
		args->sunit = args->swidth = 0;
	}

done:
	if (args->flags & XFSMNT_32BITINODES)
		mp->m_flags |= XFS_MOUNT_SMALL_INUMS;
	if (args->flags2)
		args->flags |= XFSMNT_FLAGS2;
	return 0;
}

struct proc_xfs_info {
	int	flag;
	char	*str;
};

STATIC int
xfs_showargs(
	struct xfs_mount	*mp,
	struct seq_file		*m)
{
	static struct proc_xfs_info xfs_info_set[] = {
		/* the few simple ones we can get from the mount struct */
		{ XFS_MOUNT_IKEEP,		"," MNTOPT_IKEEP },
		{ XFS_MOUNT_WSYNC,		"," MNTOPT_WSYNC },
		{ XFS_MOUNT_INO64,		"," MNTOPT_INO64 },
		{ XFS_MOUNT_NOALIGN,		"," MNTOPT_NOALIGN },
		{ XFS_MOUNT_SWALLOC,		"," MNTOPT_SWALLOC },
		{ XFS_MOUNT_NOUUID,		"," MNTOPT_NOUUID },
		{ XFS_MOUNT_NORECOVERY,		"," MNTOPT_NORECOVERY },
		{ XFS_MOUNT_OSYNCISOSYNC,	"," MNTOPT_OSYNCISOSYNC },
		{ XFS_MOUNT_ATTR2,		"," MNTOPT_ATTR2 },
		{ XFS_MOUNT_FILESTREAMS,	"," MNTOPT_FILESTREAM },
		{ XFS_MOUNT_DMAPI,		"," MNTOPT_DMAPI },
		{ XFS_MOUNT_GRPID,		"," MNTOPT_GRPID },
		{ 0, NULL }
	};
	static struct proc_xfs_info xfs_info_unset[] = {
		/* the few simple ones we can get from the mount struct */
		{ XFS_MOUNT_COMPAT_IOSIZE,	"," MNTOPT_LARGEIO },
		{ XFS_MOUNT_BARRIER,		"," MNTOPT_NOBARRIER },
		{ XFS_MOUNT_SMALL_INUMS,	"," MNTOPT_64BITINODE },
		{ 0, NULL }
	};
	struct proc_xfs_info	*xfs_infop;

	for (xfs_infop = xfs_info_set; xfs_infop->flag; xfs_infop++) {
		if (mp->m_flags & xfs_infop->flag)
			seq_puts(m, xfs_infop->str);
	}
	for (xfs_infop = xfs_info_unset; xfs_infop->flag; xfs_infop++) {
		if (!(mp->m_flags & xfs_infop->flag))
			seq_puts(m, xfs_infop->str);
	}

	if (mp->m_flags & XFS_MOUNT_DFLT_IOSIZE)
		seq_printf(m, "," MNTOPT_ALLOCSIZE "=%dk",
				(int)(1 << mp->m_writeio_log) >> 10);

	if (mp->m_logbufs > 0)
		seq_printf(m, "," MNTOPT_LOGBUFS "=%d", mp->m_logbufs);
	if (mp->m_logbsize > 0)
		seq_printf(m, "," MNTOPT_LOGBSIZE "=%dk", mp->m_logbsize >> 10);

	if (mp->m_logname)
		seq_printf(m, "," MNTOPT_LOGDEV "=%s", mp->m_logname);
	if (mp->m_rtname)
		seq_printf(m, "," MNTOPT_RTDEV "=%s", mp->m_rtname);

	if (mp->m_dalign > 0)
		seq_printf(m, "," MNTOPT_SUNIT "=%d",
				(int)XFS_FSB_TO_BB(mp, mp->m_dalign));
	if (mp->m_swidth > 0)
		seq_printf(m, "," MNTOPT_SWIDTH "=%d",
				(int)XFS_FSB_TO_BB(mp, mp->m_swidth));

	if (mp->m_qflags & (XFS_UQUOTA_ACCT|XFS_UQUOTA_ENFD))
		seq_puts(m, "," MNTOPT_USRQUOTA);
	else if (mp->m_qflags & XFS_UQUOTA_ACCT)
		seq_puts(m, "," MNTOPT_UQUOTANOENF);

	if (mp->m_qflags & (XFS_PQUOTA_ACCT|XFS_OQUOTA_ENFD))
		seq_puts(m, "," MNTOPT_PRJQUOTA);
	else if (mp->m_qflags & XFS_PQUOTA_ACCT)
		seq_puts(m, "," MNTOPT_PQUOTANOENF);

	if (mp->m_qflags & (XFS_GQUOTA_ACCT|XFS_OQUOTA_ENFD))
		seq_puts(m, "," MNTOPT_GRPQUOTA);
	else if (mp->m_qflags & XFS_GQUOTA_ACCT)
		seq_puts(m, "," MNTOPT_GQUOTANOENF);

	if (!(mp->m_qflags & XFS_ALL_QUOTA_ACCT))
		seq_puts(m, "," MNTOPT_NOQUOTA);

	return 0;
}
__uint64_t
xfs_max_file_offset(
	unsigned int		blockshift)
{
	unsigned int		pagefactor = 1;
	unsigned int		bitshift = BITS_PER_LONG - 1;

	/* Figure out maximum filesize, on Linux this can depend on
	 * the filesystem blocksize (on 32 bit platforms).
	 * __block_prepare_write does this in an [unsigned] long...
	 *      page->index << (PAGE_CACHE_SHIFT - bbits)
	 * So, for page sized blocks (4K on 32 bit platforms),
	 * this wraps at around 8Tb (hence MAX_LFS_FILESIZE which is
	 *      (((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG-1))-1)
	 * but for smaller blocksizes it is less (bbits = log2 bsize).
	 * Note1: get_block_t takes a long (implicit cast from above)
	 * Note2: The Large Block Device (LBD and HAVE_SECTOR_T) patch
	 * can optionally convert the [unsigned] long from above into
	 * an [unsigned] long long.
	 */

#if BITS_PER_LONG == 32
# if defined(CONFIG_LBD)
	ASSERT(sizeof(sector_t) == 8);
	pagefactor = PAGE_CACHE_SIZE;
	bitshift = BITS_PER_LONG;
# else
	pagefactor = PAGE_CACHE_SIZE >> (PAGE_CACHE_SHIFT - blockshift);
# endif
#endif

	return (((__uint64_t)pagefactor) << bitshift) - 1;
}

int
xfs_blkdev_get(
	xfs_mount_t		*mp,
	const char		*name,
	struct block_device	**bdevp)
{
	int			error = 0;

	*bdevp = open_bdev_excl(name, 0, mp);
	if (IS_ERR(*bdevp)) {
		error = PTR_ERR(*bdevp);
		printk("XFS: Invalid device [%s], error=%d\n", name, error);
	}

	return -error;
}

void
xfs_blkdev_put(
	struct block_device	*bdev)
{
	if (bdev)
		close_bdev_excl(bdev);
}

/*
 * Try to write out the superblock using barriers.
 */
STATIC int
xfs_barrier_test(
	xfs_mount_t	*mp)
{
	xfs_buf_t	*sbp = xfs_getsb(mp, 0);
	int		error;

	XFS_BUF_UNDONE(sbp);
	XFS_BUF_UNREAD(sbp);
	XFS_BUF_UNDELAYWRITE(sbp);
	XFS_BUF_WRITE(sbp);
	XFS_BUF_UNASYNC(sbp);
	XFS_BUF_ORDERED(sbp);

	xfsbdstrat(mp, sbp);
	error = xfs_iowait(sbp);

	/*
	 * Clear all the flags we set and possible error state in the
	 * buffer.  We only did the write to try out whether barriers
	 * worked and shouldn't leave any traces in the superblock
	 * buffer.
	 */
	XFS_BUF_DONE(sbp);
	XFS_BUF_ERROR(sbp, 0);
	XFS_BUF_UNORDERED(sbp);

	xfs_buf_relse(sbp);
	return error;
}

void
xfs_mountfs_check_barriers(xfs_mount_t *mp)
{
	int error;

	if (mp->m_logdev_targp != mp->m_ddev_targp) {
		xfs_fs_cmn_err(CE_NOTE, mp,
		  "Disabling barriers, not supported with external log device");
		mp->m_flags &= ~XFS_MOUNT_BARRIER;
		return;
	}

	if (xfs_readonly_buftarg(mp->m_ddev_targp)) {
		xfs_fs_cmn_err(CE_NOTE, mp,
		  "Disabling barriers, underlying device is readonly");
		mp->m_flags &= ~XFS_MOUNT_BARRIER;
		return;
	}

	error = xfs_barrier_test(mp);
	if (error) {
		xfs_fs_cmn_err(CE_NOTE, mp,
		  "Disabling barriers, trial barrier write failed");
		mp->m_flags &= ~XFS_MOUNT_BARRIER;
		return;
	}
}

void
xfs_blkdev_issue_flush(
	xfs_buftarg_t		*buftarg)
{
	blkdev_issue_flush(buftarg->bt_bdev, NULL);
}

STATIC void
xfs_close_devices(
	struct xfs_mount	*mp)
{
	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp) {
		struct block_device *logdev = mp->m_logdev_targp->bt_bdev;
		xfs_free_buftarg(mp->m_logdev_targp);
		xfs_blkdev_put(logdev);
	}
	if (mp->m_rtdev_targp) {
		struct block_device *rtdev = mp->m_rtdev_targp->bt_bdev;
		xfs_free_buftarg(mp->m_rtdev_targp);
		xfs_blkdev_put(rtdev);
	}
	xfs_free_buftarg(mp->m_ddev_targp);
}

/*
 * The file system configurations are:
 *	(1) device (partition) with data and internal log
 *	(2) logical volume with data and log subvolumes.
 *	(3) logical volume with data, log, and realtime subvolumes.
 *
 * We only have to handle opening the log and realtime volumes here if
 * they are present.  The data subvolume has already been opened by
 * get_sb_bdev() and is stored in sb->s_bdev.
 */
STATIC int
xfs_open_devices(
	struct xfs_mount	*mp,
	struct xfs_mount_args	*args)
{
	struct block_device	*ddev = mp->m_super->s_bdev;
	struct block_device	*logdev = NULL, *rtdev = NULL;
	int			error;

	/*
	 * Open real time and log devices - order is important.
	 */
	if (args->logname[0]) {
		error = xfs_blkdev_get(mp, args->logname, &logdev);
		if (error)
			goto out;
	}

	if (args->rtname[0]) {
		error = xfs_blkdev_get(mp, args->rtname, &rtdev);
		if (error)
			goto out_close_logdev;

		if (rtdev == ddev || rtdev == logdev) {
			cmn_err(CE_WARN,
	"XFS: Cannot mount filesystem with identical rtdev and ddev/logdev.");
			error = EINVAL;
			goto out_close_rtdev;
		}
	}

	/*
	 * Setup xfs_mount buffer target pointers
	 */
	error = ENOMEM;
	mp->m_ddev_targp = xfs_alloc_buftarg(ddev, 0);
	if (!mp->m_ddev_targp)
		goto out_close_rtdev;

	if (rtdev) {
		mp->m_rtdev_targp = xfs_alloc_buftarg(rtdev, 1);
		if (!mp->m_rtdev_targp)
			goto out_free_ddev_targ;
	}

	if (logdev && logdev != ddev) {
		mp->m_logdev_targp = xfs_alloc_buftarg(logdev, 1);
		if (!mp->m_logdev_targp)
			goto out_free_rtdev_targ;
	} else {
		mp->m_logdev_targp = mp->m_ddev_targp;
	}

	return 0;

 out_free_rtdev_targ:
	if (mp->m_rtdev_targp)
		xfs_free_buftarg(mp->m_rtdev_targp);
 out_free_ddev_targ:
	xfs_free_buftarg(mp->m_ddev_targp);
 out_close_rtdev:
	if (rtdev)
		xfs_blkdev_put(rtdev);
 out_close_logdev:
	if (logdev && logdev != ddev)
		xfs_blkdev_put(logdev);
 out:
	return error;
}

/*
 * Setup xfs_mount buffer target pointers based on superblock
 */
STATIC int
xfs_setup_devices(
	struct xfs_mount	*mp)
{
	int			error;

	error = xfs_setsize_buftarg(mp->m_ddev_targp, mp->m_sb.sb_blocksize,
				    mp->m_sb.sb_sectsize);
	if (error)
		return error;

	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp) {
		unsigned int	log_sector_size = BBSIZE;

		if (xfs_sb_version_hassector(&mp->m_sb))
			log_sector_size = mp->m_sb.sb_logsectsize;
		error = xfs_setsize_buftarg(mp->m_logdev_targp,
					    mp->m_sb.sb_blocksize,
					    log_sector_size);
		if (error)
			return error;
	}
	if (mp->m_rtdev_targp) {
		error = xfs_setsize_buftarg(mp->m_rtdev_targp,
					    mp->m_sb.sb_blocksize,
					    mp->m_sb.sb_sectsize);
		if (error)
			return error;
	}

	return 0;
}

/*
 * XFS AIL push thread support
 */
void
xfsaild_wakeup(
	xfs_mount_t		*mp,
	xfs_lsn_t		threshold_lsn)
{
	mp->m_ail.xa_target = threshold_lsn;
	wake_up_process(mp->m_ail.xa_task);
}

int
xfsaild(
	void	*data)
{
	xfs_mount_t	*mp = (xfs_mount_t *)data;
	xfs_lsn_t	last_pushed_lsn = 0;
	long		tout = 0;

	while (!kthread_should_stop()) {
		if (tout)
			schedule_timeout_interruptible(msecs_to_jiffies(tout));
		tout = 1000;

		/* swsusp */
		try_to_freeze();

		ASSERT(mp->m_log);
		if (XFS_FORCED_SHUTDOWN(mp))
			continue;

		tout = xfsaild_push(mp, &last_pushed_lsn);
	}

	return 0;
}	/* xfsaild */

int
xfsaild_start(
	xfs_mount_t	*mp)
{
	mp->m_ail.xa_target = 0;
	mp->m_ail.xa_task = kthread_run(xfsaild, mp, "xfsaild");
	if (IS_ERR(mp->m_ail.xa_task))
		return -PTR_ERR(mp->m_ail.xa_task);
	return 0;
}

void
xfsaild_stop(
	xfs_mount_t	*mp)
{
	kthread_stop(mp->m_ail.xa_task);
}



STATIC struct inode *
xfs_fs_alloc_inode(
	struct super_block	*sb)
{
	return kmem_zone_alloc(xfs_vnode_zone, KM_SLEEP);
}

STATIC void
xfs_fs_destroy_inode(
	struct inode		*inode)
{
	kmem_zone_free(xfs_vnode_zone, inode);
}

STATIC void
xfs_fs_inode_init_once(
	void			*vnode)
{
	inode_init_once((struct inode *)vnode);
}

/*
 * Attempt to flush the inode, this will actually fail
 * if the inode is pinned, but we dirty the inode again
 * at the point when it is unpinned after a log write,
 * since this is when the inode itself becomes flushable.
 */
STATIC int
xfs_fs_write_inode(
	struct inode		*inode,
	int			sync)
{
	int			error = 0;
	int			flags = 0;

	xfs_itrace_entry(XFS_I(inode));
	if (sync) {
		filemap_fdatawait(inode->i_mapping);
		flags |= FLUSH_SYNC;
	}
	error = xfs_inode_flush(XFS_I(inode), flags);
	/*
	 * if we failed to write out the inode then mark
	 * it dirty again so we'll try again later.
	 */
	if (error)
		mark_inode_dirty_sync(inode);

	return -error;
}

STATIC void
xfs_fs_clear_inode(
	struct inode		*inode)
{
	xfs_inode_t		*ip = XFS_I(inode);

	/*
	 * ip can be null when xfs_iget_core calls xfs_idestroy if we
	 * find an inode with di_mode == 0 but without IGET_CREATE set.
	 */
	if (ip) {
		xfs_itrace_entry(ip);
		XFS_STATS_INC(vn_rele);
		XFS_STATS_INC(vn_remove);
		XFS_STATS_INC(vn_reclaim);
		XFS_STATS_DEC(vn_active);

		xfs_inactive(ip);
		xfs_iflags_clear(ip, XFS_IMODIFIED);
		if (xfs_reclaim(ip))
			panic("%s: cannot reclaim 0x%p\n", __func__, inode);
	}

	ASSERT(XFS_I(inode) == NULL);
}

/*
 * Enqueue a work item to be picked up by the vfs xfssyncd thread.
 * Doing this has two advantages:
 * - It saves on stack space, which is tight in certain situations
 * - It can be used (with care) as a mechanism to avoid deadlocks.
 * Flushing while allocating in a full filesystem requires both.
 */
STATIC void
xfs_syncd_queue_work(
	struct xfs_mount *mp,
	void		*data,
	void		(*syncer)(struct xfs_mount *, void *))
{
	struct bhv_vfs_sync_work *work;

	work = kmem_alloc(sizeof(struct bhv_vfs_sync_work), KM_SLEEP);
	INIT_LIST_HEAD(&work->w_list);
	work->w_syncer = syncer;
	work->w_data = data;
	work->w_mount = mp;
	spin_lock(&mp->m_sync_lock);
	list_add_tail(&work->w_list, &mp->m_sync_list);
	spin_unlock(&mp->m_sync_lock);
	wake_up_process(mp->m_sync_task);
}

/*
 * Flush delayed allocate data, attempting to free up reserved space
 * from existing allocations.  At this point a new allocation attempt
 * has failed with ENOSPC and we are in the process of scratching our
 * heads, looking about for more room...
 */
STATIC void
xfs_flush_inode_work(
	struct xfs_mount *mp,
	void		*arg)
{
	struct inode	*inode = arg;
	filemap_flush(inode->i_mapping);
	iput(inode);
}

void
xfs_flush_inode(
	xfs_inode_t	*ip)
{
	struct inode	*inode = VFS_I(ip);

	igrab(inode);
	xfs_syncd_queue_work(ip->i_mount, inode, xfs_flush_inode_work);
	delay(msecs_to_jiffies(500));
}

/*
 * This is the "bigger hammer" version of xfs_flush_inode_work...
 * (IOW, "If at first you don't succeed, use a Bigger Hammer").
 */
STATIC void
xfs_flush_device_work(
	struct xfs_mount *mp,
	void		*arg)
{
	struct inode	*inode = arg;
	sync_blockdev(mp->m_super->s_bdev);
	iput(inode);
}

void
xfs_flush_device(
	xfs_inode_t	*ip)
{
	struct inode	*inode = VFS_I(ip);

	igrab(inode);
	xfs_syncd_queue_work(ip->i_mount, inode, xfs_flush_device_work);
	delay(msecs_to_jiffies(500));
	xfs_log_force(ip->i_mount, (xfs_lsn_t)0, XFS_LOG_FORCE|XFS_LOG_SYNC);
}

STATIC void
xfs_sync_worker(
	struct xfs_mount *mp,
	void		*unused)
{
	int		error;

	if (!(mp->m_flags & XFS_MOUNT_RDONLY))
		error = xfs_sync(mp, SYNC_FSDATA | SYNC_BDFLUSH | SYNC_ATTR);
	mp->m_sync_seq++;
	wake_up(&mp->m_wait_single_sync_task);
}

STATIC int
xfssyncd(
	void			*arg)
{
	struct xfs_mount	*mp = arg;
	long			timeleft;
	bhv_vfs_sync_work_t	*work, *n;
	LIST_HEAD		(tmp);

	set_freezable();
	timeleft = xfs_syncd_centisecs * msecs_to_jiffies(10);
	for (;;) {
		timeleft = schedule_timeout_interruptible(timeleft);
		/* swsusp */
		try_to_freeze();
		if (kthread_should_stop() && list_empty(&mp->m_sync_list))
			break;

		spin_lock(&mp->m_sync_lock);
		/*
		 * We can get woken by laptop mode, to do a sync -
		 * that's the (only!) case where the list would be
		 * empty with time remaining.
		 */
		if (!timeleft || list_empty(&mp->m_sync_list)) {
			if (!timeleft)
				timeleft = xfs_syncd_centisecs *
							msecs_to_jiffies(10);
			INIT_LIST_HEAD(&mp->m_sync_work.w_list);
			list_add_tail(&mp->m_sync_work.w_list,
					&mp->m_sync_list);
		}
		list_for_each_entry_safe(work, n, &mp->m_sync_list, w_list)
			list_move(&work->w_list, &tmp);
		spin_unlock(&mp->m_sync_lock);

		list_for_each_entry_safe(work, n, &tmp, w_list) {
			(*work->w_syncer)(mp, work->w_data);
			list_del(&work->w_list);
			if (work == &mp->m_sync_work)
				continue;
			kmem_free(work);
		}
	}

	return 0;
}

STATIC void
xfs_free_fsname(
	struct xfs_mount	*mp)
{
	kfree(mp->m_fsname);
	kfree(mp->m_rtname);
	kfree(mp->m_logname);
}

STATIC void
xfs_fs_put_super(
	struct super_block	*sb)
{
	struct xfs_mount	*mp = XFS_M(sb);
	struct xfs_inode	*rip = mp->m_rootip;
	int			unmount_event_flags = 0;
	int			error;

	kthread_stop(mp->m_sync_task);

	xfs_sync(mp, SYNC_ATTR | SYNC_DELWRI);

#ifdef HAVE_DMAPI
	if (mp->m_flags & XFS_MOUNT_DMAPI) {
		unmount_event_flags =
			(mp->m_dmevmask & (1 << DM_EVENT_UNMOUNT)) ?
				0 : DM_FLAGS_UNWANTED;
		/*
		 * Ignore error from dmapi here, first unmount is not allowed
		 * to fail anyway, and second we wouldn't want to fail a
		 * unmount because of dmapi.
		 */
		XFS_SEND_PREUNMOUNT(mp, rip, DM_RIGHT_NULL, rip, DM_RIGHT_NULL,
				NULL, NULL, 0, 0, unmount_event_flags);
	}
#endif

	/*
	 * Blow away any referenced inode in the filestreams cache.
	 * This can and will cause log traffic as inodes go inactive
	 * here.
	 */
	xfs_filestream_unmount(mp);

	XFS_bflush(mp->m_ddev_targp);
	error = xfs_unmount_flush(mp, 0);
	WARN_ON(error);

	/*
	 * If we're forcing a shutdown, typically because of a media error,
	 * we want to make sure we invalidate dirty pages that belong to
	 * referenced vnodes as well.
	 */
	if (XFS_FORCED_SHUTDOWN(mp)) {
		error = xfs_sync(mp, SYNC_WAIT | SYNC_CLOSE);
		ASSERT(error != EFSCORRUPTED);
	}

	if (mp->m_flags & XFS_MOUNT_DMAPI) {
		XFS_SEND_UNMOUNT(mp, rip, DM_RIGHT_NULL, 0, 0,
				unmount_event_flags);
	}

	xfs_unmountfs(mp);
	xfs_freesb(mp);
	xfs_icsb_destroy_counters(mp);
	xfs_close_devices(mp);
	xfs_qmops_put(mp);
	xfs_dmops_put(mp);
	xfs_free_fsname(mp);
	kfree(mp);
}

STATIC void
xfs_fs_write_super(
	struct super_block	*sb)
{
	if (!(sb->s_flags & MS_RDONLY))
		xfs_sync(XFS_M(sb), SYNC_FSDATA);
	sb->s_dirt = 0;
}

STATIC int
xfs_fs_sync_super(
	struct super_block	*sb,
	int			wait)
{
	struct xfs_mount	*mp = XFS_M(sb);
	int			error;
	int			flags;

	/*
	 * Treat a sync operation like a freeze.  This is to work
	 * around a race in sync_inodes() which works in two phases
	 * - an asynchronous flush, which can write out an inode
	 * without waiting for file size updates to complete, and a
	 * synchronous flush, which wont do anything because the
	 * async flush removed the inode's dirty flag.  Also
	 * sync_inodes() will not see any files that just have
	 * outstanding transactions to be flushed because we don't
	 * dirty the Linux inode until after the transaction I/O
	 * completes.
	 */
	if (wait || unlikely(sb->s_frozen == SB_FREEZE_WRITE)) {
		/*
		 * First stage of freeze - no more writers will make progress
		 * now we are here, so we flush delwri and delalloc buffers
		 * here, then wait for all I/O to complete.  Data is frozen at
		 * that point. Metadata is not frozen, transactions can still
		 * occur here so don't bother flushing the buftarg (i.e
		 * SYNC_QUIESCE) because it'll just get dirty again.
		 */
		flags = SYNC_DATA_QUIESCE;
	} else
		flags = SYNC_FSDATA;

	error = xfs_sync(mp, flags);
	sb->s_dirt = 0;

	if (unlikely(laptop_mode)) {
		int	prev_sync_seq = mp->m_sync_seq;

		/*
		 * The disk must be active because we're syncing.
		 * We schedule xfssyncd now (now that the disk is
		 * active) instead of later (when it might not be).
		 */
		wake_up_process(mp->m_sync_task);
		/*
		 * We have to wait for the sync iteration to complete.
		 * If we don't, the disk activity caused by the sync
		 * will come after the sync is completed, and that
		 * triggers another sync from laptop mode.
		 */
		wait_event(mp->m_wait_single_sync_task,
				mp->m_sync_seq != prev_sync_seq);
	}

	return -error;
}

STATIC int
xfs_fs_statfs(
	struct dentry		*dentry,
	struct kstatfs		*statp)
{
	struct xfs_mount	*mp = XFS_M(dentry->d_sb);
	xfs_sb_t		*sbp = &mp->m_sb;
	__uint64_t		fakeinos, id;
	xfs_extlen_t		lsize;

	statp->f_type = XFS_SB_MAGIC;
	statp->f_namelen = MAXNAMELEN - 1;

	id = huge_encode_dev(mp->m_ddev_targp->bt_dev);
	statp->f_fsid.val[0] = (u32)id;
	statp->f_fsid.val[1] = (u32)(id >> 32);

	xfs_icsb_sync_counters(mp, XFS_ICSB_LAZY_COUNT);

	spin_lock(&mp->m_sb_lock);
	statp->f_bsize = sbp->sb_blocksize;
	lsize = sbp->sb_logstart ? sbp->sb_logblocks : 0;
	statp->f_blocks = sbp->sb_dblocks - lsize;
	statp->f_bfree = statp->f_bavail =
				sbp->sb_fdblocks - XFS_ALLOC_SET_ASIDE(mp);
	fakeinos = statp->f_bfree << sbp->sb_inopblog;
#if XFS_BIG_INUMS
	fakeinos += mp->m_inoadd;
#endif
	statp->f_files =
	    MIN(sbp->sb_icount + fakeinos, (__uint64_t)XFS_MAXINUMBER);
	if (mp->m_maxicount)
#if XFS_BIG_INUMS
		if (!mp->m_inoadd)
#endif
			statp->f_files = min_t(typeof(statp->f_files),
						statp->f_files,
						mp->m_maxicount);
	statp->f_ffree = statp->f_files - (sbp->sb_icount - sbp->sb_ifree);
	spin_unlock(&mp->m_sb_lock);

	XFS_QM_DQSTATVFS(XFS_I(dentry->d_inode), statp);
	return 0;
}

STATIC int
xfs_fs_remount(
	struct super_block	*sb,
	int			*flags,
	char			*options)
{
	struct xfs_mount	*mp = XFS_M(sb);
	substring_t		args[MAX_OPT_ARGS];
	char			*p;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_barrier:
			mp->m_flags |= XFS_MOUNT_BARRIER;

			/*
			 * Test if barriers are actually working if we can,
			 * else delay this check until the filesystem is
			 * marked writeable.
			 */
			if (!(mp->m_flags & XFS_MOUNT_RDONLY))
				xfs_mountfs_check_barriers(mp);
			break;
		case Opt_nobarrier:
			mp->m_flags &= ~XFS_MOUNT_BARRIER;
			break;
		default:
			/*
			 * Logically we would return an error here to prevent
			 * users from believing they might have changed
			 * mount options using remount which can't be changed.
			 *
			 * But unfortunately mount(8) adds all options from
			 * mtab and fstab to the mount arguments in some cases
			 * so we can't blindly reject options, but have to
			 * check for each specified option if it actually
			 * differs from the currently set option and only
			 * reject it if that's the case.
			 *
			 * Until that is implemented we return success for
			 * every remount request, and silently ignore all
			 * options that we can't actually change.
			 */
#if 0
			printk(KERN_INFO
	"XFS: mount option \"%s\" not supported for remount\n", p);
			return -EINVAL;
#else
			break;
#endif
		}
	}

	/* rw/ro -> rw */
	if ((mp->m_flags & XFS_MOUNT_RDONLY) && !(*flags & MS_RDONLY)) {
		mp->m_flags &= ~XFS_MOUNT_RDONLY;
		if (mp->m_flags & XFS_MOUNT_BARRIER)
			xfs_mountfs_check_barriers(mp);
	}

	/* rw -> ro */
	if (!(mp->m_flags & XFS_MOUNT_RDONLY) && (*flags & MS_RDONLY)) {
		xfs_filestream_flush(mp);
		xfs_sync(mp, SYNC_DATA_QUIESCE);
		xfs_attr_quiesce(mp);
		mp->m_flags |= XFS_MOUNT_RDONLY;
	}

	return 0;
}

/*
 * Second stage of a freeze. The data is already frozen so we only
 * need to take care of themetadata. Once that's done write a dummy
 * record to dirty the log in case of a crash while frozen.
 */
STATIC void
xfs_fs_lockfs(
	struct super_block	*sb)
{
	struct xfs_mount	*mp = XFS_M(sb);

	xfs_attr_quiesce(mp);
	xfs_fs_log_dummy(mp);
}

STATIC int
xfs_fs_show_options(
	struct seq_file		*m,
	struct vfsmount		*mnt)
{
	return -xfs_showargs(XFS_M(mnt->mnt_sb), m);
}

STATIC int
xfs_fs_quotasync(
	struct super_block	*sb,
	int			type)
{
	return -XFS_QM_QUOTACTL(XFS_M(sb), Q_XQUOTASYNC, 0, NULL);
}

STATIC int
xfs_fs_getxstate(
	struct super_block	*sb,
	struct fs_quota_stat	*fqs)
{
	return -XFS_QM_QUOTACTL(XFS_M(sb), Q_XGETQSTAT, 0, (caddr_t)fqs);
}

STATIC int
xfs_fs_setxstate(
	struct super_block	*sb,
	unsigned int		flags,
	int			op)
{
	return -XFS_QM_QUOTACTL(XFS_M(sb), op, 0, (caddr_t)&flags);
}

STATIC int
xfs_fs_getxquota(
	struct super_block	*sb,
	int			type,
	qid_t			id,
	struct fs_disk_quota	*fdq)
{
	return -XFS_QM_QUOTACTL(XFS_M(sb),
				 (type == USRQUOTA) ? Q_XGETQUOTA :
				  ((type == GRPQUOTA) ? Q_XGETGQUOTA :
				   Q_XGETPQUOTA), id, (caddr_t)fdq);
}

STATIC int
xfs_fs_setxquota(
	struct super_block	*sb,
	int			type,
	qid_t			id,
	struct fs_disk_quota	*fdq)
{
	return -XFS_QM_QUOTACTL(XFS_M(sb),
				 (type == USRQUOTA) ? Q_XSETQLIM :
				  ((type == GRPQUOTA) ? Q_XSETGQLIM :
				   Q_XSETPQLIM), id, (caddr_t)fdq);
}

/*
 * This function fills in xfs_mount_t fields based on mount args.
 * Note: the superblock has _not_ yet been read in.
 */
STATIC int
xfs_start_flags(
	struct xfs_mount_args	*ap,
	struct xfs_mount	*mp)
{
	int			error;

	/* Values are in BBs */
	if ((ap->flags & XFSMNT_NOALIGN) != XFSMNT_NOALIGN) {
		/*
		 * At this point the superblock has not been read
		 * in, therefore we do not know the block size.
		 * Before the mount call ends we will convert
		 * these to FSBs.
		 */
		mp->m_dalign = ap->sunit;
		mp->m_swidth = ap->swidth;
	}

	if (ap->logbufs != -1 &&
	    ap->logbufs != 0 &&
	    (ap->logbufs < XLOG_MIN_ICLOGS ||
	     ap->logbufs > XLOG_MAX_ICLOGS)) {
		cmn_err(CE_WARN,
			"XFS: invalid logbufs value: %d [not %d-%d]",
			ap->logbufs, XLOG_MIN_ICLOGS, XLOG_MAX_ICLOGS);
		return XFS_ERROR(EINVAL);
	}
	mp->m_logbufs = ap->logbufs;
	if (ap->logbufsize != -1 &&
	    ap->logbufsize !=  0 &&
	    (ap->logbufsize < XLOG_MIN_RECORD_BSIZE ||
	     ap->logbufsize > XLOG_MAX_RECORD_BSIZE ||
	     !is_power_of_2(ap->logbufsize))) {
		cmn_err(CE_WARN,
	"XFS: invalid logbufsize: %d [not 16k,32k,64k,128k or 256k]",
			ap->logbufsize);
		return XFS_ERROR(EINVAL);
	}

	error = ENOMEM;

	mp->m_logbsize = ap->logbufsize;
	mp->m_fsname_len = strlen(ap->fsname) + 1;

	mp->m_fsname = kstrdup(ap->fsname, GFP_KERNEL);
	if (!mp->m_fsname)
		goto out;

	if (ap->rtname[0]) {
		mp->m_rtname = kstrdup(ap->rtname, GFP_KERNEL);
		if (!mp->m_rtname)
			goto out_free_fsname;

	}

	if (ap->logname[0]) {
		mp->m_logname = kstrdup(ap->logname, GFP_KERNEL);
		if (!mp->m_logname)
			goto out_free_rtname;
	}

	if (ap->flags & XFSMNT_WSYNC)
		mp->m_flags |= XFS_MOUNT_WSYNC;
#if XFS_BIG_INUMS
	if (ap->flags & XFSMNT_INO64) {
		mp->m_flags |= XFS_MOUNT_INO64;
		mp->m_inoadd = XFS_INO64_OFFSET;
	}
#endif
	if (ap->flags & XFSMNT_RETERR)
		mp->m_flags |= XFS_MOUNT_RETERR;
	if (ap->flags & XFSMNT_NOALIGN)
		mp->m_flags |= XFS_MOUNT_NOALIGN;
	if (ap->flags & XFSMNT_SWALLOC)
		mp->m_flags |= XFS_MOUNT_SWALLOC;
	if (ap->flags & XFSMNT_OSYNCISOSYNC)
		mp->m_flags |= XFS_MOUNT_OSYNCISOSYNC;
	if (ap->flags & XFSMNT_32BITINODES)
		mp->m_flags |= XFS_MOUNT_32BITINODES;

	if (ap->flags & XFSMNT_IOSIZE) {
		if (ap->iosizelog > XFS_MAX_IO_LOG ||
		    ap->iosizelog < XFS_MIN_IO_LOG) {
			cmn_err(CE_WARN,
		"XFS: invalid log iosize: %d [not %d-%d]",
				ap->iosizelog, XFS_MIN_IO_LOG,
				XFS_MAX_IO_LOG);
			return XFS_ERROR(EINVAL);
		}

		mp->m_flags |= XFS_MOUNT_DFLT_IOSIZE;
		mp->m_readio_log = mp->m_writeio_log = ap->iosizelog;
	}

	if (ap->flags & XFSMNT_IKEEP)
		mp->m_flags |= XFS_MOUNT_IKEEP;
	if (ap->flags & XFSMNT_DIRSYNC)
		mp->m_flags |= XFS_MOUNT_DIRSYNC;
	if (ap->flags & XFSMNT_ATTR2)
		mp->m_flags |= XFS_MOUNT_ATTR2;
	if (ap->flags & XFSMNT_NOATTR2)
		mp->m_flags |= XFS_MOUNT_NOATTR2;

	if (ap->flags2 & XFSMNT2_COMPAT_IOSIZE)
		mp->m_flags |= XFS_MOUNT_COMPAT_IOSIZE;

	/*
	 * no recovery flag requires a read-only mount
	 */
	if (ap->flags & XFSMNT_NORECOVERY) {
		if (!(mp->m_flags & XFS_MOUNT_RDONLY)) {
			cmn_err(CE_WARN,
	"XFS: tried to mount a FS read-write without recovery!");
			return XFS_ERROR(EINVAL);
		}
		mp->m_flags |= XFS_MOUNT_NORECOVERY;
	}

	if (ap->flags & XFSMNT_NOUUID)
		mp->m_flags |= XFS_MOUNT_NOUUID;
	if (ap->flags & XFSMNT_BARRIER)
		mp->m_flags |= XFS_MOUNT_BARRIER;
	else
		mp->m_flags &= ~XFS_MOUNT_BARRIER;

	if (ap->flags2 & XFSMNT2_FILESTREAMS)
		mp->m_flags |= XFS_MOUNT_FILESTREAMS;

	if (ap->flags & XFSMNT_DMAPI)
		mp->m_flags |= XFS_MOUNT_DMAPI;
	return 0;


 out_free_rtname:
	kfree(mp->m_rtname);
 out_free_fsname:
	kfree(mp->m_fsname);
 out:
	return error;
}

/*
 * This function fills in xfs_mount_t fields based on mount args.
 * Note: the superblock _has_ now been read in.
 */
STATIC int
xfs_finish_flags(
	struct xfs_mount_args	*ap,
	struct xfs_mount	*mp)
{
	int			ronly = (mp->m_flags & XFS_MOUNT_RDONLY);

	/* Fail a mount where the logbuf is smaller then the log stripe */
	if (xfs_sb_version_haslogv2(&mp->m_sb)) {
		if ((ap->logbufsize <= 0) &&
		    (mp->m_sb.sb_logsunit > XLOG_BIG_RECORD_BSIZE)) {
			mp->m_logbsize = mp->m_sb.sb_logsunit;
		} else if (ap->logbufsize > 0 &&
			   ap->logbufsize < mp->m_sb.sb_logsunit) {
			cmn_err(CE_WARN,
	"XFS: logbuf size must be greater than or equal to log stripe size");
			return XFS_ERROR(EINVAL);
		}
	} else {
		/* Fail a mount if the logbuf is larger than 32K */
		if (ap->logbufsize > XLOG_BIG_RECORD_BSIZE) {
			cmn_err(CE_WARN,
	"XFS: logbuf size for version 1 logs must be 16K or 32K");
			return XFS_ERROR(EINVAL);
		}
	}

	/*
	 * mkfs'ed attr2 will turn on attr2 mount unless explicitly
	 * told by noattr2 to turn it off
	 */
	if (xfs_sb_version_hasattr2(&mp->m_sb) &&
	    !(ap->flags & XFSMNT_NOATTR2))
		mp->m_flags |= XFS_MOUNT_ATTR2;

	/*
	 * prohibit r/w mounts of read-only filesystems
	 */
	if ((mp->m_sb.sb_flags & XFS_SBF_READONLY) && !ronly) {
		cmn_err(CE_WARN,
	"XFS: cannot mount a read-only filesystem as read-write");
		return XFS_ERROR(EROFS);
	}

	/*
	 * check for shared mount.
	 */
	if (ap->flags & XFSMNT_SHARED) {
		if (!xfs_sb_version_hasshared(&mp->m_sb))
			return XFS_ERROR(EINVAL);

		/*
		 * For IRIX 6.5, shared mounts must have the shared
		 * version bit set, have the persistent readonly
		 * field set, must be version 0 and can only be mounted
		 * read-only.
		 */
		if (!ronly || !(mp->m_sb.sb_flags & XFS_SBF_READONLY) ||
		     (mp->m_sb.sb_shared_vn != 0))
			return XFS_ERROR(EINVAL);

		mp->m_flags |= XFS_MOUNT_SHARED;

		/*
		 * Shared XFS V0 can't deal with DMI.  Return EINVAL.
		 */
		if (mp->m_sb.sb_shared_vn == 0 && (ap->flags & XFSMNT_DMAPI))
			return XFS_ERROR(EINVAL);
	}

	if (ap->flags & XFSMNT_UQUOTA) {
		mp->m_qflags |= (XFS_UQUOTA_ACCT | XFS_UQUOTA_ACTIVE);
		if (ap->flags & XFSMNT_UQUOTAENF)
			mp->m_qflags |= XFS_UQUOTA_ENFD;
	}

	if (ap->flags & XFSMNT_GQUOTA) {
		mp->m_qflags |= (XFS_GQUOTA_ACCT | XFS_GQUOTA_ACTIVE);
		if (ap->flags & XFSMNT_GQUOTAENF)
			mp->m_qflags |= XFS_OQUOTA_ENFD;
	} else if (ap->flags & XFSMNT_PQUOTA) {
		mp->m_qflags |= (XFS_PQUOTA_ACCT | XFS_PQUOTA_ACTIVE);
		if (ap->flags & XFSMNT_PQUOTAENF)
			mp->m_qflags |= XFS_OQUOTA_ENFD;
	}

	return 0;
}

STATIC int
xfs_fs_fill_super(
	struct super_block	*sb,
	void			*data,
	int			silent)
{
	struct inode		*root;
	struct xfs_mount	*mp = NULL;
	struct xfs_mount_args	*args;
	int			flags = 0, error = ENOMEM;

	args = xfs_args_allocate(sb, silent);
	if (!args)
		return -ENOMEM;

	mp = kzalloc(sizeof(struct xfs_mount), GFP_KERNEL);
	if (!mp)
		goto out_free_args;

	spin_lock_init(&mp->m_sb_lock);
	mutex_init(&mp->m_ilock);
	mutex_init(&mp->m_growlock);
	atomic_set(&mp->m_active_trans, 0);
	INIT_LIST_HEAD(&mp->m_sync_list);
	spin_lock_init(&mp->m_sync_lock);
	init_waitqueue_head(&mp->m_wait_single_sync_task);

	mp->m_super = sb;
	sb->s_fs_info = mp;

	if (sb->s_flags & MS_RDONLY)
		mp->m_flags |= XFS_MOUNT_RDONLY;

	error = xfs_parseargs(mp, (char *)data, args, 0);
	if (error)
		goto out_free_mp;

	sb_min_blocksize(sb, BBSIZE);
	sb->s_xattr = xfs_xattr_handlers;
	sb->s_export_op = &xfs_export_operations;
	sb->s_qcop = &xfs_quotactl_operations;
	sb->s_op = &xfs_super_operations;

	error = xfs_dmops_get(mp, args);
	if (error)
		goto out_free_mp;
	error = xfs_qmops_get(mp, args);
	if (error)
		goto out_put_dmops;

	if (args->flags & XFSMNT_QUIET)
		flags |= XFS_MFSI_QUIET;

	error = xfs_open_devices(mp, args);
	if (error)
		goto out_put_qmops;

	if (xfs_icsb_init_counters(mp))
		mp->m_flags |= XFS_MOUNT_NO_PERCPU_SB;

	/*
	 * Setup flags based on mount(2) options and then the superblock
	 */
	error = xfs_start_flags(args, mp);
	if (error)
		goto out_free_fsname;
	error = xfs_readsb(mp, flags);
	if (error)
		goto out_free_fsname;
	error = xfs_finish_flags(args, mp);
	if (error)
		goto out_free_sb;

	error = xfs_setup_devices(mp);
	if (error)
		goto out_free_sb;

	if (mp->m_flags & XFS_MOUNT_BARRIER)
		xfs_mountfs_check_barriers(mp);

	error = xfs_filestream_mount(mp);
	if (error)
		goto out_free_sb;

	error = xfs_mountfs(mp);
	if (error)
		goto out_filestream_unmount;

	XFS_SEND_MOUNT(mp, DM_RIGHT_NULL, args->mtpt, args->fsname);

	sb->s_dirt = 1;
	sb->s_magic = XFS_SB_MAGIC;
	sb->s_blocksize = mp->m_sb.sb_blocksize;
	sb->s_blocksize_bits = ffs(sb->s_blocksize) - 1;
	sb->s_maxbytes = xfs_max_file_offset(sb->s_blocksize_bits);
	sb->s_time_gran = 1;
	set_posix_acl_flag(sb);

	root = igrab(VFS_I(mp->m_rootip));
	if (!root) {
		error = ENOENT;
		goto fail_unmount;
	}
	if (is_bad_inode(root)) {
		error = EINVAL;
		goto fail_vnrele;
	}
	sb->s_root = d_alloc_root(root);
	if (!sb->s_root) {
		error = ENOMEM;
		goto fail_vnrele;
	}

	mp->m_sync_work.w_syncer = xfs_sync_worker;
	mp->m_sync_work.w_mount = mp;
	mp->m_sync_task = kthread_run(xfssyncd, mp, "xfssyncd");
	if (IS_ERR(mp->m_sync_task)) {
		error = -PTR_ERR(mp->m_sync_task);
		goto fail_vnrele;
	}

	xfs_itrace_exit(XFS_I(sb->s_root->d_inode));

	kfree(args);
	return 0;

 out_filestream_unmount:
	xfs_filestream_unmount(mp);
 out_free_sb:
	xfs_freesb(mp);
 out_free_fsname:
	xfs_free_fsname(mp);
	xfs_icsb_destroy_counters(mp);
	xfs_close_devices(mp);
 out_put_qmops:
	xfs_qmops_put(mp);
 out_put_dmops:
	xfs_dmops_put(mp);
 out_free_mp:
	kfree(mp);
 out_free_args:
	kfree(args);
	return -error;

 fail_vnrele:
	if (sb->s_root) {
		dput(sb->s_root);
		sb->s_root = NULL;
	} else {
		iput(root);
	}

 fail_unmount:
	/*
	 * Blow away any referenced inode in the filestreams cache.
	 * This can and will cause log traffic as inodes go inactive
	 * here.
	 */
	xfs_filestream_unmount(mp);

	XFS_bflush(mp->m_ddev_targp);
	error = xfs_unmount_flush(mp, 0);
	WARN_ON(error);

	xfs_unmountfs(mp);
	goto out_free_sb;
}

STATIC int
xfs_fs_get_sb(
	struct file_system_type	*fs_type,
	int			flags,
	const char		*dev_name,
	void			*data,
	struct vfsmount		*mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, xfs_fs_fill_super,
			   mnt);
}

static struct super_operations xfs_super_operations = {
	.alloc_inode		= xfs_fs_alloc_inode,
	.destroy_inode		= xfs_fs_destroy_inode,
	.write_inode		= xfs_fs_write_inode,
	.clear_inode		= xfs_fs_clear_inode,
	.put_super		= xfs_fs_put_super,
	.write_super		= xfs_fs_write_super,
	.sync_fs		= xfs_fs_sync_super,
	.write_super_lockfs	= xfs_fs_lockfs,
	.statfs			= xfs_fs_statfs,
	.remount_fs		= xfs_fs_remount,
	.show_options		= xfs_fs_show_options,
};

static struct quotactl_ops xfs_quotactl_operations = {
	.quota_sync		= xfs_fs_quotasync,
	.get_xstate		= xfs_fs_getxstate,
	.set_xstate		= xfs_fs_setxstate,
	.get_xquota		= xfs_fs_getxquota,
	.set_xquota		= xfs_fs_setxquota,
};

static struct file_system_type xfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "xfs",
	.get_sb			= xfs_fs_get_sb,
	.kill_sb		= kill_block_super,
	.fs_flags		= FS_REQUIRES_DEV,
};

STATIC int __init
xfs_alloc_trace_bufs(void)
{
#ifdef XFS_ALLOC_TRACE
	xfs_alloc_trace_buf = ktrace_alloc(XFS_ALLOC_TRACE_SIZE, KM_MAYFAIL);
	if (!xfs_alloc_trace_buf)
		goto out;
#endif
#ifdef XFS_BMAP_TRACE
	xfs_bmap_trace_buf = ktrace_alloc(XFS_BMAP_TRACE_SIZE, KM_MAYFAIL);
	if (!xfs_bmap_trace_buf)
		goto out_free_alloc_trace;
#endif
#ifdef XFS_BMBT_TRACE
	xfs_bmbt_trace_buf = ktrace_alloc(XFS_BMBT_TRACE_SIZE, KM_MAYFAIL);
	if (!xfs_bmbt_trace_buf)
		goto out_free_bmap_trace;
#endif
#ifdef XFS_ATTR_TRACE
	xfs_attr_trace_buf = ktrace_alloc(XFS_ATTR_TRACE_SIZE, KM_MAYFAIL);
	if (!xfs_attr_trace_buf)
		goto out_free_bmbt_trace;
#endif
#ifdef XFS_DIR2_TRACE
	xfs_dir2_trace_buf = ktrace_alloc(XFS_DIR2_GTRACE_SIZE, KM_MAYFAIL);
	if (!xfs_dir2_trace_buf)
		goto out_free_attr_trace;
#endif

	return 0;

#ifdef XFS_DIR2_TRACE
 out_free_attr_trace:
#endif
#ifdef XFS_ATTR_TRACE
	ktrace_free(xfs_attr_trace_buf);
 out_free_bmbt_trace:
#endif
#ifdef XFS_BMBT_TRACE
	ktrace_free(xfs_bmbt_trace_buf);
 out_free_bmap_trace:
#endif
#ifdef XFS_BMAP_TRACE
	ktrace_free(xfs_bmap_trace_buf);
 out_free_alloc_trace:
#endif
#ifdef XFS_ALLOC_TRACE
	ktrace_free(xfs_alloc_trace_buf);
 out:
#endif
	return -ENOMEM;
}

STATIC void
xfs_free_trace_bufs(void)
{
#ifdef XFS_DIR2_TRACE
	ktrace_free(xfs_dir2_trace_buf);
#endif
#ifdef XFS_ATTR_TRACE
	ktrace_free(xfs_attr_trace_buf);
#endif
#ifdef XFS_BMBT_TRACE
	ktrace_free(xfs_bmbt_trace_buf);
#endif
#ifdef XFS_BMAP_TRACE
	ktrace_free(xfs_bmap_trace_buf);
#endif
#ifdef XFS_ALLOC_TRACE
	ktrace_free(xfs_alloc_trace_buf);
#endif
}

STATIC int __init
xfs_init_zones(void)
{
	xfs_vnode_zone = kmem_zone_init_flags(sizeof(struct inode), "xfs_vnode",
					KM_ZONE_HWALIGN | KM_ZONE_RECLAIM |
					KM_ZONE_SPREAD,
					xfs_fs_inode_init_once);
	if (!xfs_vnode_zone)
		goto out;

	xfs_ioend_zone = kmem_zone_init(sizeof(xfs_ioend_t), "xfs_ioend");
	if (!xfs_ioend_zone)
		goto out_destroy_vnode_zone;

	xfs_ioend_pool = mempool_create_slab_pool(4 * MAX_BUF_PER_PAGE,
						  xfs_ioend_zone);
	if (!xfs_ioend_pool)
		goto out_destroy_ioend_zone;

	xfs_log_ticket_zone = kmem_zone_init(sizeof(xlog_ticket_t),
						"xfs_log_ticket");
	if (!xfs_log_ticket_zone)
		goto out_destroy_ioend_pool;

	xfs_bmap_free_item_zone = kmem_zone_init(sizeof(xfs_bmap_free_item_t),
						"xfs_bmap_free_item");
	if (!xfs_bmap_free_item_zone)
		goto out_destroy_log_ticket_zone;
	xfs_btree_cur_zone = kmem_zone_init(sizeof(xfs_btree_cur_t),
						"xfs_btree_cur");
	if (!xfs_btree_cur_zone)
		goto out_destroy_bmap_free_item_zone;

	xfs_da_state_zone = kmem_zone_init(sizeof(xfs_da_state_t),
						"xfs_da_state");
	if (!xfs_da_state_zone)
		goto out_destroy_btree_cur_zone;

	xfs_dabuf_zone = kmem_zone_init(sizeof(xfs_dabuf_t), "xfs_dabuf");
	if (!xfs_dabuf_zone)
		goto out_destroy_da_state_zone;

	xfs_ifork_zone = kmem_zone_init(sizeof(xfs_ifork_t), "xfs_ifork");
	if (!xfs_ifork_zone)
		goto out_destroy_dabuf_zone;

	xfs_trans_zone = kmem_zone_init(sizeof(xfs_trans_t), "xfs_trans");
	if (!xfs_trans_zone)
		goto out_destroy_ifork_zone;

	/*
	 * The size of the zone allocated buf log item is the maximum
	 * size possible under XFS.  This wastes a little bit of memory,
	 * but it is much faster.
	 */
	xfs_buf_item_zone = kmem_zone_init((sizeof(xfs_buf_log_item_t) +
				(((XFS_MAX_BLOCKSIZE / XFS_BLI_CHUNK) /
				  NBWORD) * sizeof(int))), "xfs_buf_item");
	if (!xfs_buf_item_zone)
		goto out_destroy_trans_zone;

	xfs_efd_zone = kmem_zone_init((sizeof(xfs_efd_log_item_t) +
			((XFS_EFD_MAX_FAST_EXTENTS - 1) *
				 sizeof(xfs_extent_t))), "xfs_efd_item");
	if (!xfs_efd_zone)
		goto out_destroy_buf_item_zone;

	xfs_efi_zone = kmem_zone_init((sizeof(xfs_efi_log_item_t) +
			((XFS_EFI_MAX_FAST_EXTENTS - 1) *
				sizeof(xfs_extent_t))), "xfs_efi_item");
	if (!xfs_efi_zone)
		goto out_destroy_efd_zone;

	xfs_inode_zone =
		kmem_zone_init_flags(sizeof(xfs_inode_t), "xfs_inode",
					KM_ZONE_HWALIGN | KM_ZONE_RECLAIM |
					KM_ZONE_SPREAD, NULL);
	if (!xfs_inode_zone)
		goto out_destroy_efi_zone;

	xfs_ili_zone =
		kmem_zone_init_flags(sizeof(xfs_inode_log_item_t), "xfs_ili",
					KM_ZONE_SPREAD, NULL);
	if (!xfs_ili_zone)
		goto out_destroy_inode_zone;

#ifdef CONFIG_XFS_POSIX_ACL
	xfs_acl_zone = kmem_zone_init(sizeof(xfs_acl_t), "xfs_acl");
	if (!xfs_acl_zone)
		goto out_destroy_ili_zone;
#endif

	return 0;

#ifdef CONFIG_XFS_POSIX_ACL
 out_destroy_ili_zone:
#endif
	kmem_zone_destroy(xfs_ili_zone);
 out_destroy_inode_zone:
	kmem_zone_destroy(xfs_inode_zone);
 out_destroy_efi_zone:
	kmem_zone_destroy(xfs_efi_zone);
 out_destroy_efd_zone:
	kmem_zone_destroy(xfs_efd_zone);
 out_destroy_buf_item_zone:
	kmem_zone_destroy(xfs_buf_item_zone);
 out_destroy_trans_zone:
	kmem_zone_destroy(xfs_trans_zone);
 out_destroy_ifork_zone:
	kmem_zone_destroy(xfs_ifork_zone);
 out_destroy_dabuf_zone:
	kmem_zone_destroy(xfs_dabuf_zone);
 out_destroy_da_state_zone:
	kmem_zone_destroy(xfs_da_state_zone);
 out_destroy_btree_cur_zone:
	kmem_zone_destroy(xfs_btree_cur_zone);
 out_destroy_bmap_free_item_zone:
	kmem_zone_destroy(xfs_bmap_free_item_zone);
 out_destroy_log_ticket_zone:
	kmem_zone_destroy(xfs_log_ticket_zone);
 out_destroy_ioend_pool:
	mempool_destroy(xfs_ioend_pool);
 out_destroy_ioend_zone:
	kmem_zone_destroy(xfs_ioend_zone);
 out_destroy_vnode_zone:
	kmem_zone_destroy(xfs_vnode_zone);
 out:
	return -ENOMEM;
}

STATIC void
xfs_destroy_zones(void)
{
#ifdef CONFIG_XFS_POSIX_ACL
	kmem_zone_destroy(xfs_acl_zone);
#endif
	kmem_zone_destroy(xfs_ili_zone);
	kmem_zone_destroy(xfs_inode_zone);
	kmem_zone_destroy(xfs_efi_zone);
	kmem_zone_destroy(xfs_efd_zone);
	kmem_zone_destroy(xfs_buf_item_zone);
	kmem_zone_destroy(xfs_trans_zone);
	kmem_zone_destroy(xfs_ifork_zone);
	kmem_zone_destroy(xfs_dabuf_zone);
	kmem_zone_destroy(xfs_da_state_zone);
	kmem_zone_destroy(xfs_btree_cur_zone);
	kmem_zone_destroy(xfs_bmap_free_item_zone);
	kmem_zone_destroy(xfs_log_ticket_zone);
	mempool_destroy(xfs_ioend_pool);
	kmem_zone_destroy(xfs_ioend_zone);
	kmem_zone_destroy(xfs_vnode_zone);

}

STATIC int __init
init_xfs_fs(void)
{
	int			error;
	static char		message[] __initdata = KERN_INFO \
		XFS_VERSION_STRING " with " XFS_BUILD_OPTIONS " enabled\n";

	printk(message);

	ktrace_init(64);
	vn_init();
	xfs_dir_startup();

	error = xfs_init_zones();
	if (error)
		goto out;

	error = xfs_alloc_trace_bufs();
	if (error)
		goto out_destroy_zones;

	error = xfs_mru_cache_init();
	if (error)
		goto out_free_trace_buffers;

	error = xfs_filestream_init();
	if (error)
		goto out_mru_cache_uninit;

	error = xfs_buf_init();
	if (error)
		goto out_filestream_uninit;

	error = xfs_init_procfs();
	if (error)
		goto out_buf_terminate;

	error = xfs_sysctl_register();
	if (error)
		goto out_cleanup_procfs;

	vfs_initquota();

	error = register_filesystem(&xfs_fs_type);
	if (error)
		goto out_sysctl_unregister;
	return 0;

 out_sysctl_unregister:
	xfs_sysctl_unregister();
 out_cleanup_procfs:
	xfs_cleanup_procfs();
 out_buf_terminate:
	xfs_buf_terminate();
 out_filestream_uninit:
	xfs_filestream_uninit();
 out_mru_cache_uninit:
	xfs_mru_cache_uninit();
 out_free_trace_buffers:
	xfs_free_trace_bufs();
 out_destroy_zones:
	xfs_destroy_zones();
 out:
	return error;
}

STATIC void __exit
exit_xfs_fs(void)
{
	vfs_exitquota();
	unregister_filesystem(&xfs_fs_type);
	xfs_sysctl_unregister();
	xfs_cleanup_procfs();
	xfs_buf_terminate();
	xfs_filestream_uninit();
	xfs_mru_cache_uninit();
	xfs_free_trace_bufs();
	xfs_destroy_zones();
	ktrace_uninit();
}

module_init(init_xfs_fs);
module_exit(exit_xfs_fs);

MODULE_AUTHOR("Silicon Graphics, Inc.");
MODULE_DESCRIPTION(XFS_VERSION_STRING " with " XFS_BUILD_OPTIONS " enabled");
MODULE_LICENSE("GPL");
