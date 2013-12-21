/*
 * drivers/video/gcngx.c
 *
 * Nintendo GameCube GX driver extension
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 *
 * Parts borrowed heavily from libogc.  This driver would not have
 * been possible with this library.  Thanks!
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/console.h>
#include <linux/vt_kern.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <asm/pgtable.h>
#include <asm/atomic.h>
#include <platforms/gamecube.h>
#include "gcngx.h"

/* Function definitions */
static inline void  __GX_AckFifoInt(int isOver);
static inline void  __GX_WriteFifoIntEnable(int over,int under);
static void gcngx_munmap(struct vm_area_struct *vma);
static void gcngx_free_munmap(struct vm_area_struct *vma);
static void gcngx_destroy_fifo(void);
static void gcngx_init_fifo(void);
extern int gcnfb_restorefb(struct fb_info *info);
extern void gcnfb_set_framebuffer(u32 addr);

extern struct fb_ops gcnfb_ops;

/* Defines */
#define mtwpar(v) mtspr(921,v)
#define mfwpar(v) mfspr(921)

#define GX_ENABLE 1
#define GX_DISABLE 0
#define GX_TRUE 1
#define GX_FALSE 0
#define _SHIFTL(v, s, w)	\
    ((u32) (((u32)(v) & ((0x01 << (w)) - 1)) << (s)))
#define _SHIFTR(v, s, w)	\
    ((u32)(((u32)(v) >> (s)) & ((0x01 << (w)) - 1)))

#define IRQ_VIDEO 8
#define IRQ_PE_TOKEN 9
#define IRQ_PE_FINISH 10
#define IRQ_CP_FIFO   11

#define VIDEO_MMAP_BASE    0x0C000000
#define VIDEO_MMAP_LENGTH  0x9000

#define KMALLOC_BASE 0x0D000000

#define VIDEO_PE_INTERRUPT		((void __iomem *)0xcc00100a)
#define VIDEO_PE_TOKEN			((void __iomem *)0xcc00100e)
#define VIDEO_PE_INTERRUPT_TOKEN_ENABLE     (1 << 0)
#define VIDEO_PE_INTERRUPT_FINISH_ENABLE    (1 << 1)
#define VIDEO_PE_INTERRUPT_TOKEN_INTERRUPT  (1 << 2)
#define VIDEO_PE_INTERRUPT_FINISH_INTERRUPT (1 << 3)

#define	gcngx_disable_pe_interrupts() writew(readw(VIDEO_PE_INTERRUPT) & ~(VIDEO_PE_INTERRUPT_TOKEN_ENABLE | VIDEO_PE_INTERRUPT_FINISH_ENABLE),VIDEO_PE_INTERRUPT)
#define gcngx_enable_pe_interrupts() { writew(readw(VIDEO_PE_INTERRUPT) | (VIDEO_PE_INTERRUPT_TOKEN_ENABLE | VIDEO_PE_INTERRUPT_FINISH_ENABLE | VIDEO_PE_INTERRUPT_TOKEN_INTERRUPT | VIDEO_PE_INTERRUPT_FINISH_INTERRUPT),VIDEO_PE_INTERRUPT); writew(0,VIDEO_PE_TOKEN); }

#define VIDEO_CP_SR		((volatile u16 __iomem *)0xcc000000)
#define VIDEO_CP_SR_OVERFLOW	(1 << 0)
#define VIDEO_CP_SR_UNDERFLOW	(1 << 1)

#define VIDEO_CP_CR		((volatile u16 __iomem *)0xcc000002)
#define VIDEO_CP_CR_GP_FIFO_READ_ENABLE  (1 << 0)
#define VIDEO_CP_CR_CP_IRQ_ENABLE        (1 << 1)
#define VIDEO_CP_CR_OVERFLOW_IRQ_ENABLE  (1 << 2)
#define VIDEO_CP_CR_UNDERFLOW_IRQ_ENABLE (1 << 3)
#define VIDEO_CP_CR_GP_LINK_ENABLE       (1 << 4)
#define VIDEO_CP_CR_MASK                 (0x1F)

#define SIG_PE_FINISH       (SIGRTMIN+14)
#define SIG_PE_TOKEN        (SIGRTMIN+15)
#define SIG_VTRACE_COMPLETE (SIGRTMIN+16)

#define FIFO_PUTU8(x)  (*((volatile u8*) WGPIPE) = (x))
#define FIFO_PUTU32(x) (*((volatile u32*)WGPIPE) = (x))

#define LOAD_BP_REG(x) do { FIFO_PUTU8(0x61); FIFO_PUTU32(x); } while (0)

/* Static data */
static task_t *mmap_task;
static int overflow;
static u32 xfb[2];
static int currentFB = 0;
static int flipRequest = 0;
static u8 *mmap_fifo_base;
static u8 *phys_fifo_base;
static const u32 fifo_len       = GCN_GX_FIFO_SIZE;
static struct vm_operations_struct gcngx_vm_ops =
{
	.close = gcngx_munmap,
};
static struct vm_operations_struct gcngx_vm_free_ops = 
{
	.close = gcngx_free_munmap,
};

static volatile u32* const _piReg = (volatile u32*)0xCC003000;
static volatile u16* const _cpReg = (volatile u16*)0xCC000000;
static volatile u16* const _peReg = (volatile u16*)0xCC001000;
static volatile u16* const _memReg = (volatile u16*)0xCC004000;
static volatile u32* const WGPIPE  = (volatile u32*)0xCC008000;

static irqreturn_t gcfb_fifo_irq_handler(int irq,void *dev_id,struct pt_regs *regs)
{
	/* now handle the int */
	u16 val = readw(VIDEO_CP_SR);
	
	/* ENABLE_RUMBLE(); */
	
	if (val & VIDEO_CP_SR_OVERFLOW)
	{
		/* fifo overflow, must halt the current application */
		if (mmap_task)
		{
			printk(KERN_INFO "Man you are writing too fast!  Slow down!  I will make you!\n");
			set_task_state(mmap_task,TASK_UNINTERRUPTIBLE);
			overflow = 1;
		}
		__GX_AckFifoInt(1);
		__GX_WriteFifoIntEnable(GX_DISABLE,GX_ENABLE);
		return IRQ_HANDLED;
	}
	else if (val & VIDEO_CP_SR_UNDERFLOW)
	{
		/* underflow, resume the current application */
		if (mmap_task && overflow)
		{
			printk(KERN_INFO "OK dude, the GX has crunched the data, you can resume now\n");
			set_task_state(mmap_task,TASK_RUNNING);
			overflow = 0;
		}
		__GX_AckFifoInt(0);
		__GX_WriteFifoIntEnable(GX_ENABLE,GX_DISABLE);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

void gcngx_vtrace(void)
{
	struct siginfo sig;
	/* ok flip the image if we have a flip request.
	   send signal on completion */
	if (mmap_task && flipRequest)
	{
		/* do the flip! */
		flipRequest = 0;
		/* setup the signal info and flip buffer pointers */
		currentFB = currentFB ? 0 : 1;
		sig.si_errno  = xfb[currentFB];
		/* inform the hardware */
		gcnfb_set_framebuffer(xfb[currentFB]);
		/* notify the process */
		sig.si_signo = SIG_VTRACE_COMPLETE;
		sig.si_code  = 0;
		send_sig_info(SIG_VTRACE_COMPLETE,&sig,mmap_task);
	}
}

static irqreturn_t gcfb_pe_finish_irq_handler(int irq,void *dev_id,struct pt_regs *regs)
{
	u16 val;
	struct siginfo sig;
	/* ack the interrupt */
	val = readw(VIDEO_PE_INTERRUPT) | VIDEO_PE_INTERRUPT_FINISH_INTERRUPT;
	writew(val,VIDEO_PE_INTERRUPT);
	
	/* send SIG_PE_FINISH to the process */
	if (mmap_task)
	{
		sig.si_signo = SIG_PE_FINISH;
		sig.si_errno = 0;
		sig.si_code  = 0;
		send_sig_info(SIG_PE_FINISH,&sig,mmap_task);
	}
	return IRQ_HANDLED;
}

static irqreturn_t gcfb_pe_token_irq_handler(int irq,void *dev_id,struct pt_regs *regs)
{
	u16 val;
	struct siginfo sig;
	/* ack the interrupt */
	val = readw(VIDEO_PE_INTERRUPT) | VIDEO_PE_INTERRUPT_TOKEN_INTERRUPT;
	writew(val,VIDEO_PE_INTERRUPT);
	/* send SIG_PE_TOKEN to the process */
	if (mmap_task)
	{
		sig.si_signo = SIG_PE_TOKEN;
		sig.si_errno = 0;
		sig.si_code  = _peReg[7];
		send_sig_info(SIG_PE_TOKEN,&sig,mmap_task);
	}
	return IRQ_HANDLED;
}

int gcngx_ioctl(struct inode *inode,struct file *file,
		unsigned int cmd,unsigned long arg,
		struct fb_info *info)
{
	if (cmd == FBIOFLIP)
	{
		flipRequest = 1;
		return 0;
	}
	return -EINVAL;
}

static void *mymalloc(unsigned int len)
{
	struct page *page;
	void *p = kmalloc(len,GFP_KERNEL);
	if (p && len)
	{
		/* reserve all the memory so remap_page_range works */
		for (page=virt_to_page(p);page<virt_to_page(p+len);++page) {
			SetPageReserved(page);
			SetPageLocked(page);
		}
	}
	return p;	
}

static void myfree(void *p)
{
	struct page *page;
	u32 len;
	if (p)
	{
		len = ksize(p);
		for (page=virt_to_page(p);page<virt_to_page(p+len);++page) {
			ClearPageReserved(page);
			ClearPageLocked(page);
		}
		kfree(p);
	}
}

static void gcngx_free_munmap(struct vm_area_struct *vma)
{
	if (vma->vm_private_data)
	{
		myfree(vma->vm_private_data);
		vma->vm_private_data = NULL;
	}
}

static void gcngx_munmap(struct vm_area_struct *vma)
{
	struct fb_info *info = (struct fb_info*)vma->vm_private_data;
	struct vc_data *vc;
	
	gcngx_destroy_fifo();
	
	/* nobody has up mapped anymore */
	mmap_task = NULL;
	overflow = 0;
	
	/* restore the framebuffer */
	gcnfb_restorefb(info);
#ifdef CONFIG_FRAMEBUFFER_CONSOLE
	acquire_console_sem();
	vc = vc_cons[fg_console].d;
	update_screen(vc);
	unblank_screen();
	release_console_sem();
#endif
}

int gcngx_mmap(struct fb_info *info,struct file *file,
	       struct vm_area_struct *vma)
{
	int ret;
	static spinlock_t lock = SPIN_LOCK_UNLOCKED;
	u32 phys;
	u32 len;

	len = vma->vm_end - vma->vm_start;
	
	if (vma->vm_pgoff == (VIDEO_MMAP_BASE >> PAGE_SHIFT) &&
	    len == VIDEO_MMAP_LENGTH)
	{
		/* our special case, map the memory info */
		vma->vm_flags |= VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		if (io_remap_pfn_range(vma,vma->vm_start,
					VIDEO_MMAP_BASE >> PAGE_SHIFT,
					len,
					vma->vm_page_prot))
		{
			return -EINVAL;
		}
		vma->vm_ops   = &gcngx_vm_ops;
		vma->vm_private_data = info;
		/* store task for when fifo is overflown */
		mmap_task = current;
		overflow = 0;
		/* init the fifo before we return */
		gcngx_init_fifo();
		return 0;
	}
	else if (vma->vm_pgoff >= (KMALLOC_BASE >> PAGE_SHIFT))
	{
		/* ok kmalloc the memory now */
		vma->vm_private_data = mymalloc(len);
		if (!vma->vm_private_data)
		{
			return -ENOMEM;
		}
		/* now setup the mapping */
		phys = virt_to_phys(vma->vm_private_data);
		vma->vm_flags |= (VM_RESERVED | VM_LOCKED);
		if (remap_pfn_range(vma,vma->vm_start,
				     phys >> PAGE_SHIFT,len,vma->vm_page_prot))
		{
			kfree(vma->vm_private_data);
			return -EINVAL;
		}
		vma->vm_ops = &gcngx_vm_free_ops;
		/* now write the physical mapping in the first u32 */
		*((u32*)vma->vm_private_data) = phys;
		/* return successful */
		return 0;
	}

	/* call the frame buffer mmap method */
	if (file->f_op->mmap)
	{
		spin_lock(&lock);
		/* reset our mmap since the fb driver will call it */
		gcnfb_ops.fb_mmap = NULL;
		ret = file->f_op->mmap(file,vma);
		/* reset our mmap */
		gcnfb_ops.fb_mmap = gcngx_mmap;
		spin_unlock(&lock);
		return ret;
	}
	return -EINVAL;
}

static inline void  __GX_AckFifoInt(int isOver)
{
	if (isOver)
		_cpReg[2] |= (1 << 0);
	else
		_cpReg[2] |= (1 << 1);
}

static inline void  __GX_Flush(void)
{
	/* write 8 32 bit values to the WGPIPE */
	*WGPIPE = 0;
	*WGPIPE = 0;
	*WGPIPE = 0;
	*WGPIPE = 0;
	*WGPIPE = 0;
	*WGPIPE = 0;
	*WGPIPE = 0;
	*WGPIPE = 0;
}

static inline void  __GX_WriteFifoIntEnable(int over, int under)
{
	u16 val = _cpReg[1] & ~(VIDEO_CP_CR_GP_FIFO_READ_ENABLE | 
				VIDEO_CP_CR_CP_IRQ_ENABLE |
				VIDEO_CP_CR_GP_LINK_ENABLE);
	
	if (over)  val |= VIDEO_CP_CR_OVERFLOW_IRQ_ENABLE;
	if (under) val |= VIDEO_CP_CR_UNDERFLOW_IRQ_ENABLE;
	
	_cpReg[1] = val;
	/* ack it just for fun */
	_cpReg[2] = 0x3;
}

static inline void  __GX_FifoReadEnable(int enable)
{
	if (enable) 
		_cpReg[1] |= VIDEO_CP_CR_GP_FIFO_READ_ENABLE;
	else
		_cpReg[1] &= ~VIDEO_CP_CR_GP_FIFO_READ_ENABLE;
}

static inline void __GX_FifoLink(u8 enable)
{
	if (enable)
		_cpReg[1] |= VIDEO_CP_CR_GP_LINK_ENABLE;
	else
		_cpReg[1] &= ~VIDEO_CP_CR_GP_LINK_ENABLE;
}

static void __GX_EnableWriteGatherPipe(u8 enable)
{
	u32 flags;
	if (enable)
	{
		mtwpar(0x0C008000);
	}

	asm ("isync");
	asm ("sync");

	flags = mfspr(920);
	if (enable)
	{
		flags |= 0x40000000;
	}
	else
	{
		flags &= ~0x40000000;
	}
	
	mtspr(920,flags);
	asm ("isync");
	asm ("sync");
}

static inline void __GX_DrawDone(void)
{
	LOAD_BP_REG(0x45000002);
	__GX_Flush();
}

static void gcngx_destroy_fifo()
{
	gcngx_disable_pe_interrupts();
	_peReg[7] = 0;

	__GX_DrawDone();
	/* wait for the buffer to empty? */
	__GX_WriteFifoIntEnable(GX_DISABLE,GX_DISABLE);
	__GX_FifoReadEnable(0);
	__GX_FifoLink(GX_FALSE);

	__GX_EnableWriteGatherPipe(0);
}

struct fifo_info
{
	u8 *base;
	u8 *end;
	u32 length;
	u8 *lo_water_mark;
	u8 *hi_water_mark;
	u8 *write_ptr;
	u8 *read_ptr;
};

static void gcngx_init_fifo(void)
{
	struct fifo_info fi;
	int i;
	
	fi.base = phys_fifo_base;
	fi.end  = phys_fifo_base + fifo_len - 4;
	fi.length = fifo_len;
	fi.lo_water_mark = phys_fifo_base + ((fifo_len / 2) & ~31);
	fi.hi_water_mark = phys_fifo_base + fifo_len - (16*1024);
	fi.write_ptr = phys_fifo_base;
	fi.read_ptr  = phys_fifo_base;
	
	/* reset currentFB pointer */
	currentFB = 0;
	flipRequest = 0;

	/* printk(KERN_INFO "Initializing Flipper FIFO at %p of length %u\n",
	   fi.base,fifo_len); */
	
	__GX_FifoLink(GX_FALSE);
	__GX_WriteFifoIntEnable(GX_DISABLE,GX_DISABLE);
	__GX_FifoReadEnable(0);

	/* clear the fifo */
	for (i=0;i<fifo_len/4;++i)
	{
		((u32*)mmap_fifo_base)[i] = 0;
	}
	/* flush it */
	flush_dcache_range((u32)mmap_fifo_base,
			   (u32)(mmap_fifo_base+fifo_len));
	
	_peReg[7] = 0;
	
	/* fifo base start */
	_piReg[3] = (u32)fi.base;
	/* fifo base end */
	_piReg[4] = (u32)fi.end;
	/* fifo write pointer */
	_piReg[5] = (u32)fi.write_ptr;

	/* init and flush the write gather pipe */
	__GX_EnableWriteGatherPipe(1);
	__GX_Flush();

	/* wait for all data to be flushed */
	while (mfwpar() & 1);
	_piReg[3] = (u32)fi.base;
	_piReg[4] = (u32)fi.end;
	_piReg[5] = (u32)fi.write_ptr;
	while (mfwpar() & 1);

	/* setup fifo base */
	_cpReg[16] = _SHIFTL(fi.base,0,16);
	_cpReg[17] = _SHIFTR(fi.base,16,16);
	
	/* setup fifo end */
	_cpReg[18] = _SHIFTL(fi.end,0,16);
	_cpReg[19] = _SHIFTR(fi.end,16,16);
	
	/* setup hiwater mark */
	_cpReg[20] = _SHIFTL(fi.hi_water_mark,0,16);
	_cpReg[21] = _SHIFTR(fi.hi_water_mark,16,16);
	
	/* setup lowater mark */
	_cpReg[22] = _SHIFTL(fi.lo_water_mark,0,16);
	_cpReg[23] = _SHIFTR(fi.lo_water_mark,16,16);
	
	/* setup rd<->wd dist */
	/*_cpReg[24] = _SHIFTL((pad)[7],0,16);
	  _cpReg[25] = _SHIFTR((pad)[7],16,16);*/
	_cpReg[24] = 0;
	_cpReg[25] = 0;
	
	/* setup wt ptr */
	_cpReg[26] = _SHIFTL(fi.write_ptr,0,16);
	_cpReg[27] = _SHIFTR(fi.write_ptr,16,16);
	
	/* setup rd ptr */
	_cpReg[28] = _SHIFTL(fi.read_ptr,0,16);
	_cpReg[29] = _SHIFTR(fi.read_ptr,16,16);

	asm ("sync");
	asm ("isync");
	/* enable the write gather pipe */
	__GX_WriteFifoIntEnable(GX_ENABLE,GX_DISABLE);
	__GX_FifoLink(GX_TRUE);
	__GX_FifoReadEnable(1);
	/* enable interrupts */
	gcngx_enable_pe_interrupts();

	asm("sync");
	asm("isync");
}

int gcngx_init(struct fb_info *info)
{
	int err;
	/* compute framebuffer pointers */
	xfb[0] = (u32)info->fix.smem_start;
	xfb[1] = (u32)info->fix.smem_start + info->fix.smem_len/2;
	/* disable the interrupts */
	gcngx_disable_pe_interrupts();
	__GX_WriteFifoIntEnable(GX_DISABLE,GX_DISABLE);

	/* map the fifo area */
	phys_fifo_base = (u8*)GCN_GX_FIFO_START;
	if (!request_mem_region((u32)phys_fifo_base,fifo_len,"GX FIFO")) {
		printk(KERN_ERR "Cannot reserve fifo memory area at %p\n",phys_fifo_base);
		return -EIO;
	}
	if (!(mmap_fifo_base = ioremap((u32)phys_fifo_base,fifo_len))) {
		printk(KERN_ERR "Cannot map the fifo area at %p\n",phys_fifo_base);
		err = -EIO;
		goto free_mem;
	}
	
	if ((err=request_irq(IRQ_PE_TOKEN,gcfb_pe_token_irq_handler,SA_INTERRUPT,"PE Token",0)))
	{
		goto free_iounmap;
	}
	if ((err=request_irq(IRQ_PE_FINISH,gcfb_pe_finish_irq_handler,SA_INTERRUPT,"PE Finish",0)))
	{
		goto free_pe_token;
	}
	if ((err=request_irq(IRQ_CP_FIFO,gcfb_fifo_irq_handler,SA_INTERRUPT,"CP FIFO",0)))
	{
		goto free_pe_finish;
	}
	return 0;

 free_pe_finish:
	free_irq(IRQ_PE_FINISH,0);
 free_pe_token:
	free_irq(IRQ_PE_TOKEN,0);
 free_iounmap:
	iounmap(mmap_fifo_base);
 free_mem:
	release_mem_region((u32)phys_fifo_base,fifo_len);
	
	return err;
}

void gcngx_exit(struct fb_info *info)
{
	gcngx_destroy_fifo();
	
	free_irq(IRQ_PE_FINISH,0);
	free_irq(IRQ_PE_TOKEN,0);
	free_irq(IRQ_CP_FIFO,0);

	iounmap(mmap_fifo_base);
	release_mem_region((u32)phys_fifo_base,fifo_len);
}
