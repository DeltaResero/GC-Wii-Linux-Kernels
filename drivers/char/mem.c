/*
 *  linux/drivers/char/mem.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Added devfs support. 
 *    Jan-11-1998, C. Scott Ananian <cananian@alumni.princeton.edu>
 *  Shared /dev/zero mmaping support, Feb 2000, Kanoj Sarcar <kanoj@sgi.com>
 */

#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/raw.h>
#include <linux/tty.h>
#include <linux/capability.h>
#include <linux/ptrace.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/crash_dump.h>
#include <linux/backing-dev.h>
#include <linux/bootmem.h>
#include <linux/splice.h>
#include <linux/pfn.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/io.h>

#ifdef CONFIG_IA64
# include <linux/efi.h>
#endif

/*
 * Architectures vary in how they handle caching for addresses
 * outside of main memory.
 *
 */
static inline int uncached_access(struct file *file, unsigned long addr)
{
#if defined(CONFIG_IA64)
	/*
	 * On ia64, we ignore O_SYNC because we cannot tolerate memory attribute aliases.
	 */
	return !(efi_mem_attributes(addr) & EFI_MEMORY_WB);
#elif defined(CONFIG_MIPS)
	{
		extern int __uncached_access(struct file *file,
					     unsigned long addr);

		return __uncached_access(file, addr);
	}
#else
	/*
	 * Accessing memory above the top the kernel knows about or through a file pointer
	 * that was marked O_SYNC will be done non-cached.
	 */
	if (file->f_flags & O_SYNC)
		return 1;
	return addr >= __pa(high_memory);
#endif
}

#ifndef ARCH_HAS_VALID_PHYS_ADDR_RANGE
static inline int valid_phys_addr_range(unsigned long addr, size_t count)
{
	if (addr + count > __pa(high_memory))
		return 0;

	return 1;
}

static inline int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	return 1;
}
#endif

#ifdef CONFIG_STRICT_DEVMEM
static inline int range_is_allowed(unsigned long pfn, unsigned long size)
{
	u64 from = ((u64)pfn) << PAGE_SHIFT;
	u64 to = from + size;
	u64 cursor = from;

	while (cursor < to) {
		if (!devmem_is_allowed(pfn)) {
			printk(KERN_INFO
		"Program %s tried to access /dev/mem between %Lx->%Lx.\n",
				current->comm, from, to);
			return 0;
		}
		cursor += PAGE_SIZE;
		pfn++;
	}
	return 1;
}
#else
static inline int range_is_allowed(unsigned long pfn, unsigned long size)
{
	return 1;
}
#endif

void __attribute__((weak)) unxlate_dev_mem_ptr(unsigned long phys, void *addr)
{
}

/*
 * This funcion reads the *physical* memory. The f_pos points directly to the 
 * memory location. 
 */
static ssize_t read_mem(struct file * file, char __user * buf,
			size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	ssize_t read, sz;
	char *ptr;

	if (!valid_phys_addr_range(p, count))
		return -EFAULT;
	read = 0;
#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
	/* we don't have page 0 mapped on sparc and m68k.. */
	if (p < PAGE_SIZE) {
		sz = PAGE_SIZE - p;
		if (sz > count) 
			sz = count; 
		if (sz > 0) {
			if (clear_user(buf, sz))
				return -EFAULT;
			buf += sz; 
			p += sz; 
			count -= sz; 
			read += sz; 
		}
	}
#endif

	while (count > 0) {
		/*
		 * Handle first page in case it's not aligned
		 */
		if (-p & (PAGE_SIZE - 1))
			sz = -p & (PAGE_SIZE - 1);
		else
			sz = PAGE_SIZE;

		sz = min_t(unsigned long, sz, count);

		if (!range_is_allowed(p >> PAGE_SHIFT, count))
			return -EPERM;

		/*
		 * On ia64 if a page has been mapped somewhere as
		 * uncached, then it must also be accessed uncached
		 * by the kernel or data corruption may occur
		 */
		ptr = xlate_dev_mem_ptr(p);
		if (!ptr)
			return -EFAULT;

		if (copy_to_user(buf, ptr, sz)) {
			unxlate_dev_mem_ptr(p, ptr);
			return -EFAULT;
		}

		unxlate_dev_mem_ptr(p, ptr);

		buf += sz;
		p += sz;
		count -= sz;
		read += sz;
	}

	*ppos += read;
	return read;
}

static ssize_t write_mem(struct file * file, const char __user * buf, 
			 size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	ssize_t written, sz;
	unsigned long copied;
	void *ptr;

	if (!valid_phys_addr_range(p, count))
		return -EFAULT;

	written = 0;

#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
	/* we don't have page 0 mapped on sparc and m68k.. */
	if (p < PAGE_SIZE) {
		unsigned long sz = PAGE_SIZE - p;
		if (sz > count)
			sz = count;
		/* Hmm. Do something? */
		buf += sz;
		p += sz;
		count -= sz;
		written += sz;
	}
#endif

	while (count > 0) {
		/*
		 * Handle first page in case it's not aligned
		 */
		if (-p & (PAGE_SIZE - 1))
			sz = -p & (PAGE_SIZE - 1);
		else
			sz = PAGE_SIZE;

		sz = min_t(unsigned long, sz, count);

		if (!range_is_allowed(p >> PAGE_SHIFT, sz))
			return -EPERM;

		/*
		 * On ia64 if a page has been mapped somewhere as
		 * uncached, then it must also be accessed uncached
		 * by the kernel or data corruption may occur
		 */
		ptr = xlate_dev_mem_ptr(p);
		if (!ptr) {
			if (written)
				break;
			return -EFAULT;
		}

		copied = copy_from_user(ptr, buf, sz);
		if (copied) {
			written += sz - copied;
			unxlate_dev_mem_ptr(p, ptr);
			if (written)
				break;
			return -EFAULT;
		}

		unxlate_dev_mem_ptr(p, ptr);

		buf += sz;
		p += sz;
		count -= sz;
		written += sz;
	}

	*ppos += written;
	return written;
}

int __attribute__((weak)) phys_mem_access_prot_allowed(struct file *file,
	unsigned long pfn, unsigned long size, pgprot_t *vma_prot)
{
	return 1;
}

#ifndef __HAVE_PHYS_MEM_ACCESS_PROT
static pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
				     unsigned long size, pgprot_t vma_prot)
{
#ifdef pgprot_noncached
	unsigned long offset = pfn << PAGE_SHIFT;

	if (uncached_access(file, offset))
		return pgprot_noncached(vma_prot);
#endif
	return vma_prot;
}
#endif

#ifndef CONFIG_MMU
static unsigned long get_unmapped_area_mem(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long pgoff,
					   unsigned long flags)
{
	if (!valid_mmap_phys_addr_range(pgoff, len))
		return (unsigned long) -EINVAL;
	return pgoff << PAGE_SHIFT;
}

/* can't do an in-place private mapping if there's no MMU */
static inline int private_mapping_ok(struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_MAYSHARE;
}
#else
#define get_unmapped_area_mem	NULL

static inline int private_mapping_ok(struct vm_area_struct *vma)
{
	return 1;
}
#endif

void __attribute__((weak))
map_devmem(unsigned long pfn, unsigned long len, pgprot_t prot)
{
	/* nothing. architectures can override. */
}

void __attribute__((weak))
unmap_devmem(unsigned long pfn, unsigned long len, pgprot_t prot)
{
	/* nothing. architectures can override. */
}

static void mmap_mem_open(struct vm_area_struct *vma)
{
	map_devmem(vma->vm_pgoff,  vma->vm_end - vma->vm_start,
			vma->vm_page_prot);
}

static void mmap_mem_close(struct vm_area_struct *vma)
{
	unmap_devmem(vma->vm_pgoff,  vma->vm_end - vma->vm_start,
			vma->vm_page_prot);
}

static struct vm_operations_struct mmap_mem_ops = {
	.open  = mmap_mem_open,
	.close = mmap_mem_close,
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

static int mmap_mem(struct file * file, struct vm_area_struct * vma)
{
	size_t size = vma->vm_end - vma->vm_start;

	if (!valid_mmap_phys_addr_range(vma->vm_pgoff, size))
		return -EINVAL;

	if (!private_mapping_ok(vma))
		return -ENOSYS;

	if (!range_is_allowed(vma->vm_pgoff, size))
		return -EPERM;

	if (!phys_mem_access_prot_allowed(file, vma->vm_pgoff, size,
						&vma->vm_page_prot))
		return -EINVAL;

	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff,
						 size,
						 vma->vm_page_prot);

	vma->vm_ops = &mmap_mem_ops;

	/* Remap-pfn-range will mark the range VM_IO and VM_RESERVED */
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    vma->vm_pgoff,
			    size,
			    vma->vm_page_prot)) {
		unmap_devmem(vma->vm_pgoff, size, vma->vm_page_prot);
		return -EAGAIN;
	}
	return 0;
}

#ifdef CONFIG_DEVKMEM
static int mmap_kmem(struct file * file, struct vm_area_struct * vma)
{
	unsigned long pfn;

	/* Turn a kernel-virtual address into a physical page frame */
	pfn = __pa((u64)vma->vm_pgoff << PAGE_SHIFT) >> PAGE_SHIFT;

	/*
	 * RED-PEN: on some architectures there is more mapped memory
	 * than available in mem_map which pfn_valid checks
	 * for. Perhaps should add a new macro here.
	 *
	 * RED-PEN: vmalloc is not supported right now.
	 */
	if (!pfn_valid(pfn))
		return -EIO;

	vma->vm_pgoff = pfn;
	return mmap_mem(file, vma);
}
#endif

#ifdef CONFIG_CRASH_DUMP
/*
 * Read memory corresponding to the old kernel.
 */
static ssize_t read_oldmem(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	unsigned long pfn, offset;
	size_t read = 0, csize;
	int rc = 0;

	while (count) {
		pfn = *ppos / PAGE_SIZE;
		if (pfn > saved_max_pfn)
			return read;

		offset = (unsigned long)(*ppos % PAGE_SIZE);
		if (count > PAGE_SIZE - offset)
			csize = PAGE_SIZE - offset;
		else
			csize = count;

		rc = copy_oldmem_page(pfn, buf, csize, offset, 1);
		if (rc < 0)
			return rc;
		buf += csize;
		*ppos += csize;
		read += csize;
		count -= csize;
	}
	return read;
}
#endif

extern long vread(char *buf, char *addr, unsigned long count);
extern long vwrite(char *buf, char *addr, unsigned long count);

#ifdef CONFIG_DEVKMEM
/*
 * This function reads the *virtual* memory as seen by the kernel.
 */
static ssize_t read_kmem(struct file *file, char __user *buf, 
			 size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	ssize_t low_count, read, sz;
	char * kbuf; /* k-addr because vread() takes vmlist_lock rwlock */

	read = 0;
	if (p < (unsigned long) high_memory) {
		low_count = count;
		if (count > (unsigned long) high_memory - p)
			low_count = (unsigned long) high_memory - p;

#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
		/* we don't have page 0 mapped on sparc and m68k.. */
		if (p < PAGE_SIZE && low_count > 0) {
			size_t tmp = PAGE_SIZE - p;
			if (tmp > low_count) tmp = low_count;
			if (clear_user(buf, tmp))
				return -EFAULT;
			buf += tmp;
			p += tmp;
			read += tmp;
			low_count -= tmp;
			count -= tmp;
		}
#endif
		while (low_count > 0) {
			/*
			 * Handle first page in case it's not aligned
			 */
			if (-p & (PAGE_SIZE - 1))
				sz = -p & (PAGE_SIZE - 1);
			else
				sz = PAGE_SIZE;

			sz = min_t(unsigned long, sz, low_count);

			/*
			 * On ia64 if a page has been mapped somewhere as
			 * uncached, then it must also be accessed uncached
			 * by the kernel or data corruption may occur
			 */
			kbuf = xlate_dev_kmem_ptr((char *)p);

			if (copy_to_user(buf, kbuf, sz))
				return -EFAULT;
			buf += sz;
			p += sz;
			read += sz;
			low_count -= sz;
			count -= sz;
		}
	}

	if (count > 0) {
		kbuf = (char *)__get_free_page(GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		while (count > 0) {
			int len = count;

			if (len > PAGE_SIZE)
				len = PAGE_SIZE;
			len = vread(kbuf, (char *)p, len);
			if (!len)
				break;
			if (copy_to_user(buf, kbuf, len)) {
				free_page((unsigned long)kbuf);
				return -EFAULT;
			}
			count -= len;
			buf += len;
			read += len;
			p += len;
		}
		free_page((unsigned long)kbuf);
	}
 	*ppos = p;
 	return read;
}


static inline ssize_t
do_write_kmem(void *p, unsigned long realp, const char __user * buf,
	      size_t count, loff_t *ppos)
{
	ssize_t written, sz;
	unsigned long copied;

	written = 0;
#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
	/* we don't have page 0 mapped on sparc and m68k.. */
	if (realp < PAGE_SIZE) {
		unsigned long sz = PAGE_SIZE - realp;
		if (sz > count)
			sz = count;
		/* Hmm. Do something? */
		buf += sz;
		p += sz;
		realp += sz;
		count -= sz;
		written += sz;
	}
#endif

	while (count > 0) {
		char *ptr;
		/*
		 * Handle first page in case it's not aligned
		 */
		if (-realp & (PAGE_SIZE - 1))
			sz = -realp & (PAGE_SIZE - 1);
		else
			sz = PAGE_SIZE;

		sz = min_t(unsigned long, sz, count);

		/*
		 * On ia64 if a page has been mapped somewhere as
		 * uncached, then it must also be accessed uncached
		 * by the kernel or data corruption may occur
		 */
		ptr = xlate_dev_kmem_ptr(p);

		copied = copy_from_user(ptr, buf, sz);
		if (copied) {
			written += sz - copied;
			if (written)
				break;
			return -EFAULT;
		}
		buf += sz;
		p += sz;
		realp += sz;
		count -= sz;
		written += sz;
	}

	*ppos += written;
	return written;
}


/*
 * This function writes to the *virtual* memory as seen by the kernel.
 */
static ssize_t write_kmem(struct file * file, const char __user * buf, 
			  size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	ssize_t wrote = 0;
	ssize_t virtr = 0;
	ssize_t written;
	char * kbuf; /* k-addr because vwrite() takes vmlist_lock rwlock */

	if (p < (unsigned long) high_memory) {

		wrote = count;
		if (count > (unsigned long) high_memory - p)
			wrote = (unsigned long) high_memory - p;

		written = do_write_kmem((void*)p, p, buf, wrote, ppos);
		if (written != wrote)
			return written;
		wrote = written;
		p += wrote;
		buf += wrote;
		count -= wrote;
	}

	if (count > 0) {
		kbuf = (char *)__get_free_page(GFP_KERNEL);
		if (!kbuf)
			return wrote ? wrote : -ENOMEM;
		while (count > 0) {
			int len = count;

			if (len > PAGE_SIZE)
				len = PAGE_SIZE;
			if (len) {
				written = copy_from_user(kbuf, buf, len);
				if (written) {
					if (wrote + virtr)
						break;
					free_page((unsigned long)kbuf);
					return -EFAULT;
				}
			}
			len = vwrite(kbuf, (char *)p, len);
			count -= len;
			buf += len;
			virtr += len;
			p += len;
		}
		free_page((unsigned long)kbuf);
	}

 	*ppos = p;
 	return virtr + wrote;
}
#endif

#ifdef CONFIG_DEVPORT
static ssize_t read_port(struct file * file, char __user * buf,
			 size_t count, loff_t *ppos)
{
	unsigned long i = *ppos;
	char __user *tmp = buf;

	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT; 
	while (count-- > 0 && i < 65536) {
		if (__put_user(inb(i),tmp) < 0) 
			return -EFAULT;  
		i++;
		tmp++;
	}
	*ppos = i;
	return tmp-buf;
}

static ssize_t write_port(struct file * file, const char __user * buf,
			  size_t count, loff_t *ppos)
{
	unsigned long i = *ppos;
	const char __user * tmp = buf;

	if (!access_ok(VERIFY_READ,buf,count))
		return -EFAULT;
	while (count-- > 0 && i < 65536) {
		char c;
		if (__get_user(c, tmp)) {
			if (tmp > buf)
				break;
			return -EFAULT; 
		}
		outb(c,i);
		i++;
		tmp++;
	}
	*ppos = i;
	return tmp-buf;
}
#endif

static ssize_t read_null(struct file * file, char __user * buf,
			 size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t write_null(struct file * file, const char __user * buf,
			  size_t count, loff_t *ppos)
{
	return count;
}

static int pipe_to_null(struct pipe_inode_info *info, struct pipe_buffer *buf,
			struct splice_desc *sd)
{
	return sd->len;
}

static ssize_t splice_write_null(struct pipe_inode_info *pipe,struct file *out,
				 loff_t *ppos, size_t len, unsigned int flags)
{
	return splice_from_pipe(pipe, out, ppos, len, flags, pipe_to_null);
}

static ssize_t read_zero(struct file * file, char __user * buf, 
			 size_t count, loff_t *ppos)
{
	size_t written;

	if (!count)
		return 0;

	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;

	written = 0;
	while (count) {
		unsigned long unwritten;
		size_t chunk = count;

		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;	/* Just for latency reasons */
		unwritten = clear_user(buf, chunk);
		written += chunk - unwritten;
		if (unwritten)
			break;
		/* Consider changing this to just 'signal_pending()' with lots of testing */
		if (fatal_signal_pending(current))
			return written ? written : -EINTR;
		buf += chunk;
		count -= chunk;
		cond_resched();
	}
	return written ? written : -EFAULT;
}

static int mmap_zero(struct file * file, struct vm_area_struct * vma)
{
#ifndef CONFIG_MMU
	return -ENOSYS;
#endif
	if (vma->vm_flags & VM_SHARED)
		return shmem_zero_setup(vma);
	return 0;
}

static ssize_t write_full(struct file * file, const char __user * buf,
			  size_t count, loff_t *ppos)
{
	return -ENOSPC;
}

/*
 * Special lseek() function for /dev/null and /dev/zero.  Most notably, you
 * can fopen() both devices with "a" now.  This was previously impossible.
 * -- SRB.
 */

static loff_t null_lseek(struct file * file, loff_t offset, int orig)
{
	return file->f_pos = 0;
}

/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static loff_t memory_lseek(struct file * file, loff_t offset, int orig)
{
	loff_t ret;

	mutex_lock(&file->f_path.dentry->d_inode->i_mutex);
	switch (orig) {
		case 0:
			file->f_pos = offset;
			ret = file->f_pos;
			force_successful_syscall_return();
			break;
		case 1:
			file->f_pos += offset;
			ret = file->f_pos;
			force_successful_syscall_return();
			break;
		default:
			ret = -EINVAL;
	}
	mutex_unlock(&file->f_path.dentry->d_inode->i_mutex);
	return ret;
}

static int open_port(struct inode * inode, struct file * filp)
{
	return capable(CAP_SYS_RAWIO) ? 0 : -EPERM;
}

#define zero_lseek	null_lseek
#define full_lseek      null_lseek
#define write_zero	write_null
#define read_full       read_zero
#define open_mem	open_port
#define open_kmem	open_mem
#define open_oldmem	open_mem

static const struct file_operations mem_fops = {
	.llseek		= memory_lseek,
	.read		= read_mem,
	.write		= write_mem,
	.mmap		= mmap_mem,
	.open		= open_mem,
	.get_unmapped_area = get_unmapped_area_mem,
};

#ifdef CONFIG_DEVKMEM
static const struct file_operations kmem_fops = {
	.llseek		= memory_lseek,
	.read		= read_kmem,
	.write		= write_kmem,
	.mmap		= mmap_kmem,
	.open		= open_kmem,
	.get_unmapped_area = get_unmapped_area_mem,
};
#endif

static const struct file_operations null_fops = {
	.llseek		= null_lseek,
	.read		= read_null,
	.write		= write_null,
	.splice_write	= splice_write_null,
};

#ifdef CONFIG_DEVPORT
static const struct file_operations port_fops = {
	.llseek		= memory_lseek,
	.read		= read_port,
	.write		= write_port,
	.open		= open_port,
};
#endif

static const struct file_operations zero_fops = {
	.llseek		= zero_lseek,
	.read		= read_zero,
	.write		= write_zero,
	.mmap		= mmap_zero,
};

/*
 * capabilities for /dev/zero
 * - permits private mappings, "copies" are taken of the source of zeros
 */
static struct backing_dev_info zero_bdi = {
	.capabilities	= BDI_CAP_MAP_COPY,
};

static const struct file_operations full_fops = {
	.llseek		= full_lseek,
	.read		= read_full,
	.write		= write_full,
};

#ifdef CONFIG_CRASH_DUMP
static const struct file_operations oldmem_fops = {
	.read	= read_oldmem,
	.open	= open_oldmem,
};
#endif

static ssize_t kmsg_write(struct file * file, const char __user * buf,
			  size_t count, loff_t *ppos)
{
	char *tmp;
	ssize_t ret;

	tmp = kmalloc(count + 1, GFP_KERNEL);
	if (tmp == NULL)
		return -ENOMEM;
	ret = -EFAULT;
	if (!copy_from_user(tmp, buf, count)) {
		tmp[count] = 0;
		ret = printk("%s", tmp);
		if (ret > count)
			/* printk can add a prefix */
			ret = count;
	}
	kfree(tmp);
	return ret;
}

static const struct file_operations kmsg_fops = {
	.write =	kmsg_write,
};

static int memory_open(struct inode * inode, struct file * filp)
{
	int ret = 0;

	lock_kernel();
	switch (iminor(inode)) {
		case 1:
			filp->f_op = &mem_fops;
			filp->f_mapping->backing_dev_info =
				&directly_mappable_cdev_bdi;
			break;
#ifdef CONFIG_DEVKMEM
		case 2:
			filp->f_op = &kmem_fops;
			filp->f_mapping->backing_dev_info =
				&directly_mappable_cdev_bdi;
			break;
#endif
		case 3:
			filp->f_op = &null_fops;
			break;
#ifdef CONFIG_DEVPORT
		case 4:
			filp->f_op = &port_fops;
			break;
#endif
		case 5:
			filp->f_mapping->backing_dev_info = &zero_bdi;
			filp->f_op = &zero_fops;
			break;
		case 7:
			filp->f_op = &full_fops;
			break;
		case 8:
			filp->f_op = &random_fops;
			break;
		case 9:
			filp->f_op = &urandom_fops;
			break;
		case 11:
			filp->f_op = &kmsg_fops;
			break;
#ifdef CONFIG_CRASH_DUMP
		case 12:
			filp->f_op = &oldmem_fops;
			break;
#endif
		default:
			unlock_kernel();
			return -ENXIO;
	}
	if (filp->f_op && filp->f_op->open)
		ret = filp->f_op->open(inode,filp);
	unlock_kernel();
	return ret;
}

static const struct file_operations memory_fops = {
	.open		= memory_open,	/* just a selector for the real open */
};

static const struct {
	unsigned int		minor;
	char			*name;
	umode_t			mode;
	const struct file_operations	*fops;
} devlist[] = { /* list of minor devices */
	{1, "mem",     S_IRUSR | S_IWUSR | S_IRGRP, &mem_fops},
#ifdef CONFIG_DEVKMEM
	{2, "kmem",    S_IRUSR | S_IWUSR | S_IRGRP, &kmem_fops},
#endif
	{3, "null",    S_IRUGO | S_IWUGO,           &null_fops},
#ifdef CONFIG_DEVPORT
	{4, "port",    S_IRUSR | S_IWUSR | S_IRGRP, &port_fops},
#endif
	{5, "zero",    S_IRUGO | S_IWUGO,           &zero_fops},
	{7, "full",    S_IRUGO | S_IWUGO,           &full_fops},
	{8, "random",  S_IRUGO | S_IWUSR,           &random_fops},
	{9, "urandom", S_IRUGO | S_IWUSR,           &urandom_fops},
	{11,"kmsg",    S_IRUGO | S_IWUSR,           &kmsg_fops},
#ifdef CONFIG_CRASH_DUMP
	{12,"oldmem",    S_IRUSR | S_IWUSR | S_IRGRP, &oldmem_fops},
#endif
};

static struct class *mem_class;

static int __init chr_dev_init(void)
{
	int i;
	int err;

	err = bdi_init(&zero_bdi);
	if (err)
		return err;

	if (register_chrdev(MEM_MAJOR,"mem",&memory_fops))
		printk("unable to get major %d for memory devs\n", MEM_MAJOR);

	mem_class = class_create(THIS_MODULE, "mem");
	for (i = 0; i < ARRAY_SIZE(devlist); i++)
		device_create_drvdata(mem_class, NULL,
				      MKDEV(MEM_MAJOR, devlist[i].minor),
				      NULL, devlist[i].name);

	return 0;
}

fs_initcall(chr_dev_init);
