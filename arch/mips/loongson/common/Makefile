#
# Makefile for loongson based machines.
#

obj-y += setup.o init.o cmdline.o env.o time.o reset.o irq.o \
    pci.o bonito-irq.o mem.o machtype.o

#
# Early printk support
#
obj-$(CONFIG_EARLY_PRINTK) += early_printk.o

# Enable RTC Class support
#
# please enable CONFIG_RTC_DRV_CMOS
#
obj-$(CONFIG_RTC_DRV_CMOS) += rtc.o
