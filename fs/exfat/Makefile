#
# Makefile for the Linux fat filesystem support.
#

obj-$(CONFIG_EXFAT_FS) += exfat.o

CFLAGS_exfat_core.o += -O2
exfat-y	:= exfat_core.o exfat_super.o exfat_api.o exfat_blkdev.o exfat_cache.o \
			   exfat_data.o exfat_bitmap.o exfat_nls.o exfat_oal.o exfat_upcase.o
