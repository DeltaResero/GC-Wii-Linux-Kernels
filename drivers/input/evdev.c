/*
 * Event char devices, giving access to raw input device events.
 *
 * Copyright (c) 1999-2002 Vojtech Pavlik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#define EVDEV_MINOR_BASE	64
#define EVDEV_MINORS		32
#define EVDEV_BUFFER_SIZE	64

#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/major.h>
#include <linux/device.h>
#include <linux/compat.h>

struct evdev {
	int exist;
	int open;
	int minor;
	char name[16];
	struct input_handle handle;
	wait_queue_head_t wait;
	struct evdev_client *grab;
	struct list_head client_list;
	spinlock_t client_lock; /* protects client_list */
	struct mutex mutex;
	struct device dev;
};

struct evdev_client {
	struct input_event buffer[EVDEV_BUFFER_SIZE];
	int head;
	int tail;
	spinlock_t buffer_lock; /* protects access to buffer, head and tail */
	struct fasync_struct *fasync;
	struct evdev *evdev;
	struct list_head node;
};

static struct evdev *evdev_table[EVDEV_MINORS];
static DEFINE_MUTEX(evdev_table_mutex);

static void evdev_pass_event(struct evdev_client *client,
			     struct input_event *event)
{
	/*
	 * Interrupts are disabled, just acquire the lock
	 */
	spin_lock(&client->buffer_lock);
	client->buffer[client->head++] = *event;
	client->head &= EVDEV_BUFFER_SIZE - 1;
	spin_unlock(&client->buffer_lock);

	kill_fasync(&client->fasync, SIGIO, POLL_IN);
}

/*
 * Pass incoming event to all connected clients. Note that we are
 * caleld under a spinlock with interrupts off so we don't need
 * to use rcu_read_lock() here. Writers will be using syncronize_sched()
 * instead of synchrnoize_rcu().
 */
static void evdev_event(struct input_handle *handle,
			unsigned int type, unsigned int code, int value)
{
	struct evdev *evdev = handle->private;
	struct evdev_client *client;
	struct input_event event;

	do_gettimeofday(&event.time);
	event.type = type;
	event.code = code;
	event.value = value;

	client = rcu_dereference(evdev->grab);
	if (client)
		evdev_pass_event(client, &event);
	else
		list_for_each_entry_rcu(client, &evdev->client_list, node)
			evdev_pass_event(client, &event);

	wake_up_interruptible(&evdev->wait);
}

static int evdev_fasync(int fd, struct file *file, int on)
{
	struct evdev_client *client = file->private_data;
	int retval;

	retval = fasync_helper(fd, file, on, &client->fasync);

	return retval < 0 ? retval : 0;
}

static int evdev_flush(struct file *file, fl_owner_t id)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	int retval;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist)
		retval = -ENODEV;
	else
		retval = input_flush_device(&evdev->handle, file);

	mutex_unlock(&evdev->mutex);
	return retval;
}

static void evdev_free(struct device *dev)
{
	struct evdev *evdev = container_of(dev, struct evdev, dev);

	kfree(evdev);
}

/*
 * Grabs an event device (along with underlying input device).
 * This function is called with evdev->mutex taken.
 */
static int evdev_grab(struct evdev *evdev, struct evdev_client *client)
{
	int error;

	if (evdev->grab)
		return -EBUSY;

	error = input_grab_device(&evdev->handle);
	if (error)
		return error;

	rcu_assign_pointer(evdev->grab, client);
	/*
	 * We don't use synchronize_rcu() here because read-side
	 * critical section is protected by a spinlock instead
	 * of rcu_read_lock().
	 */
	synchronize_sched();

	return 0;
}

static int evdev_ungrab(struct evdev *evdev, struct evdev_client *client)
{
	if (evdev->grab != client)
		return  -EINVAL;

	rcu_assign_pointer(evdev->grab, NULL);
	synchronize_sched();
	input_release_device(&evdev->handle);

	return 0;
}

static void evdev_attach_client(struct evdev *evdev,
				struct evdev_client *client)
{
	spin_lock(&evdev->client_lock);
	list_add_tail_rcu(&client->node, &evdev->client_list);
	spin_unlock(&evdev->client_lock);
	synchronize_sched();
}

static void evdev_detach_client(struct evdev *evdev,
				struct evdev_client *client)
{
	spin_lock(&evdev->client_lock);
	list_del_rcu(&client->node);
	spin_unlock(&evdev->client_lock);
	synchronize_sched();
}

static int evdev_open_device(struct evdev *evdev)
{
	int retval;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist)
		retval = -ENODEV;
	else if (!evdev->open++) {
		retval = input_open_device(&evdev->handle);
		if (retval)
			evdev->open--;
	}

	mutex_unlock(&evdev->mutex);
	return retval;
}

static void evdev_close_device(struct evdev *evdev)
{
	mutex_lock(&evdev->mutex);

	if (evdev->exist && !--evdev->open)
		input_close_device(&evdev->handle);

	mutex_unlock(&evdev->mutex);
}

/*
 * Wake up users waiting for IO so they can disconnect from
 * dead device.
 */
static void evdev_hangup(struct evdev *evdev)
{
	struct evdev_client *client;

	spin_lock(&evdev->client_lock);
	list_for_each_entry(client, &evdev->client_list, node)
		kill_fasync(&client->fasync, SIGIO, POLL_HUP);
	spin_unlock(&evdev->client_lock);

	wake_up_interruptible(&evdev->wait);
}

static int evdev_release(struct inode *inode, struct file *file)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;

	mutex_lock(&evdev->mutex);
	if (evdev->grab == client)
		evdev_ungrab(evdev, client);
	mutex_unlock(&evdev->mutex);

	evdev_fasync(-1, file, 0);
	evdev_detach_client(evdev, client);
	kfree(client);

	evdev_close_device(evdev);
	put_device(&evdev->dev);

	return 0;
}

static int evdev_open(struct inode *inode, struct file *file)
{
	struct evdev *evdev;
	struct evdev_client *client;
	int i = iminor(inode) - EVDEV_MINOR_BASE;
	int error;

	if (i >= EVDEV_MINORS)
		return -ENODEV;

	error = mutex_lock_interruptible(&evdev_table_mutex);
	if (error)
		return error;
	evdev = evdev_table[i];
	if (evdev)
		get_device(&evdev->dev);
	mutex_unlock(&evdev_table_mutex);

	if (!evdev)
		return -ENODEV;

	client = kzalloc(sizeof(struct evdev_client), GFP_KERNEL);
	if (!client) {
		error = -ENOMEM;
		goto err_put_evdev;
	}

	spin_lock_init(&client->buffer_lock);
	client->evdev = evdev;
	evdev_attach_client(evdev, client);

	error = evdev_open_device(evdev);
	if (error)
		goto err_free_client;

	file->private_data = client;
	return 0;

 err_free_client:
	evdev_detach_client(evdev, client);
	kfree(client);
 err_put_evdev:
	put_device(&evdev->dev);
	return error;
}

#ifdef CONFIG_COMPAT

struct input_event_compat {
	struct compat_timeval time;
	__u16 type;
	__u16 code;
	__s32 value;
};

/* Note to the author of this code: did it ever occur to
   you why the ifdefs are needed? Think about it again. -AK */
#ifdef CONFIG_X86_64
#  define COMPAT_TEST is_compat_task()
#elif defined(CONFIG_IA64)
#  define COMPAT_TEST IS_IA32_PROCESS(task_pt_regs(current))
#elif defined(CONFIG_S390)
#  define COMPAT_TEST test_thread_flag(TIF_31BIT)
#elif defined(CONFIG_MIPS)
#  define COMPAT_TEST test_thread_flag(TIF_32BIT_ADDR)
#else
#  define COMPAT_TEST test_thread_flag(TIF_32BIT)
#endif

static inline size_t evdev_event_size(void)
{
	return COMPAT_TEST ?
		sizeof(struct input_event_compat) : sizeof(struct input_event);
}

static int evdev_event_from_user(const char __user *buffer,
				 struct input_event *event)
{
	if (COMPAT_TEST) {
		struct input_event_compat compat_event;

		if (copy_from_user(&compat_event, buffer,
				   sizeof(struct input_event_compat)))
			return -EFAULT;

		event->time.tv_sec = compat_event.time.tv_sec;
		event->time.tv_usec = compat_event.time.tv_usec;
		event->type = compat_event.type;
		event->code = compat_event.code;
		event->value = compat_event.value;

	} else {
		if (copy_from_user(event, buffer, sizeof(struct input_event)))
			return -EFAULT;
	}

	return 0;
}

static int evdev_event_to_user(char __user *buffer,
				const struct input_event *event)
{
	if (COMPAT_TEST) {
		struct input_event_compat compat_event;

		compat_event.time.tv_sec = event->time.tv_sec;
		compat_event.time.tv_usec = event->time.tv_usec;
		compat_event.type = event->type;
		compat_event.code = event->code;
		compat_event.value = event->value;

		if (copy_to_user(buffer, &compat_event,
				 sizeof(struct input_event_compat)))
			return -EFAULT;

	} else {
		if (copy_to_user(buffer, event, sizeof(struct input_event)))
			return -EFAULT;
	}

	return 0;
}

#else

static inline size_t evdev_event_size(void)
{
	return sizeof(struct input_event);
}

static int evdev_event_from_user(const char __user *buffer,
				 struct input_event *event)
{
	if (copy_from_user(event, buffer, sizeof(struct input_event)))
		return -EFAULT;

	return 0;
}

static int evdev_event_to_user(char __user *buffer,
				const struct input_event *event)
{
	if (copy_to_user(buffer, event, sizeof(struct input_event)))
		return -EFAULT;

	return 0;
}

#endif /* CONFIG_COMPAT */

static ssize_t evdev_write(struct file *file, const char __user *buffer,
			   size_t count, loff_t *ppos)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	struct input_event event;
	int retval;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist) {
		retval = -ENODEV;
		goto out;
	}

	while (retval < count) {

		if (evdev_event_from_user(buffer + retval, &event)) {
			retval = -EFAULT;
			goto out;
		}

		input_inject_event(&evdev->handle,
				   event.type, event.code, event.value);
		retval += evdev_event_size();
	}

 out:
	mutex_unlock(&evdev->mutex);
	return retval;
}

static int evdev_fetch_next_event(struct evdev_client *client,
				  struct input_event *event)
{
	int have_event;

	spin_lock_irq(&client->buffer_lock);

	have_event = client->head != client->tail;
	if (have_event) {
		*event = client->buffer[client->tail++];
		client->tail &= EVDEV_BUFFER_SIZE - 1;
	}

	spin_unlock_irq(&client->buffer_lock);

	return have_event;
}

static ssize_t evdev_read(struct file *file, char __user *buffer,
			  size_t count, loff_t *ppos)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	struct input_event event;
	int retval;

	if (count < evdev_event_size())
		return -EINVAL;

	if (client->head == client->tail && evdev->exist &&
	    (file->f_flags & O_NONBLOCK))
		return -EAGAIN;

	retval = wait_event_interruptible(evdev->wait,
		client->head != client->tail || !evdev->exist);
	if (retval)
		return retval;

	if (!evdev->exist)
		return -ENODEV;

	while (retval + evdev_event_size() <= count &&
	       evdev_fetch_next_event(client, &event)) {

		if (evdev_event_to_user(buffer + retval, &event))
			return -EFAULT;

		retval += evdev_event_size();
	}

	return retval;
}

/* No kernel lock - fine */
static unsigned int evdev_poll(struct file *file, poll_table *wait)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;

	poll_wait(file, &evdev->wait, wait);
	return ((client->head == client->tail) ? 0 : (POLLIN | POLLRDNORM)) |
		(evdev->exist ? 0 : (POLLHUP | POLLERR));
}

#ifdef CONFIG_COMPAT

#define BITS_PER_LONG_COMPAT (sizeof(compat_long_t) * 8)
#define NBITS_COMPAT(x) ((((x) - 1) / BITS_PER_LONG_COMPAT) + 1)

#ifdef __BIG_ENDIAN
static int bits_to_user(unsigned long *bits, unsigned int maxbit,
			unsigned int maxlen, void __user *p, int compat)
{
	int len, i;

	if (compat) {
		len = NBITS_COMPAT(maxbit) * sizeof(compat_long_t);
		if (len > maxlen)
			len = maxlen;

		for (i = 0; i < len / sizeof(compat_long_t); i++)
			if (copy_to_user((compat_long_t __user *) p + i,
					 (compat_long_t *) bits +
						i + 1 - ((i % 2) << 1),
					 sizeof(compat_long_t)))
				return -EFAULT;
	} else {
		len = NBITS(maxbit) * sizeof(long);
		if (len > maxlen)
			len = maxlen;

		if (copy_to_user(p, bits, len))
			return -EFAULT;
	}

	return len;
}
#else
static int bits_to_user(unsigned long *bits, unsigned int maxbit,
			unsigned int maxlen, void __user *p, int compat)
{
	int len = compat ?
			NBITS_COMPAT(maxbit) * sizeof(compat_long_t) :
			NBITS(maxbit) * sizeof(long);

	if (len > maxlen)
		len = maxlen;

	return copy_to_user(p, bits, len) ? -EFAULT : len;
}
#endif /* __BIG_ENDIAN */

#else

static int bits_to_user(unsigned long *bits, unsigned int maxbit,
			unsigned int maxlen, void __user *p, int compat)
{
	int len = NBITS(maxbit) * sizeof(long);

	if (len > maxlen)
		len = maxlen;

	return copy_to_user(p, bits, len) ? -EFAULT : len;
}

#endif /* CONFIG_COMPAT */

static int str_to_user(const char *str, unsigned int maxlen, void __user *p)
{
	int len;

	if (!str)
		return -ENOENT;

	len = strlen(str) + 1;
	if (len > maxlen)
		len = maxlen;

	return copy_to_user(p, str, len) ? -EFAULT : len;
}

static long evdev_do_ioctl(struct file *file, unsigned int cmd,
			   void __user *p, int compat_mode)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	struct input_dev *dev = evdev->handle.dev;
	struct input_absinfo abs;
	struct ff_effect effect;
	int __user *ip = (int __user *)p;
	int i, t, u, v;
	int error;

	switch (cmd) {

	case EVIOCGVERSION:
		return put_user(EV_VERSION, ip);

	case EVIOCGID:
		if (copy_to_user(p, &dev->id, sizeof(struct input_id)))
			return -EFAULT;
		return 0;

	case EVIOCGREP:
		if (!test_bit(EV_REP, dev->evbit))
			return -ENOSYS;
		if (put_user(dev->rep[REP_DELAY], ip))
			return -EFAULT;
		if (put_user(dev->rep[REP_PERIOD], ip + 1))
			return -EFAULT;
		return 0;

	case EVIOCSREP:
		if (!test_bit(EV_REP, dev->evbit))
			return -ENOSYS;
		if (get_user(u, ip))
			return -EFAULT;
		if (get_user(v, ip + 1))
			return -EFAULT;

		input_inject_event(&evdev->handle, EV_REP, REP_DELAY, u);
		input_inject_event(&evdev->handle, EV_REP, REP_PERIOD, v);

		return 0;

	case EVIOCGKEYCODE:
		if (get_user(t, ip))
			return -EFAULT;

		error = dev->getkeycode(dev, t, &v);
		if (error)
			return error;

		if (put_user(v, ip + 1))
			return -EFAULT;

		return 0;

	case EVIOCSKEYCODE:
		if (get_user(t, ip) || get_user(v, ip + 1))
			return -EFAULT;

		return dev->setkeycode(dev, t, v);

	case EVIOCSFF:
		if (copy_from_user(&effect, p, sizeof(effect)))
			return -EFAULT;

		error = input_ff_upload(dev, &effect, file);

		if (put_user(effect.id, &(((struct ff_effect __user *)p)->id)))
			return -EFAULT;

		return error;

	case EVIOCRMFF:
		return input_ff_erase(dev, (int)(unsigned long) p, file);

	case EVIOCGEFFECTS:
		i = test_bit(EV_FF, dev->evbit) ?
				dev->ff->max_effects : 0;
		if (put_user(i, ip))
			return -EFAULT;
		return 0;

	case EVIOCGRAB:
		if (p)
			return evdev_grab(evdev, client);
		else
			return evdev_ungrab(evdev, client);

	default:

		if (_IOC_TYPE(cmd) != 'E')
			return -EINVAL;

		if (_IOC_DIR(cmd) == _IOC_READ) {

			if ((_IOC_NR(cmd) & ~EV_MAX) == _IOC_NR(EVIOCGBIT(0, 0))) {

				unsigned long *bits;
				int len;

				switch (_IOC_NR(cmd) & EV_MAX) {

				case      0: bits = dev->evbit;  len = EV_MAX;  break;
				case EV_KEY: bits = dev->keybit; len = KEY_MAX; break;
				case EV_REL: bits = dev->relbit; len = REL_MAX; break;
				case EV_ABS: bits = dev->absbit; len = ABS_MAX; break;
				case EV_MSC: bits = dev->mscbit; len = MSC_MAX; break;
				case EV_LED: bits = dev->ledbit; len = LED_MAX; break;
				case EV_SND: bits = dev->sndbit; len = SND_MAX; break;
				case EV_FF:  bits = dev->ffbit;  len = FF_MAX;  break;
				case EV_SW:  bits = dev->swbit;  len = SW_MAX;  break;
				default: return -EINVAL;
			}
				return bits_to_user(bits, len, _IOC_SIZE(cmd), p, compat_mode);
			}

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGKEY(0)))
				return bits_to_user(dev->key, KEY_MAX, _IOC_SIZE(cmd),
						    p, compat_mode);

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGLED(0)))
				return bits_to_user(dev->led, LED_MAX, _IOC_SIZE(cmd),
						    p, compat_mode);

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGSND(0)))
				return bits_to_user(dev->snd, SND_MAX, _IOC_SIZE(cmd),
						    p, compat_mode);

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGSW(0)))
				return bits_to_user(dev->sw, SW_MAX, _IOC_SIZE(cmd),
						    p, compat_mode);

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGNAME(0)))
				return str_to_user(dev->name, _IOC_SIZE(cmd), p);

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGPHYS(0)))
				return str_to_user(dev->phys, _IOC_SIZE(cmd), p);

			if (_IOC_NR(cmd) == _IOC_NR(EVIOCGUNIQ(0)))
				return str_to_user(dev->uniq, _IOC_SIZE(cmd), p);

			if ((_IOC_NR(cmd) & ~ABS_MAX) == _IOC_NR(EVIOCGABS(0))) {

				t = _IOC_NR(cmd) & ABS_MAX;

				abs.value = dev->abs[t];
				abs.minimum = dev->absmin[t];
				abs.maximum = dev->absmax[t];
				abs.fuzz = dev->absfuzz[t];
				abs.flat = dev->absflat[t];

				if (copy_to_user(p, &abs, sizeof(struct input_absinfo)))
					return -EFAULT;

				return 0;
			}

		}

		if (_IOC_DIR(cmd) == _IOC_WRITE) {

			if ((_IOC_NR(cmd) & ~ABS_MAX) == _IOC_NR(EVIOCSABS(0))) {

				t = _IOC_NR(cmd) & ABS_MAX;

				if (copy_from_user(&abs, p,
						sizeof(struct input_absinfo)))
					return -EFAULT;

				/*
				 * Take event lock to ensure that we are not
				 * changing device parameters in the middle
				 * of event.
				 */
				spin_lock_irq(&dev->event_lock);

				dev->abs[t] = abs.value;
				dev->absmin[t] = abs.minimum;
				dev->absmax[t] = abs.maximum;
				dev->absfuzz[t] = abs.fuzz;
				dev->absflat[t] = abs.flat;

				spin_unlock_irq(&dev->event_lock);

				return 0;
			}
		}
	}
	return -EINVAL;
}

static long evdev_ioctl_handler(struct file *file, unsigned int cmd,
				void __user *p, int compat_mode)
{
	struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	int retval;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist) {
		retval = -ENODEV;
		goto out;
	}

	retval = evdev_do_ioctl(file, cmd, p, compat_mode);

 out:
	mutex_unlock(&evdev->mutex);
	return retval;
}

static long evdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return evdev_ioctl_handler(file, cmd, (void __user *)arg, 0);
}

#ifdef CONFIG_COMPAT
static long evdev_ioctl_compat(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	return evdev_ioctl_handler(file, cmd, compat_ptr(arg), 1);
}
#endif

static const struct file_operations evdev_fops = {
	.owner		= THIS_MODULE,
	.read		= evdev_read,
	.write		= evdev_write,
	.poll		= evdev_poll,
	.open		= evdev_open,
	.release	= evdev_release,
	.unlocked_ioctl	= evdev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= evdev_ioctl_compat,
#endif
	.fasync		= evdev_fasync,
	.flush		= evdev_flush
};

static int evdev_install_chrdev(struct evdev *evdev)
{
	/*
	 * No need to do any locking here as calls to connect and
	 * disconnect are serialized by the input core
	 */
	evdev_table[evdev->minor] = evdev;
	return 0;
}

static void evdev_remove_chrdev(struct evdev *evdev)
{
	/*
	 * Lock evdev table to prevent race with evdev_open()
	 */
	mutex_lock(&evdev_table_mutex);
	evdev_table[evdev->minor] = NULL;
	mutex_unlock(&evdev_table_mutex);
}

/*
 * Mark device non-existent. This disables writes, ioctls and
 * prevents new users from opening the device. Already posted
 * blocking reads will stay, however new ones will fail.
 */
static void evdev_mark_dead(struct evdev *evdev)
{
	mutex_lock(&evdev->mutex);
	evdev->exist = 0;
	mutex_unlock(&evdev->mutex);
}

static void evdev_cleanup(struct evdev *evdev)
{
	struct input_handle *handle = &evdev->handle;

	evdev_mark_dead(evdev);
	evdev_hangup(evdev);
	evdev_remove_chrdev(evdev);

	/* evdev is marked dead so no one else accesses evdev->open */
	if (evdev->open) {
		input_flush_device(handle, NULL);
		input_close_device(handle);
	}
}

/*
 * Create new evdev device. Note that input core serializes calls
 * to connect and disconnect so we don't need to lock evdev_table here.
 */
static int evdev_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct evdev *evdev;
	int minor;
	int error;

	for (minor = 0; minor < EVDEV_MINORS; minor++)
		if (!evdev_table[minor])
			break;

	if (minor == EVDEV_MINORS) {
		printk(KERN_ERR "evdev: no more free evdev devices\n");
		return -ENFILE;
	}

	evdev = kzalloc(sizeof(struct evdev), GFP_KERNEL);
	if (!evdev)
		return -ENOMEM;

	INIT_LIST_HEAD(&evdev->client_list);
	spin_lock_init(&evdev->client_lock);
	mutex_init(&evdev->mutex);
	init_waitqueue_head(&evdev->wait);

	snprintf(evdev->name, sizeof(evdev->name), "event%d", minor);
	evdev->exist = 1;
	evdev->minor = minor;

	evdev->handle.dev = dev;
	evdev->handle.name = evdev->name;
	evdev->handle.handler = handler;
	evdev->handle.private = evdev;

	strlcpy(evdev->dev.bus_id, evdev->name, sizeof(evdev->dev.bus_id));
	evdev->dev.devt = MKDEV(INPUT_MAJOR, EVDEV_MINOR_BASE + minor);
	evdev->dev.class = &input_class;
	evdev->dev.parent = &dev->dev;
	evdev->dev.release = evdev_free;
	device_initialize(&evdev->dev);

	error = input_register_handle(&evdev->handle);
	if (error)
		goto err_free_evdev;

	error = evdev_install_chrdev(evdev);
	if (error)
		goto err_unregister_handle;

	error = device_add(&evdev->dev);
	if (error)
		goto err_cleanup_evdev;

	return 0;

 err_cleanup_evdev:
	evdev_cleanup(evdev);
 err_unregister_handle:
	input_unregister_handle(&evdev->handle);
 err_free_evdev:
	put_device(&evdev->dev);
	return error;
}

static void evdev_disconnect(struct input_handle *handle)
{
	struct evdev *evdev = handle->private;

	device_del(&evdev->dev);
	evdev_cleanup(evdev);
	input_unregister_handle(handle);
	put_device(&evdev->dev);
}

static const struct input_device_id evdev_ids[] = {
	{ .driver_info = 1 },	/* Matches all devices */
	{ },			/* Terminating zero entry */
};

MODULE_DEVICE_TABLE(input, evdev_ids);

static struct input_handler evdev_handler = {
	.event		= evdev_event,
	.connect	= evdev_connect,
	.disconnect	= evdev_disconnect,
	.fops		= &evdev_fops,
	.minor		= EVDEV_MINOR_BASE,
	.name		= "evdev",
	.id_table	= evdev_ids,
};

static int __init evdev_init(void)
{
	return input_register_handler(&evdev_handler);
}

static void __exit evdev_exit(void)
{
	input_unregister_handler(&evdev_handler);
}

module_init(evdev_init);
module_exit(evdev_exit);

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Input driver event char devices");
MODULE_LICENSE("GPL");
