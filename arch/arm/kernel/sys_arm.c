/*
 *  linux/arch/arm/kernel/sys_arm.c
 *
 *  Copyright (C) People who wrote linux/arch/i386/kernel/sys_i386.c
 *  Copyright (C) 1995, 1996 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This file contains various random system calls that
 *  have a non-standard calling sequence on the Linux/arm
 *  platform.
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sem.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include <linux/stat.h>
#include <linux/syscalls.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/ipc.h>
#include <linux/uaccess.h>

struct mmap_arg_struct {
	unsigned long addr;
	unsigned long len;
	unsigned long prot;
	unsigned long flags;
	unsigned long fd;
	unsigned long offset;
};

asmlinkage int old_mmap(struct mmap_arg_struct __user *arg)
{
	int error = -EFAULT;
	struct mmap_arg_struct a;

	if (copy_from_user(&a, arg, sizeof(a)))
		goto out;

	error = -EINVAL;
	if (a.offset & ~PAGE_MASK)
		goto out;

	error = sys_mmap_pgoff(a.addr, a.len, a.prot, a.flags, a.fd, a.offset >> PAGE_SHIFT);
out:
	return error;
}

/*
 * Perform the select(nd, in, out, ex, tv) and mmap() system
 * calls.
 */

struct sel_arg_struct {
	unsigned long n;
	fd_set __user *inp, *outp, *exp;
	struct timeval __user *tvp;
};

asmlinkage int old_select(struct sel_arg_struct __user *arg)
{
	struct sel_arg_struct a;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	/* sys_select() does the appropriate kernel locking */
	return sys_select(a.n, a.inp, a.outp, a.exp, a.tvp);
}

#if !defined(CONFIG_AEABI) || defined(CONFIG_OABI_COMPAT)
/*
 * sys_ipc() is the de-multiplexer for the SysV IPC calls..
 *
 * This is really horribly ugly.
 */
asmlinkage int sys_ipc(uint call, int first, int second, int third,
		       void __user *ptr, long fifth)
{
	int version, ret;

	version = call >> 16; /* hack for backward compatibility */
	call &= 0xffff;

	switch (call) {
	case SEMOP:
		return sys_semtimedop (first, (struct sembuf __user *)ptr, second, NULL);
	case SEMTIMEDOP:
		return sys_semtimedop(first, (struct sembuf __user *)ptr, second,
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

	case MSGSND:
		return sys_msgsnd(first, (struct msgbuf __user *) ptr, 
				  second, third);
	case MSGRCV:
		switch (version) {
		case 0: {
			struct ipc_kludge tmp;
			if (!ptr)
				return -EINVAL;
			if (copy_from_user(&tmp,(struct ipc_kludge __user *)ptr,
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
		return sys_msgctl(first, second, (struct msqid_ds __user *)ptr);

	case SHMAT:
		switch (version) {
		default: {
			ulong raddr;
			ret = do_shmat(first, (char __user *)ptr, second, &raddr);
			if (ret)
				return ret;
			return put_user(raddr, (ulong __user *)third);
		}
		case 1: /* Of course, we don't support iBCS2! */
			return -EINVAL;
		}
	case SHMDT: 
		return sys_shmdt ((char __user *)ptr);
	case SHMGET:
		return sys_shmget (first, second, third);
	case SHMCTL:
		return sys_shmctl (first, second,
				   (struct shmid_ds __user *) ptr);
	default:
		return -ENOSYS;
	}
}
#endif

/* Fork a new task - this creates a new program thread.
 * This is called indirectly via a small wrapper
 */
asmlinkage int sys_fork(struct pt_regs *regs)
{
#ifdef CONFIG_MMU
	return do_fork(SIGCHLD, regs->ARM_sp, regs, 0, NULL, NULL);
#else
	/* can not support in nommu mode */
	return(-EINVAL);
#endif
}

/* Clone a task - this clones the calling program thread.
 * This is called indirectly via a small wrapper
 */
asmlinkage int sys_clone(unsigned long clone_flags, unsigned long newsp,
			 int __user *parent_tidptr, int tls_val,
			 int __user *child_tidptr, struct pt_regs *regs)
{
	if (!newsp)
		newsp = regs->ARM_sp;

	return do_fork(clone_flags, newsp, regs, 0, parent_tidptr, child_tidptr);
}

asmlinkage int sys_vfork(struct pt_regs *regs)
{
	return do_fork(CLONE_VFORK | CLONE_VM | SIGCHLD, regs->ARM_sp, regs, 0, NULL, NULL);
}

/* sys_execve() executes a new program.
 * This is called indirectly via a small wrapper
 */
asmlinkage int sys_execve(char __user *filenamei, char __user * __user *argv,
			  char __user * __user *envp, struct pt_regs *regs)
{
	int error;
	char * filename;

	filename = getname(filenamei);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;
	error = do_execve(filename, argv, envp, regs);
	putname(filename);
out:
	return error;
}

int kernel_execve(const char *filename, char *const argv[], char *const envp[])
{
	struct pt_regs regs;
	int ret;

	memset(&regs, 0, sizeof(struct pt_regs));
	ret = do_execve((char *)filename, (char __user * __user *)argv,
			(char __user * __user *)envp, &regs);
	if (ret < 0)
		goto out;

	/*
	 * Save argc to the register structure for userspace.
	 */
	regs.ARM_r0 = ret;

	/*
	 * We were successful.  We won't be returning to our caller, but
	 * instead to user space by manipulating the kernel stack.
	 */
	asm(	"add	r0, %0, %1\n\t"
		"mov	r1, %2\n\t"
		"mov	r2, %3\n\t"
		"bl	memmove\n\t"	/* copy regs to top of stack */
		"mov	r8, #0\n\t"	/* not a syscall */
		"mov	r9, %0\n\t"	/* thread structure */
		"mov	sp, r0\n\t"	/* reposition stack pointer */
		"b	ret_to_user"
		:
		: "r" (current_thread_info()),
		  "Ir" (THREAD_START_SP - sizeof(regs)),
		  "r" (&regs),
		  "Ir" (sizeof(regs))
		: "r0", "r1", "r2", "r3", "r8", "r9", "ip", "lr", "memory");

 out:
	return ret;
}
EXPORT_SYMBOL(kernel_execve);

/*
 * Since loff_t is a 64 bit type we avoid a lot of ABI hassle
 * with a different argument ordering.
 */
asmlinkage long sys_arm_fadvise64_64(int fd, int advice,
				     loff_t offset, loff_t len)
{
	return sys_fadvise64_64(fd, offset, len, advice);
}
