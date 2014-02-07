/*
 * linux/arch/sh/kernel/sys_sh.c
 *
 * This file contains various random system calls that
 * have a non-standard calling sequence on the Linux/SuperH
 * platform.
 *
 * Taken from i386 version.
 */
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sem.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include <linux/stat.h>
#include <linux/syscalls.h>
#include <linux/mman.h>
#include <linux/file.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/ipc.h>
#include <asm/syscalls.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/cacheflush.h>
#include <asm/cachectl.h>

asmlinkage int old_mmap(unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags,
	int fd, unsigned long off)
{
	if (off & ~PAGE_MASK)
		return -EINVAL;
	return sys_mmap_pgoff(addr, len, prot, flags, fd, off>>PAGE_SHIFT);
}

asmlinkage long sys_mmap2(unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags,
	unsigned long fd, unsigned long pgoff)
{
	/*
	 * The shift for mmap2 is constant, regardless of PAGE_SIZE
	 * setting.
	 */
	if (pgoff & ((1 << (PAGE_SHIFT - 12)) - 1))
		return -EINVAL;

	pgoff >>= PAGE_SHIFT - 12;

	return sys_mmap_pgoff(addr, len, prot, flags, fd, pgoff);
}

/*
 * sys_ipc() is the de-multiplexer for the SysV IPC calls..
 *
 * This is really horribly ugly.
 */
asmlinkage int sys_ipc(uint call, int first, int second,
		       int third, void __user *ptr, long fifth)
{
	int version, ret;

	version = call >> 16; /* hack for backward compatibility */
	call &= 0xffff;

	if (call <= SEMTIMEDOP)
		switch (call) {
		case SEMOP:
			return sys_semtimedop(first,
					      (struct sembuf __user *)ptr,
					      second, NULL);
		case SEMTIMEDOP:
			return sys_semtimedop(first,
				(struct sembuf __user *)ptr, second,
			        (const struct timespec __user *)fifth);
		case SEMGET:
			return sys_semget (first, second, third);
		case SEMCTL: {
			union semun fourth;
			if (!ptr)
				return -EINVAL;
			if (get_user(fourth.__pad, (void __user * __user *) ptr))
				return -EFAULT;
			return sys_semctl (first, second, third, fourth);
			}
		default:
			return -EINVAL;
		}

	if (call <= MSGCTL)
		switch (call) {
		case MSGSND:
			return sys_msgsnd (first, (struct msgbuf __user *) ptr,
					  second, third);
		case MSGRCV:
			switch (version) {
			case 0:
			{
				struct ipc_kludge tmp;

				if (!ptr)
					return -EINVAL;

				if (copy_from_user(&tmp,
					(struct ipc_kludge __user *) ptr,
						   sizeof (tmp)))
					return -EFAULT;

				return sys_msgrcv (first, tmp.msgp, second,
						   tmp.msgtyp, third);
			}
			default:
				return sys_msgrcv (first,
						   (struct msgbuf __user *) ptr,
						   second, fifth, third);
			}
		case MSGGET:
			return sys_msgget ((key_t) first, second);
		case MSGCTL:
			return sys_msgctl (first, second,
					   (struct msqid_ds __user *) ptr);
		default:
			return -EINVAL;
		}
	if (call <= SHMCTL)
		switch (call) {
		case SHMAT:
			switch (version) {
			default: {
				ulong raddr;
				ret = do_shmat (first, (char __user *) ptr,
						 second, &raddr);
				if (ret)
					return ret;
				return put_user (raddr, (ulong __user *) third);
			}
			case 1:	/* iBCS2 emulator entry point */
				if (!segment_eq(get_fs(), get_ds()))
					return -EINVAL;
				return do_shmat (first, (char __user *) ptr,
						  second, (ulong *) third);
			}
		case SHMDT:
			return sys_shmdt ((char __user *)ptr);
		case SHMGET:
			return sys_shmget (first, second, third);
		case SHMCTL:
			return sys_shmctl (first, second,
					   (struct shmid_ds __user *) ptr);
		default:
			return -EINVAL;
		}

	return -EINVAL;
}

/* sys_cacheflush -- flush (part of) the processor cache.  */
asmlinkage int sys_cacheflush(unsigned long addr, unsigned long len, int op)
{
	struct vm_area_struct *vma;

	if ((op <= 0) || (op > (CACHEFLUSH_D_PURGE|CACHEFLUSH_I)))
		return -EINVAL;

	/*
	 * Verify that the specified address region actually belongs
	 * to this process.
	 */
	if (addr + len < addr)
		return -EFAULT;

	down_read(&current->mm->mmap_sem);
	vma = find_vma (current->mm, addr);
	if (vma == NULL || addr < vma->vm_start || addr + len > vma->vm_end) {
		up_read(&current->mm->mmap_sem);
		return -EFAULT;
	}

	switch (op & CACHEFLUSH_D_PURGE) {
		case CACHEFLUSH_D_INVAL:
			__flush_invalidate_region((void *)addr, len);
			break;
		case CACHEFLUSH_D_WB:
			__flush_wback_region((void *)addr, len);
			break;
		case CACHEFLUSH_D_PURGE:
			__flush_purge_region((void *)addr, len);
			break;
	}

	if (op & CACHEFLUSH_I)
		flush_cache_all();

	up_read(&current->mm->mmap_sem);
	return 0;
}

asmlinkage int sys_uname(struct old_utsname __user *name)
{
	int err;
	if (!name)
		return -EFAULT;
	down_read(&uts_sem);
	err = copy_to_user(name, utsname(), sizeof(*name));
	up_read(&uts_sem);
	return err?-EFAULT:0;
}
