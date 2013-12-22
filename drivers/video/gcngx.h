#ifndef __GCGX__
#define __GCGX__

#ifdef CONFIG_FB_GAMECUBE_GX

int gcngx_mmap(struct fb_info *info, struct vm_area_struct *vma);
int gcngx_ioctl(struct fb_info *info, unsigned int cmd,unsigned long arg);

int gcngx_init(struct fb_info *info);
void gcngx_exit(struct fb_info *info);

struct vi_ctl;
void gcngx_vtrace(struct vi_ctl *ctl);

#endif

#endif
