/*
 *  ec.c - ACPI Embedded Controller Driver (v2.1)
 *
 *  Copyright (C) 2006-2008 Alexey Starikovskiy <astarikovskiy@suse.de>
 *  Copyright (C) 2006 Denis Sadykov <denis.m.sadykov@intel.com>
 *  Copyright (C) 2004 Luming Yu <luming.yu@intel.com>
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

/* Uncomment next line to get verbose printout */
/* #define DEBUG */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <asm/io.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/actypes.h>

#define ACPI_EC_CLASS			"embedded_controller"
#define ACPI_EC_DEVICE_NAME		"Embedded Controller"
#define ACPI_EC_FILE_INFO		"info"

#undef PREFIX
#define PREFIX				"ACPI: EC: "

/* EC status register */
#define ACPI_EC_FLAG_OBF	0x01	/* Output buffer full */
#define ACPI_EC_FLAG_IBF	0x02	/* Input buffer full */
#define ACPI_EC_FLAG_BURST	0x10	/* burst mode */
#define ACPI_EC_FLAG_SCI	0x20	/* EC-SCI occurred */

/* EC commands */
enum ec_command {
	ACPI_EC_COMMAND_READ = 0x80,
	ACPI_EC_COMMAND_WRITE = 0x81,
	ACPI_EC_BURST_ENABLE = 0x82,
	ACPI_EC_BURST_DISABLE = 0x83,
	ACPI_EC_COMMAND_QUERY = 0x84,
};

#define ACPI_EC_DELAY		500	/* Wait 500ms max. during EC ops */
#define ACPI_EC_UDELAY_GLK	1000	/* Wait 1ms max. to get global lock */
#define ACPI_EC_UDELAY		100	/* Wait 100us before polling EC again */

#define ACPI_EC_STORM_THRESHOLD 8	/* number of false interrupts
					   per one transaction */

enum {
	EC_FLAGS_QUERY_PENDING,		/* Query is pending */
	EC_FLAGS_GPE_MODE,		/* Expect GPE to be sent
					 * for status change */
	EC_FLAGS_NO_GPE,		/* Don't use GPE mode */
	EC_FLAGS_GPE_STORM,		/* GPE storm detected */
	EC_FLAGS_HANDLERS_INSTALLED	/* Handlers for GPE and
					 * OpReg are installed */
};

/* If we find an EC via the ECDT, we need to keep a ptr to its context */
/* External interfaces use first EC only, so remember */
typedef int (*acpi_ec_query_func) (void *data);

struct acpi_ec_query_handler {
	struct list_head node;
	acpi_ec_query_func func;
	acpi_handle handle;
	void *data;
	u8 query_bit;
};

struct transaction {
	const u8 *wdata;
	u8 *rdata;
	unsigned short irq_count;
	u8 command;
	u8 wi;
	u8 ri;
	u8 wlen;
	u8 rlen;
	bool done;
};

static struct acpi_ec {
	acpi_handle handle;
	unsigned long gpe;
	unsigned long command_addr;
	unsigned long data_addr;
	unsigned long global_lock;
	unsigned long flags;
	struct mutex lock;
	wait_queue_head_t wait;
	struct list_head list;
	struct transaction *curr;
	spinlock_t curr_lock;
} *boot_ec, *first_ec;

/* 
 * Some Asus system have exchanged ECDT data/command IO addresses.
 */
static int print_ecdt_error(const struct dmi_system_id *id)
{
	printk(KERN_NOTICE PREFIX "%s detected - "
		"ECDT has exchanged control/data I/O address\n",
		id->ident);
	return 0;
}

static struct dmi_system_id __cpuinitdata ec_dmi_table[] = {
	{
	print_ecdt_error, "Asus L4R", {
	DMI_MATCH(DMI_BIOS_VERSION, "1008.006"),
	DMI_MATCH(DMI_PRODUCT_NAME, "L4R"),
	DMI_MATCH(DMI_BOARD_NAME, "L4R") }, NULL},
	{
	print_ecdt_error, "Asus M6R", {
	DMI_MATCH(DMI_BIOS_VERSION, "0207"),
	DMI_MATCH(DMI_PRODUCT_NAME, "M6R"),
	DMI_MATCH(DMI_BOARD_NAME, "M6R") }, NULL},
	{},
};

/* --------------------------------------------------------------------------
                             Transaction Management
   -------------------------------------------------------------------------- */

static inline u8 acpi_ec_read_status(struct acpi_ec *ec)
{
	u8 x = inb(ec->command_addr);
	pr_debug(PREFIX "---> status = 0x%2.2x\n", x);
	return x;
}

static inline u8 acpi_ec_read_data(struct acpi_ec *ec)
{
	u8 x = inb(ec->data_addr);
	pr_debug(PREFIX "---> data = 0x%2.2x\n", x);
	return x;
}

static inline void acpi_ec_write_cmd(struct acpi_ec *ec, u8 command)
{
	pr_debug(PREFIX "<--- command = 0x%2.2x\n", command);
	outb(command, ec->command_addr);
}

static inline void acpi_ec_write_data(struct acpi_ec *ec, u8 data)
{
	pr_debug(PREFIX "<--- data = 0x%2.2x\n", data);
	outb(data, ec->data_addr);
}

static int ec_transaction_done(struct acpi_ec *ec)
{
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&ec->curr_lock, flags);
	if (!ec->curr || ec->curr->done)
		ret = 1;
	spin_unlock_irqrestore(&ec->curr_lock, flags);
	return ret;
}

static void start_transaction(struct acpi_ec *ec)
{
	ec->curr->irq_count = ec->curr->wi = ec->curr->ri = 0;
	ec->curr->done = false;
	acpi_ec_write_cmd(ec, ec->curr->command);
}

static void gpe_transaction(struct acpi_ec *ec, u8 status)
{
	unsigned long flags;
	spin_lock_irqsave(&ec->curr_lock, flags);
	if (!ec->curr)
		goto unlock;
	if (ec->curr->wlen > ec->curr->wi) {
		if ((status & ACPI_EC_FLAG_IBF) == 0)
			acpi_ec_write_data(ec,
				ec->curr->wdata[ec->curr->wi++]);
		else
			goto err;
	} else if (ec->curr->rlen > ec->curr->ri) {
		if ((status & ACPI_EC_FLAG_OBF) == 1) {
			ec->curr->rdata[ec->curr->ri++] = acpi_ec_read_data(ec);
			if (ec->curr->rlen == ec->curr->ri)
				ec->curr->done = true;
		} else
			goto err;
	} else if (ec->curr->wlen == ec->curr->wi &&
		   (status & ACPI_EC_FLAG_IBF) == 0)
		ec->curr->done = true;
	goto unlock;
err:
	/* false interrupt, state didn't change */
	if (in_interrupt())
		++ec->curr->irq_count;
unlock:
	spin_unlock_irqrestore(&ec->curr_lock, flags);
}

static int acpi_ec_wait(struct acpi_ec *ec)
{
	if (wait_event_timeout(ec->wait, ec_transaction_done(ec),
			       msecs_to_jiffies(ACPI_EC_DELAY)))
		return 0;
	/* try restart command if we get any false interrupts */
	if (ec->curr->irq_count &&
	    (acpi_ec_read_status(ec) & ACPI_EC_FLAG_IBF) == 0) {
		pr_debug(PREFIX "controller reset, restart transaction\n");
		start_transaction(ec);
		if (wait_event_timeout(ec->wait, ec_transaction_done(ec),
					msecs_to_jiffies(ACPI_EC_DELAY)))
			return 0;
	}
	/* missing GPEs, switch back to poll mode */
	if (printk_ratelimit())
		pr_info(PREFIX "missing confirmations, "
				"switch off interrupt mode.\n");
	set_bit(EC_FLAGS_NO_GPE, &ec->flags);
	clear_bit(EC_FLAGS_GPE_MODE, &ec->flags);
	return 1;
}

static void acpi_ec_gpe_query(void *ec_cxt);

static int ec_check_sci(struct acpi_ec *ec, u8 state)
{
	if (state & ACPI_EC_FLAG_SCI) {
		if (!test_and_set_bit(EC_FLAGS_QUERY_PENDING, &ec->flags))
			return acpi_os_execute(OSL_EC_BURST_HANDLER,
				acpi_ec_gpe_query, ec);
	}
	return 0;
}

static int ec_poll(struct acpi_ec *ec)
{
	unsigned long delay = jiffies + msecs_to_jiffies(ACPI_EC_DELAY);
	udelay(ACPI_EC_UDELAY);
	while (time_before(jiffies, delay)) {
		gpe_transaction(ec, acpi_ec_read_status(ec));
		udelay(ACPI_EC_UDELAY);
		if (ec_transaction_done(ec))
			return 0;
	}
	return -ETIME;
}

static int acpi_ec_transaction_unlocked(struct acpi_ec *ec,
					struct transaction *t,
					int force_poll)
{
	unsigned long tmp;
	int ret = 0;
	pr_debug(PREFIX "transaction start\n");
	/* disable GPE during transaction if storm is detected */
	if (test_bit(EC_FLAGS_GPE_STORM, &ec->flags)) {
		clear_bit(EC_FLAGS_GPE_MODE, &ec->flags);
		acpi_disable_gpe(NULL, ec->gpe);
	}
	/* start transaction */
	spin_lock_irqsave(&ec->curr_lock, tmp);
	/* following two actions should be kept atomic */
	ec->curr = t;
	start_transaction(ec);
	if (ec->curr->command == ACPI_EC_COMMAND_QUERY)
		clear_bit(EC_FLAGS_QUERY_PENDING, &ec->flags);
	spin_unlock_irqrestore(&ec->curr_lock, tmp);
	/* if we selected poll mode or failed in GPE-mode do a poll loop */
	if (force_poll ||
	    !test_bit(EC_FLAGS_GPE_MODE, &ec->flags) ||
	    acpi_ec_wait(ec))
		ret = ec_poll(ec);
	pr_debug(PREFIX "transaction end\n");
	spin_lock_irqsave(&ec->curr_lock, tmp);
	ec->curr = NULL;
	spin_unlock_irqrestore(&ec->curr_lock, tmp);
	if (test_bit(EC_FLAGS_GPE_STORM, &ec->flags)) {
		/* check if we received SCI during transaction */
		ec_check_sci(ec, acpi_ec_read_status(ec));
		/* it is safe to enable GPE outside of transaction */
		acpi_enable_gpe(NULL, ec->gpe);
	} else if (test_bit(EC_FLAGS_GPE_MODE, &ec->flags) &&
		   t->irq_count > ACPI_EC_STORM_THRESHOLD) {
		pr_info(PREFIX "GPE storm detected, "
			"transactions will use polling mode\n");
		set_bit(EC_FLAGS_GPE_STORM, &ec->flags);
	}
	return ret;
}

static int ec_check_ibf0(struct acpi_ec *ec)
{
	u8 status = acpi_ec_read_status(ec);
	return (status & ACPI_EC_FLAG_IBF) == 0;
}

static int ec_wait_ibf0(struct acpi_ec *ec)
{
	unsigned long delay = jiffies + msecs_to_jiffies(ACPI_EC_DELAY);
	/* interrupt wait manually if GPE mode is not active */
	unsigned long timeout = test_bit(EC_FLAGS_GPE_MODE, &ec->flags) ?
		msecs_to_jiffies(ACPI_EC_DELAY) : msecs_to_jiffies(1);
	while (time_before(jiffies, delay))
		if (wait_event_timeout(ec->wait, ec_check_ibf0(ec), timeout))
			return 0;
	return -ETIME;
}

static int acpi_ec_transaction(struct acpi_ec *ec, struct transaction *t,
			       int force_poll)
{
	int status;
	u32 glk;
	if (!ec || (!t) || (t->wlen && !t->wdata) || (t->rlen && !t->rdata))
		return -EINVAL;
	if (t->rdata)
		memset(t->rdata, 0, t->rlen);
	mutex_lock(&ec->lock);
	if (ec->global_lock) {
		status = acpi_acquire_global_lock(ACPI_EC_UDELAY_GLK, &glk);
		if (ACPI_FAILURE(status)) {
			status = -ENODEV;
			goto unlock;
		}
	}
	if (ec_wait_ibf0(ec)) {
		pr_err(PREFIX "input buffer is not empty, "
				"aborting transaction\n");
		status = -ETIME;
		goto end;
	}
	status = acpi_ec_transaction_unlocked(ec, t, force_poll);
end:
	if (ec->global_lock)
		acpi_release_global_lock(glk);
unlock:
	mutex_unlock(&ec->lock);
	return status;
}

/*
 * Note: samsung nv5000 doesn't work with ec burst mode.
 * http://bugzilla.kernel.org/show_bug.cgi?id=4980
 */
int acpi_ec_burst_enable(struct acpi_ec *ec)
{
	u8 d;
	struct transaction t = {.command = ACPI_EC_BURST_ENABLE,
				.wdata = NULL, .rdata = &d,
				.wlen = 0, .rlen = 1};

	return acpi_ec_transaction(ec, &t, 0);
}

int acpi_ec_burst_disable(struct acpi_ec *ec)
{
	struct transaction t = {.command = ACPI_EC_BURST_DISABLE,
				.wdata = NULL, .rdata = NULL,
				.wlen = 0, .rlen = 0};

	return (acpi_ec_read_status(ec) & ACPI_EC_FLAG_BURST) ?
				acpi_ec_transaction(ec, &t, 0) : 0;
}

static int acpi_ec_read(struct acpi_ec *ec, u8 address, u8 * data)
{
	int result;
	u8 d;
	struct transaction t = {.command = ACPI_EC_COMMAND_READ,
				.wdata = &address, .rdata = &d,
				.wlen = 1, .rlen = 1};

	result = acpi_ec_transaction(ec, &t, 0);
	*data = d;
	return result;
}

static int acpi_ec_write(struct acpi_ec *ec, u8 address, u8 data)
{
	u8 wdata[2] = { address, data };
	struct transaction t = {.command = ACPI_EC_COMMAND_WRITE,
				.wdata = wdata, .rdata = NULL,
				.wlen = 2, .rlen = 0};

	return acpi_ec_transaction(ec, &t, 0);
}

/*
 * Externally callable EC access functions. For now, assume 1 EC only
 */
int ec_burst_enable(void)
{
	if (!first_ec)
		return -ENODEV;
	return acpi_ec_burst_enable(first_ec);
}

EXPORT_SYMBOL(ec_burst_enable);

int ec_burst_disable(void)
{
	if (!first_ec)
		return -ENODEV;
	return acpi_ec_burst_disable(first_ec);
}

EXPORT_SYMBOL(ec_burst_disable);

int ec_read(u8 addr, u8 * val)
{
	int err;
	u8 temp_data;

	if (!first_ec)
		return -ENODEV;

	err = acpi_ec_read(first_ec, addr, &temp_data);

	if (!err) {
		*val = temp_data;
		return 0;
	} else
		return err;
}

EXPORT_SYMBOL(ec_read);

int ec_write(u8 addr, u8 val)
{
	int err;

	if (!first_ec)
		return -ENODEV;

	err = acpi_ec_write(first_ec, addr, val);

	return err;
}

EXPORT_SYMBOL(ec_write);

int ec_transaction(u8 command,
		   const u8 * wdata, unsigned wdata_len,
		   u8 * rdata, unsigned rdata_len,
		   int force_poll)
{
	struct transaction t = {.command = command,
				.wdata = wdata, .rdata = rdata,
				.wlen = wdata_len, .rlen = rdata_len};
	if (!first_ec)
		return -ENODEV;

	return acpi_ec_transaction(first_ec, &t, force_poll);
}

EXPORT_SYMBOL(ec_transaction);

static int acpi_ec_query(struct acpi_ec *ec, u8 * data)
{
	int result;
	u8 d;
	struct transaction t = {.command = ACPI_EC_COMMAND_QUERY,
				.wdata = NULL, .rdata = &d,
				.wlen = 0, .rlen = 1};
	if (!ec || !data)
		return -EINVAL;

	/*
	 * Query the EC to find out which _Qxx method we need to evaluate.
	 * Note that successful completion of the query causes the ACPI_EC_SCI
	 * bit to be cleared (and thus clearing the interrupt source).
	 */

	result = acpi_ec_transaction(ec, &t, 0);
	if (result)
		return result;

	if (!d)
		return -ENODATA;

	*data = d;
	return 0;
}

/* --------------------------------------------------------------------------
                                Event Management
   -------------------------------------------------------------------------- */
int acpi_ec_add_query_handler(struct acpi_ec *ec, u8 query_bit,
			      acpi_handle handle, acpi_ec_query_func func,
			      void *data)
{
	struct acpi_ec_query_handler *handler =
	    kzalloc(sizeof(struct acpi_ec_query_handler), GFP_KERNEL);
	if (!handler)
		return -ENOMEM;

	handler->query_bit = query_bit;
	handler->handle = handle;
	handler->func = func;
	handler->data = data;
	mutex_lock(&ec->lock);
	list_add(&handler->node, &ec->list);
	mutex_unlock(&ec->lock);
	return 0;
}

EXPORT_SYMBOL_GPL(acpi_ec_add_query_handler);

void acpi_ec_remove_query_handler(struct acpi_ec *ec, u8 query_bit)
{
	struct acpi_ec_query_handler *handler, *tmp;
	mutex_lock(&ec->lock);
	list_for_each_entry_safe(handler, tmp, &ec->list, node) {
		if (query_bit == handler->query_bit) {
			list_del(&handler->node);
			kfree(handler);
		}
	}
	mutex_unlock(&ec->lock);
}

EXPORT_SYMBOL_GPL(acpi_ec_remove_query_handler);

static void acpi_ec_gpe_query(void *ec_cxt)
{
	struct acpi_ec *ec = ec_cxt;
	u8 value = 0;
	struct acpi_ec_query_handler *handler, copy;

	if (!ec || acpi_ec_query(ec, &value))
		return;
	mutex_lock(&ec->lock);
	list_for_each_entry(handler, &ec->list, node) {
		if (value == handler->query_bit) {
			/* have custom handler for this bit */
			memcpy(&copy, handler, sizeof(copy));
			mutex_unlock(&ec->lock);
			if (copy.func) {
				copy.func(copy.data);
			} else if (copy.handle) {
				acpi_evaluate_object(copy.handle, NULL, NULL, NULL);
			}
			return;
		}
	}
	mutex_unlock(&ec->lock);
}

static u32 acpi_ec_gpe_handler(void *data)
{
	struct acpi_ec *ec = data;
	u8 status;

	pr_debug(PREFIX "~~~> interrupt\n");
	status = acpi_ec_read_status(ec);

	if (test_bit(EC_FLAGS_GPE_MODE, &ec->flags)) {
		gpe_transaction(ec, status);
		if (ec_transaction_done(ec) &&
		    (status & ACPI_EC_FLAG_IBF) == 0)
			wake_up(&ec->wait);
	}

	ec_check_sci(ec, status);
	if (!test_bit(EC_FLAGS_GPE_MODE, &ec->flags) &&
	    !test_bit(EC_FLAGS_NO_GPE, &ec->flags)) {
		/* this is non-query, must be confirmation */
		if (!test_bit(EC_FLAGS_GPE_STORM, &ec->flags)) {
			if (printk_ratelimit())
				pr_info(PREFIX "non-query interrupt received,"
					" switching to interrupt mode\n");
		} else {
			/* hush, STORM switches the mode every transaction */
			pr_debug(PREFIX "non-query interrupt received,"
				" switching to interrupt mode\n");
		}
		set_bit(EC_FLAGS_GPE_MODE, &ec->flags);
	}
	return ACPI_INTERRUPT_HANDLED;
}

/* --------------------------------------------------------------------------
                             Address Space Management
   -------------------------------------------------------------------------- */

static acpi_status
acpi_ec_space_handler(u32 function, acpi_physical_address address,
		      u32 bits, acpi_integer *value,
		      void *handler_context, void *region_context)
{
	struct acpi_ec *ec = handler_context;
	int result = 0, i;
	u8 temp = 0;

	if ((address > 0xFF) || !value || !handler_context)
		return AE_BAD_PARAMETER;

	if (function != ACPI_READ && function != ACPI_WRITE)
		return AE_BAD_PARAMETER;

	if (bits != 8 && acpi_strict)
		return AE_BAD_PARAMETER;

	acpi_ec_burst_enable(ec);

	if (function == ACPI_READ) {
		result = acpi_ec_read(ec, address, &temp);
		*value = temp;
	} else {
		temp = 0xff & (*value);
		result = acpi_ec_write(ec, address, temp);
	}

	for (i = 8; unlikely(bits - i > 0); i += 8) {
		++address;
		if (function == ACPI_READ) {
			result = acpi_ec_read(ec, address, &temp);
			(*value) |= ((acpi_integer)temp) << i;
		} else {
			temp = 0xff & ((*value) >> i);
			result = acpi_ec_write(ec, address, temp);
		}
	}

	acpi_ec_burst_disable(ec);

	switch (result) {
	case -EINVAL:
		return AE_BAD_PARAMETER;
		break;
	case -ENODEV:
		return AE_NOT_FOUND;
		break;
	case -ETIME:
		return AE_TIME;
		break;
	default:
		return AE_OK;
	}
}

/* --------------------------------------------------------------------------
                              FS Interface (/proc)
   -------------------------------------------------------------------------- */

static struct proc_dir_entry *acpi_ec_dir;

static int acpi_ec_read_info(struct seq_file *seq, void *offset)
{
	struct acpi_ec *ec = seq->private;

	if (!ec)
		goto end;

	seq_printf(seq, "gpe:\t\t\t0x%02x\n", (u32) ec->gpe);
	seq_printf(seq, "ports:\t\t\t0x%02x, 0x%02x\n",
		   (unsigned)ec->command_addr, (unsigned)ec->data_addr);
	seq_printf(seq, "use global lock:\t%s\n",
		   ec->global_lock ? "yes" : "no");
      end:
	return 0;
}

static int acpi_ec_info_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, acpi_ec_read_info, PDE(inode)->data);
}

static struct file_operations acpi_ec_info_ops = {
	.open = acpi_ec_info_open_fs,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static int acpi_ec_add_fs(struct acpi_device *device)
{
	struct proc_dir_entry *entry = NULL;

	if (!acpi_device_dir(device)) {
		acpi_device_dir(device) = proc_mkdir(acpi_device_bid(device),
						     acpi_ec_dir);
		if (!acpi_device_dir(device))
			return -ENODEV;
	}

	entry = proc_create_data(ACPI_EC_FILE_INFO, S_IRUGO,
				 acpi_device_dir(device),
				 &acpi_ec_info_ops, acpi_driver_data(device));
	if (!entry)
		return -ENODEV;
	return 0;
}

static int acpi_ec_remove_fs(struct acpi_device *device)
{

	if (acpi_device_dir(device)) {
		remove_proc_entry(ACPI_EC_FILE_INFO, acpi_device_dir(device));
		remove_proc_entry(acpi_device_bid(device), acpi_ec_dir);
		acpi_device_dir(device) = NULL;
	}

	return 0;
}

/* --------------------------------------------------------------------------
                               Driver Interface
   -------------------------------------------------------------------------- */
static acpi_status
ec_parse_io_ports(struct acpi_resource *resource, void *context);

static struct acpi_ec *make_acpi_ec(void)
{
	struct acpi_ec *ec = kzalloc(sizeof(struct acpi_ec), GFP_KERNEL);
	if (!ec)
		return NULL;
	ec->flags = 1 << EC_FLAGS_QUERY_PENDING;
	mutex_init(&ec->lock);
	init_waitqueue_head(&ec->wait);
	INIT_LIST_HEAD(&ec->list);
	spin_lock_init(&ec->curr_lock);
	return ec;
}

static acpi_status
acpi_ec_register_query_methods(acpi_handle handle, u32 level,
			       void *context, void **return_value)
{
	struct acpi_namespace_node *node = handle;
	struct acpi_ec *ec = context;
	int value = 0;

	if (sscanf(node->name.ascii, "_Q%2x", &value) == 1)
		acpi_ec_add_query_handler(ec, value, handle, NULL, NULL);

	return AE_OK;
}

static acpi_status
ec_parse_device(acpi_handle handle, u32 Level, void *context, void **retval)
{
	acpi_status status;
	unsigned long long tmp = 0;

	struct acpi_ec *ec = context;
	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     ec_parse_io_ports, ec);
	if (ACPI_FAILURE(status))
		return status;

	/* Get GPE bit assignment (EC events). */
	/* TODO: Add support for _GPE returning a package */
	status = acpi_evaluate_integer(handle, "_GPE", NULL, &tmp);
	if (ACPI_FAILURE(status))
		return status;
	ec->gpe = tmp;
	/* Use the global lock for all EC transactions? */
	tmp = 0;
	acpi_evaluate_integer(handle, "_GLK", NULL, &tmp);
	ec->global_lock = tmp;
	ec->handle = handle;
	return AE_CTRL_TERMINATE;
}

static void ec_remove_handlers(struct acpi_ec *ec)
{
	if (ACPI_FAILURE(acpi_remove_address_space_handler(ec->handle,
				ACPI_ADR_SPACE_EC, &acpi_ec_space_handler)))
		pr_err(PREFIX "failed to remove space handler\n");
	if (ACPI_FAILURE(acpi_remove_gpe_handler(NULL, ec->gpe,
				&acpi_ec_gpe_handler)))
		pr_err(PREFIX "failed to remove gpe handler\n");
	clear_bit(EC_FLAGS_HANDLERS_INSTALLED, &ec->flags);
}

static int acpi_ec_add(struct acpi_device *device)
{
	struct acpi_ec *ec = NULL;

	if (!device)
		return -EINVAL;
	strcpy(acpi_device_name(device), ACPI_EC_DEVICE_NAME);
	strcpy(acpi_device_class(device), ACPI_EC_CLASS);

	/* Check for boot EC */
	if (boot_ec &&
	    (boot_ec->handle == device->handle ||
	     boot_ec->handle == ACPI_ROOT_OBJECT)) {
		ec = boot_ec;
		boot_ec = NULL;
	} else {
		ec = make_acpi_ec();
		if (!ec)
			return -ENOMEM;
		if (ec_parse_device(device->handle, 0, ec, NULL) !=
		    AE_CTRL_TERMINATE) {
			kfree(ec);
			return -EINVAL;
		}
	}

	ec->handle = device->handle;

	/* Find and register all query methods */
	acpi_walk_namespace(ACPI_TYPE_METHOD, ec->handle, 1,
			    acpi_ec_register_query_methods, ec, NULL);

	if (!first_ec)
		first_ec = ec;
	device->driver_data = ec;
	acpi_ec_add_fs(device);
	pr_info(PREFIX "GPE = 0x%lx, I/O: command/status = 0x%lx, data = 0x%lx\n",
			  ec->gpe, ec->command_addr, ec->data_addr);
	pr_info(PREFIX "driver started in %s mode\n",
		(test_bit(EC_FLAGS_GPE_MODE, &ec->flags))?"interrupt":"poll");
	return 0;
}

static int acpi_ec_remove(struct acpi_device *device, int type)
{
	struct acpi_ec *ec;
	struct acpi_ec_query_handler *handler, *tmp;

	if (!device)
		return -EINVAL;

	ec = acpi_driver_data(device);
	mutex_lock(&ec->lock);
	list_for_each_entry_safe(handler, tmp, &ec->list, node) {
		list_del(&handler->node);
		kfree(handler);
	}
	mutex_unlock(&ec->lock);
	acpi_ec_remove_fs(device);
	device->driver_data = NULL;
	if (ec == first_ec)
		first_ec = NULL;
	kfree(ec);
	return 0;
}

static acpi_status
ec_parse_io_ports(struct acpi_resource *resource, void *context)
{
	struct acpi_ec *ec = context;

	if (resource->type != ACPI_RESOURCE_TYPE_IO)
		return AE_OK;

	/*
	 * The first address region returned is the data port, and
	 * the second address region returned is the status/command
	 * port.
	 */
	if (ec->data_addr == 0)
		ec->data_addr = resource->data.io.minimum;
	else if (ec->command_addr == 0)
		ec->command_addr = resource->data.io.minimum;
	else
		return AE_CTRL_TERMINATE;

	return AE_OK;
}

static int ec_install_handlers(struct acpi_ec *ec)
{
	acpi_status status;
	if (test_bit(EC_FLAGS_HANDLERS_INSTALLED, &ec->flags))
		return 0;
	status = acpi_install_gpe_handler(NULL, ec->gpe,
				  ACPI_GPE_EDGE_TRIGGERED,
				  &acpi_ec_gpe_handler, ec);
	if (ACPI_FAILURE(status))
		return -ENODEV;
	acpi_set_gpe_type(NULL, ec->gpe, ACPI_GPE_TYPE_RUNTIME);
	acpi_enable_gpe(NULL, ec->gpe);
	status = acpi_install_address_space_handler(ec->handle,
						    ACPI_ADR_SPACE_EC,
						    &acpi_ec_space_handler,
						    NULL, ec);
	if (ACPI_FAILURE(status)) {
		if (status == AE_NOT_FOUND) {
			/*
			 * Maybe OS fails in evaluating the _REG object.
			 * The AE_NOT_FOUND error will be ignored and OS
			 * continue to initialize EC.
			 */
			printk(KERN_ERR "Fail in evaluating the _REG object"
				" of EC device. Broken bios is suspected.\n");
		} else {
			acpi_remove_gpe_handler(NULL, ec->gpe,
				&acpi_ec_gpe_handler);
			return -ENODEV;
		}
	}

	set_bit(EC_FLAGS_HANDLERS_INSTALLED, &ec->flags);
	return 0;
}

static int acpi_ec_start(struct acpi_device *device)
{
	struct acpi_ec *ec;
	int ret = 0;

	if (!device)
		return -EINVAL;

	ec = acpi_driver_data(device);

	if (!ec)
		return -EINVAL;

	ret = ec_install_handlers(ec);

	/* EC is fully operational, allow queries */
	clear_bit(EC_FLAGS_QUERY_PENDING, &ec->flags);
	return ret;
}

static int acpi_ec_stop(struct acpi_device *device, int type)
{
	struct acpi_ec *ec;
	if (!device)
		return -EINVAL;
	ec = acpi_driver_data(device);
	if (!ec)
		return -EINVAL;
	ec_remove_handlers(ec);

	return 0;
}

int __init acpi_boot_ec_enable(void)
{
	if (!boot_ec || test_bit(EC_FLAGS_HANDLERS_INSTALLED, &boot_ec->flags))
		return 0;
	if (!ec_install_handlers(boot_ec)) {
		first_ec = boot_ec;
		return 0;
	}
	return -EFAULT;
}

static const struct acpi_device_id ec_device_ids[] = {
	{"PNP0C09", 0},
	{"", 0},
};

int __init acpi_ec_ecdt_probe(void)
{
	int ret;
	acpi_status status;
	struct acpi_table_ecdt *ecdt_ptr;

	boot_ec = make_acpi_ec();
	if (!boot_ec)
		return -ENOMEM;
	/*
	 * Generate a boot ec context
	 */
	status = acpi_get_table(ACPI_SIG_ECDT, 1,
				(struct acpi_table_header **)&ecdt_ptr);
	if (ACPI_SUCCESS(status)) {
		pr_info(PREFIX "EC description table is found, configuring boot EC\n");
		boot_ec->command_addr = ecdt_ptr->control.address;
		boot_ec->data_addr = ecdt_ptr->data.address;
		if (dmi_check_system(ec_dmi_table)) {
			/*
			 * If the board falls into ec_dmi_table, it means
			 * that ECDT table gives the incorrect command/status
			 * & data I/O address. Just fix it.
			 */
			boot_ec->data_addr = ecdt_ptr->control.address;
			boot_ec->command_addr = ecdt_ptr->data.address;
		}
		boot_ec->gpe = ecdt_ptr->gpe;
		boot_ec->handle = ACPI_ROOT_OBJECT;
		acpi_get_handle(ACPI_ROOT_OBJECT, ecdt_ptr->id, &boot_ec->handle);
	} else {
		/* This workaround is needed only on some broken machines,
		 * which require early EC, but fail to provide ECDT */
		acpi_handle x;
		printk(KERN_DEBUG PREFIX "Look up EC in DSDT\n");
		status = acpi_get_devices(ec_device_ids[0].id, ec_parse_device,
						boot_ec, NULL);
		/* Check that acpi_get_devices actually find something */
		if (ACPI_FAILURE(status) || !boot_ec->handle)
			goto error;
		/* We really need to limit this workaround, the only ASUS,
		 * which needs it, has fake EC._INI method, so use it as flag.
		 * Keep boot_ec struct as it will be needed soon.
		 */
		if (ACPI_FAILURE(acpi_get_handle(boot_ec->handle, "_INI", &x)))
			return -ENODEV;
	}

	ret = ec_install_handlers(boot_ec);
	if (!ret) {
		first_ec = boot_ec;
		return 0;
	}
      error:
	kfree(boot_ec);
	boot_ec = NULL;
	return -ENODEV;
}

static int acpi_ec_suspend(struct acpi_device *device, pm_message_t state)
{
	struct acpi_ec *ec = acpi_driver_data(device);
	/* Stop using GPE */
	set_bit(EC_FLAGS_NO_GPE, &ec->flags);
	clear_bit(EC_FLAGS_GPE_MODE, &ec->flags);
	acpi_disable_gpe(NULL, ec->gpe);
	return 0;
}

static int acpi_ec_resume(struct acpi_device *device)
{
	struct acpi_ec *ec = acpi_driver_data(device);
	/* Enable use of GPE back */
	clear_bit(EC_FLAGS_NO_GPE, &ec->flags);
	acpi_enable_gpe(NULL, ec->gpe);
	return 0;
}

static struct acpi_driver acpi_ec_driver = {
	.name = "ec",
	.class = ACPI_EC_CLASS,
	.ids = ec_device_ids,
	.ops = {
		.add = acpi_ec_add,
		.remove = acpi_ec_remove,
		.start = acpi_ec_start,
		.stop = acpi_ec_stop,
		.suspend = acpi_ec_suspend,
		.resume = acpi_ec_resume,
		},
};

static int __init acpi_ec_init(void)
{
	int result = 0;

	if (acpi_disabled)
		return 0;

	acpi_ec_dir = proc_mkdir(ACPI_EC_CLASS, acpi_root_dir);
	if (!acpi_ec_dir)
		return -ENODEV;

	/* Now register the driver for the EC */
	result = acpi_bus_register_driver(&acpi_ec_driver);
	if (result < 0) {
		remove_proc_entry(ACPI_EC_CLASS, acpi_root_dir);
		return -ENODEV;
	}

	return result;
}

subsys_initcall(acpi_ec_init);

/* EC driver currently not unloadable */
#if 0
static void __exit acpi_ec_exit(void)
{

	acpi_bus_unregister_driver(&acpi_ec_driver);

	remove_proc_entry(ACPI_EC_CLASS, acpi_root_dir);

	return;
}
#endif	/* 0 */
