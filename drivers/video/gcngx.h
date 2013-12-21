#ifndef __GCGX__
#define __GCGX__

int gcngx_mmap(struct fb_info *info,struct file *file,
		    struct vm_area_struct *vma);

int gcngx_ioctl(struct inode *inode,struct file *file,
			    unsigned int cmd,unsigned long arg,
			    struct fb_info *info);

int gcngx_init(struct fb_info *info);
void gcngx_exit(struct fb_info *info);

void gcngx_vtrace(void);

#endif
