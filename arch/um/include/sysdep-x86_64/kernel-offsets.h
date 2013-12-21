#include <linux/stddef.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/elf.h>
#include <linux/crypto.h>
#include <asm/page.h>
#include <asm/mman.h>

#define DEFINE(sym, val) \
	asm volatile("\n->" #sym " %0 " #val : : "i" (val))

#define DEFINE_STR1(x) #x
#define DEFINE_STR(sym, val) asm volatile("\n->" #sym " " DEFINE_STR1(val) " " #val: : )

#define BLANK() asm volatile("\n->" : : )

#define OFFSET(sym, str, mem) \
	DEFINE(sym, offsetof(struct str, mem));

void foo(void)
{
	DEFINE(KERNEL_MADV_REMOVE, MADV_REMOVE);
#ifdef CONFIG_MODE_TT
	OFFSET(HOST_TASK_EXTERN_PID, task_struct, thread.mode.tt.extern_pid);
#endif
#include <common-offsets.h>
}
