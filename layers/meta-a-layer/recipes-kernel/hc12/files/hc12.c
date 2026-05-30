// SPDX-License-Identifier: GPL-2.0-only
/*
 * HC-12 433 MHz wireless UART module driver.
 *
 * The HC-12 is a transparent UART-to-RF bridge. Bytes written to its
 * RX pin are transmitted over the air; bytes received over the air
 * come out of its TX pin. A SET pin (active low) switches the module
 * into AT-command mode for configuration.
 *
 * Datasheet timing:
 *   SET low  -> wait >=40 ms before sending AT
 *   SET high -> wait >=80 ms before resuming data traffic
 *
 * ============================================================
 *  READING LIST (top to bottom, in order of relevance)
 * ============================================================
 *
 *  serdev framework (kernel-owned UART peripherals):
 *      Documentation/driver-api/serial/index.rst
 *      include/linux/serdev.h
 *      https://elixir.bootlin.com/linux/latest/source/include/linux/serdev.h
 *
 *  GPIO consumer API (we ask for the SET line):
 *      Documentation/driver-api/gpio/consumer.rst
 *
 *  Device-managed (`devm_*`) resource API
 *  (auto-free on probe failure / driver detach):
 *      Documentation/driver-api/driver-model/devres.rst
 *
 *  Misc character devices (the /dev/hc12 node):
 *      include/linux/miscdevice.h
 *      Documentation/admin-guide/devices.rst
 *
 *  kfifo (lock-light circular buffer used for RX):
 *      Documentation/core-api/circular-buffers.rst
 *      include/linux/kfifo.h
 *      https://lwn.net/Articles/347619/
 *
 *  sysfs attributes (the /sys/class/misc/hc12/... knobs):
 *      Documentation/filesystems/sysfs.rst
 *      include/linux/sysfs.h
 *
 *  Completion / wait-queues / mutexes / spinlocks:
 *      Documentation/scheduler/completion.rst
 *      Documentation/core-api/wait-queue.rst
 *      Documentation/locking/mutex-design.rst
 *      Documentation/locking/spinlocks.rst
 *
 *  Device tree from a driver's perspective:
 *      Documentation/devicetree/usage-model.rst
 *      include/linux/of.h
 *
 *  General kernel-newcomer intro:
 *      https://sysprog21.github.io/lkmpg/ (Linux Kernel Module Programming Guide)
 *      Documentation/kernel-hacking/hacking.rst
 */

/*
 * --- INCLUDES ---
 *
 * Kernel code includes headers that declare the APIs it uses; there
 * is no equivalent of userspace's libc and you cannot include
 * <stdio.h>, <string.h>, <stdlib.h>, etc. The kernel provides its
 * own equivalents under <linux/...h>.
 *
 * Each include below is paired with a short note on what it brings
 * in. Look up `git grep -nw <symbol> include/` in a kernel tree if
 * you're ever unsure which header to add.
 */
#include <linux/completion.h>   /* struct completion, wait_for_completion_*  */
#include <linux/delay.h>        /* msleep(), udelay()                        */
#include <linux/device.h>       /* struct device, dev_info/dev_err, sysfs    */
#include <linux/fs.h>           /* struct file_operations                    */
#include <linux/gpio/consumer.h>/* gpiod_*, devm_gpiod_get                   */
#include <linux/kfifo.h>        /* DECLARE_KFIFO_PTR style ring buffer       */
#include <linux/miscdevice.h>   /* struct miscdevice, misc_register          */
#include <linux/module.h>       /* MODULE_LICENSE, module_init/_exit         */
#include <linux/mutex.h>        /* struct mutex, mutex_lock/_unlock          */
#include <linux/of.h>           /* of_property_read_*, of_device_id          */
#include <linux/poll.h>         /* poll_table, EPOLL* flags                  */
#include <linux/serdev.h>       /* serdev_device, serdev_device_driver       */
#include <linux/slab.h>         /* kmalloc/kzalloc/kfree, memdup_user        */
#include <linux/uaccess.h>      /* copy_*_user (used indirectly via kfifo)   */
#include <linux/wait.h>         /* wait_queue_head_t, wait_event_*           */

/*
 * --- CONSTANTS ---
 *
 * #define is the kernel's preferred way to express compile-time
 * scalar constants. (Plain `const int` works too but #define avoids
 * a small bss-allocated variable and matches surrounding code.)
 */
#define HC12_RX_FIFO_SIZE	4096	/* bytes buffered between RF and read() */
#define HC12_AT_RESP_SIZE	128	/* "OK+FU3 \r\n OK+B9600 \r\n ..." fits  */
#define HC12_AT_TIMEOUT_MS	200	/* round-trip + module turnaround budget */
#define HC12_SET_LOW_DELAY_MS	45	/* datasheet: >=40 after SET=low         */
#define HC12_SET_HIGH_DELAY_MS	85	/* datasheet: >=80 after SET=high        */

/*
 * --- DRIVER STATE STRUCT ---
 *
 * One `struct hc12` is allocated per probed HC-12 device. In a typical
 * embedded setup there is only ever one, but the kernel driver model
 * is multi-instance by default and you should write code as if there
 * could be many. The struct is allocated with `devm_kzalloc()` in
 * probe (see below) and freed automatically when the device goes
 * away.
 *
 * Idiom to recognize: a struct full of pointers and embedded objects.
 * The embedded objects (e.g. `miscdev`, `at_lock`) are owned by us;
 * the pointers (`serdev`, `set_gpio`, `class_dev`) point to objects
 * owned elsewhere whose lifetimes we coordinate with.
 *
 * Embedded vs. pointer-to is a deliberate design choice that recurs
 * everywhere in kernel code. See Documentation/process/coding-style.rst
 * and the LWN article "container_of" linked next to the macro use.
 */
struct hc12 {
	struct serdev_device	*serdev;	/* the UART port we are bound to */
	struct gpio_desc	*set_gpio;	/* opaque handle for the SET line */
	struct miscdevice	miscdev;	/* registers /dev/hc12 + carries sysfs groups */

	/* RX path: serdev callback -> kfifo -> /dev/hc12 read */
	struct kfifo		rx_fifo;	/* ring buffer of RX'd bytes        */
	spinlock_t		rx_lock;	/* short, irq-safe critical section */
	wait_queue_head_t	rx_wait;	/* threads blocked in read() wake here */

	/* AT command serialization and response capture */
	struct mutex		at_lock;	/* one AT round-trip at a time     */
	bool			at_mode;	/* true while SET is asserted      */
	char			at_resp[HC12_AT_RESP_SIZE];
	size_t			at_resp_len;
	struct completion	at_done;	/* receive_buf -> AT caller wakeup */

	/* Last-applied config, cached for sysfs `show` callbacks. We
	 * could re-query the module with AT each time, but caching is
	 * faster and avoids toggling SET on every `cat`.
	 */
	u32			channel;
	u32			baud;
	char			mode[8];
	u32			power;
};

/* ======================================================================
 * serdev RX callback
 *
 * Anatomy of the serdev (serial device) framework:
 *   - A `struct serdev_device` represents a serial port (UART) that
 *     the kernel "owns", i.e. it is NOT exposed as /dev/ttyAMA*.
 *     The DT node `&uart3 { hc12 { compatible = "hc01,hc-12"; }; }`
 *     gives our driver exclusive access to the port.
 *   - We register a `serdev_device_driver` (bottom of this file).
 *     When the DT match table fires, the core calls our .probe().
 *   - The core invokes our `receive_buf` callback whenever data
 *     arrives on the line. That callback runs in interrupt/softirq
 *     context, so it MUST NOT sleep or block.
 *
 * See drivers/tty/serdev/core.c for the implementation, and
 * Documentation/driver-api/serial/serdev.rst.
 * ====================================================================== */

static size_t hc12_receive_buf(struct serdev_device *serdev,
			       const u8 *buf, size_t count)
{
	/*
	 * `serdev_device_get_drvdata()` retrieves the per-device cookie
	 * we stored via `serdev_device_set_drvdata()` in probe. This is
	 * the standard kernel pattern for "find my own struct given a
	 * framework-supplied object pointer".
	 */
	struct hc12 *hc = serdev_device_get_drvdata(serdev);
	size_t copied;

	/*
	 * READ_ONCE/WRITE_ONCE force the compiler to actually emit a
	 * load/store at this point (no caching in a register, no tearing
	 * across multiple instructions for word-sized values). They are
	 * used whenever a value is read without locking from a context
	 * where another CPU/thread might be writing it.
	 * See Documentation/memory-barriers.txt.
	 */
	if (READ_ONCE(hc->at_mode)) {
		size_t space = HC12_AT_RESP_SIZE - 1 - hc->at_resp_len;
		size_t take = min(count, space);

		if (take) {
			memcpy(hc->at_resp + hc->at_resp_len, buf, take);
			hc->at_resp_len += take;
			hc->at_resp[hc->at_resp_len] = '\0';
		}
		/*
		 * `complete()` wakes any thread blocked in
		 * `wait_for_completion_timeout()` on the same completion.
		 * It is safe to call repeatedly; the second call just
		 * signals an already-completed state. The AT caller will
		 * keep accumulating bytes via this function until its
		 * timeout elapses.
		 */
		complete(&hc->at_done);
		return count;
	}

	/*
	 * Normal data path: push the received bytes into the kfifo and
	 * wake anyone blocked in read(). kfifo is a single-producer /
	 * single-consumer lock-free ring buffer; we still take a
	 * spinlock here because serdev MAY (depending on the underlying
	 * tty driver) call receive_buf from different CPUs.
	 *
	 * Why spinlock and not mutex? receive_buf runs in
	 * interrupt/softirq context where sleeping is forbidden, and
	 * mutexes can sleep. Spinlocks busy-wait, which is the right
	 * primitive here.
	 */
	spin_lock(&hc->rx_lock);
	copied = kfifo_in(&hc->rx_fifo, buf, count);
	spin_unlock(&hc->rx_lock);
	if (copied)
		wake_up_interruptible(&hc->rx_wait);

	/*
	 * Returning `count` tells serdev "I consumed all of it". If
	 * the FIFO was full we silently dropped bytes (`copied < count`)
	 * rather than back-pressuring the serial layer. For an RF link
	 * with no flow control, dropping is the only sane option.
	 */
	return count;
}

/*
 * The `ops` table the serdev core looks at when delivering events to
 * our driver. .write_wakeup is the standard serdev helper used when
 * a write buffer has space again — we don't need anything custom.
 */
static const struct serdev_device_ops hc12_serdev_ops = {
	.receive_buf	= hc12_receive_buf,
	.write_wakeup	= serdev_device_write_wakeup,
};

/* ======================================================================
 * AT command helper
 *
 * The flow is:
 *   1. Take the AT mutex (so only one AT round-trip happens at a time
 *      AND no userspace write() injects data while SET is low).
 *   2. Assert SET (active-low line goes low) + wait 45 ms per datasheet.
 *   3. Mark at_mode=true so receive_buf routes bytes into at_resp[].
 *   4. Send the AT command and wait up to HC12_AT_TIMEOUT_MS for a
 *      response. complete()/wait_for_completion_timeout() is the
 *      classic kernel signalling pattern between two contexts.
 *   5. Settle 30 ms more so trailing bytes from the module finish
 *      landing in at_resp[].
 *   6. Deassert SET, wait 85 ms, release the mutex.
 *
 * See Documentation/scheduler/completion.rst for completion semantics.
 * ====================================================================== */

static int hc12_send_at(struct hc12 *hc, const char *cmd,
			char *resp, size_t resp_size)
{
	int ret;
	size_t n;

	mutex_lock(&hc->at_lock);

	/*
	 * gpiod_set_value_cansleep() is the variant of gpiod_set_value()
	 * that may sleep — required for GPIOs whose backing controller
	 * uses an I2C/SPI bus. It's harmless for fast (MMIO) GPIOs.
	 * Convention: in code that might run on either kind of GPIO,
	 * use the _cansleep variant when you're already in sleepable
	 * context. See Documentation/driver-api/gpio/consumer.rst, section
	 * "The active low and open drain semantics".
	 *
	 * Note on logical levels: we declared the GPIO as GPIO_ACTIVE_LOW
	 * in the device tree, so the gpiod API inverts the wire level
	 * for us. gpiod_set_value(.., 1) means "asserted", which on the
	 * wire is electrically low. The driver code reads naturally
	 * regardless of polarity.
	 */
	gpiod_set_value_cansleep(hc->set_gpio, 1);	/* SET asserted -> line low -> AT mode */
	msleep(HC12_SET_LOW_DELAY_MS);

	/*
	 * reinit_completion() resets the completion to its un-signalled
	 * state. Without this, a stale complete() from a previous AT
	 * round would short-circuit our wait below.
	 */
	reinit_completion(&hc->at_done);
	hc->at_resp_len = 0;
	hc->at_resp[0] = '\0';
	WRITE_ONCE(hc->at_mode, true);

	/*
	 * serdev_device_write() blocks until either all bytes are queued
	 * to the UART or the timeout expires. We pass a small timeout
	 * because the write itself is fast; the longer wait is for the
	 * RESPONSE (handled below by wait_for_completion_timeout).
	 */
	ret = serdev_device_write(hc->serdev, cmd, strlen(cmd),
				  msecs_to_jiffies(HC12_AT_TIMEOUT_MS));
	if (ret < 0) {
		dev_err(&hc->serdev->dev, "AT write failed: %d\n", ret);
		goto out;
	}

	/*
	 * Wait for receive_buf() to signal at_done. The completion
	 * fires as soon as the FIRST byte arrives; we then sleep
	 * another short window (msleep(30)) so additional bytes have
	 * time to land. This is a simple, robust framing strategy for
	 * a chatty UART peer that emits multi-byte responses without
	 * a clear delimiter.
	 *
	 * msecs_to_jiffies() converts a real-world millisecond value
	 * into the kernel's internal time unit ("jiffies"), which is
	 * what most timing APIs accept.
	 */
	wait_for_completion_timeout(&hc->at_done,
				    msecs_to_jiffies(HC12_AT_TIMEOUT_MS));
	msleep(30);

	WRITE_ONCE(hc->at_mode, false);

	n = hc->at_resp_len;
	if (resp && resp_size) {
		size_t take = min(n, resp_size - 1);

		memcpy(resp, hc->at_resp, take);
		resp[take] = '\0';
	}

	if (n == 0) {
		dev_warn(&hc->serdev->dev, "AT '%s' got no response\n", cmd);
		ret = -ETIMEDOUT;
	} else {
		/*
		 * dev_dbg() prints only when DEBUG is defined for this
		 * file or dynamic debug is enabled at runtime via
		 * /sys/kernel/debug/dynamic_debug/control. See
		 * Documentation/admin-guide/dynamic-debug-howto.rst.
		 */
		dev_dbg(&hc->serdev->dev, "AT '%s' -> '%s'\n", cmd, hc->at_resp);
		ret = n;
	}

out:
	/* Leave AT mode */
	gpiod_set_value_cansleep(hc->set_gpio, 0);
	msleep(HC12_SET_HIGH_DELAY_MS);

	mutex_unlock(&hc->at_lock);
	return ret;
}

/* ======================================================================
 * Character device (/dev/hc12)
 *
 * A "character device" is the kernel's name for a stream-oriented
 * file like /dev/ttyS0 or /dev/random. We use the `miscdevice`
 * framework because it auto-assigns a minor number and keeps the
 * setup short. See include/linux/miscdevice.h.
 *
 * Userspace programs interact with us through:
 *   write(fd, buf, n) -> hc12_chr_write -> serdev_device_write -> RF TX
 *   read(fd, buf, n)  -> hc12_chr_read  -> kfifo_to_user        -> RX
 *   poll(fds, ...)    -> hc12_chr_poll  -> select/epoll readiness
 *
 * The file_operations table at the bottom of this section wires the
 * userspace syscalls to these functions.
 * ====================================================================== */

/*
 * Classic kernel idiom: given a `struct file *`, recover our driver
 * struct. We registered the miscdevice with `f->private_data` set
 * to its `struct miscdevice *`; container_of() does pointer
 * arithmetic to get from the embedded miscdevice back to its
 * containing hc12.
 *
 * container_of() is THE essential kernel macro. Read once and you'll
 * see it everywhere:
 *   https://www.kernel.org/doc/html/latest/kernel-hacking/hacking.html#container-of
 *   include/linux/container_of.h
 */
static struct hc12 *hc12_from_file(struct file *f)
{
	struct miscdevice *m = f->private_data;

	return container_of(m, struct hc12, miscdev);
}

static ssize_t hc12_chr_read(struct file *f, char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	struct hc12 *hc = hc12_from_file(f);
	unsigned int copied = 0;
	int ret;

	/*
	 * The `__user` annotation on a pointer is a sparse marker that
	 * says "this pointer is a userspace address; do not deref it
	 * directly". Use copy_*_user / kfifo_to_user / etc. instead.
	 * See Documentation/process/coding-style.rst and the sparse(1)
	 * static analysis tool.
	 */

	if (kfifo_is_empty(&hc->rx_fifo)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/*
		 * Sleep until the FIFO has data. wait_event_interruptible()
		 * puts the task on the wait queue, marks it INTERRUPTIBLE
		 * (so a signal will wake it), and yields the CPU. When
		 * receive_buf() calls wake_up_interruptible() it re-evaluates
		 * the condition; if true, we return 0; if a signal arrived,
		 * we return -ERESTARTSYS (negative -> error path).
		 * See Documentation/core-api/wait-queue.rst.
		 */
		ret = wait_event_interruptible(hc->rx_wait,
					       !kfifo_is_empty(&hc->rx_fifo));
		if (ret)
			return ret;
	}

	/*
	 * kfifo_to_user() drains the FIFO directly into userspace,
	 * handling the user-pointer access for us. `copied` receives
	 * the number of bytes successfully transferred.
	 */
	ret = kfifo_to_user(&hc->rx_fifo, ubuf, len, &copied);
	return ret ? ret : copied;
}

static ssize_t hc12_chr_write(struct file *f, const char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct hc12 *hc = hc12_from_file(f);
	char *kbuf;
	int ret;

	if (len == 0)
		return 0;
	if (len > PAGE_SIZE)
		len = PAGE_SIZE;

	/*
	 * memdup_user() = kmalloc(len) + copy_from_user(); a single
	 * helper for the common "I need a kernel-side copy of a chunk
	 * of userspace data" pattern. Returns ERR_PTR on failure;
	 * IS_ERR()/PTR_ERR() are the kernel idiom for unwrapping such
	 * "pointer or error" returns.
	 */
	kbuf = memdup_user(ubuf, len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	/*
	 * We hold the AT mutex around the actual write so we cannot be
	 * mid-write when an AT command starts (which would inject user
	 * data into AT mode). The mutex is uncontended in the common
	 * case so this is essentially free.
	 */
	mutex_lock(&hc->at_lock);
	ret = serdev_device_write(hc->serdev, kbuf, len, msecs_to_jiffies(1000));
	mutex_unlock(&hc->at_lock);

	kfree(kbuf);
	return ret < 0 ? ret : len;
}

/*
 * poll() implementation. select/poll/epoll all call this. Returns
 * a bitmask of EPOLL* flags indicating which events are ready.
 *
 * The poll_wait() call doesn't actually wait; it registers our wait
 * queue with the poll subsystem so the caller can be woken when we
 * later call wake_up_interruptible() on that queue.
 * See Documentation/filesystems/locking.rst#poll.
 */
static __poll_t hc12_chr_poll(struct file *f, poll_table *wait)
{
	struct hc12 *hc = hc12_from_file(f);
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;	/* write is always ready */

	poll_wait(f, &hc->rx_wait, wait);
	if (!kfifo_is_empty(&hc->rx_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

/*
 * file_operations: the table of "what to do when userspace invokes
 * each syscall on this file". `.owner = THIS_MODULE` ensures the
 * module's refcount is bumped while files are open so we can't be
 * unloaded out from under an open fd. See include/linux/fs.h.
 */
static const struct file_operations hc12_fops = {
	.owner	= THIS_MODULE,
	.read	= hc12_chr_read,
	.write	= hc12_chr_write,
	.poll	= hc12_chr_poll,
	/* No .llseek — the default is "not supported", which is what we want
	 * for a byte stream. (Pre-6.12 kernels used .llseek = no_llseek; that
	 * helper was removed once it became the implicit default.)
	 */
};

/* ======================================================================
 * sysfs attributes
 *
 * Every sysfs file is backed by a `show` function (for `cat`) and
 * optionally a `store` function (for `echo ... >`). The DEVICE_ATTR_*
 * macros wire those callbacks to a `struct device_attribute`. The
 * `attribute_group` collects them so they can be created/destroyed
 * atomically as a set. See Documentation/filesystems/sysfs.rst.
 *
 * Naming convention: a function named `foo_show` matched with
 * DEVICE_ATTR_RO(foo) creates a file called `foo` in sysfs. The
 * macro magic relies on this convention.
 *
 * sysfs_emit() is the recommended way to format a sysfs read; it
 * writes to a PAGE_SIZE buffer and returns the byte count. Older
 * code uses sprintf() — both work but sysfs_emit() is bounds-checked.
 * ====================================================================== */

static ssize_t channel_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct hc12 *hc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", hc->channel);
}

static ssize_t channel_store(struct device *dev, struct device_attribute *a,
			     const char *buf, size_t len)
{
	struct hc12 *hc = dev_get_drvdata(dev);
	char cmd[16];
	u32 v;
	int ret;

	/*
	 * kstrtou32() is the kernel's safe equivalent of strtoul();
	 * returns 0 on success, a negative errno on parse failure.
	 * See lib/kstrtox.c.
	 */
	if (kstrtou32(buf, 10, &v) || v < 1 || v > 127)
		return -EINVAL;

	scnprintf(cmd, sizeof(cmd), "AT+C%03u", v);
	ret = hc12_send_at(hc, cmd, NULL, 0);
	if (ret < 0)
		return ret;
	hc->channel = v;
	/*
	 * sysfs store callbacks return the number of bytes consumed
	 * from the input buffer; returning `len` means "I accepted
	 * everything you wrote".
	 */
	return len;
}
/* DEVICE_ATTR_RW(name) -> struct device_attribute dev_attr_name with
 * mode 0644 and pointers to name_show/name_store. See
 * include/linux/device.h. */
static DEVICE_ATTR_RW(channel);

static ssize_t baud_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct hc12 *hc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", hc->baud);
}

static ssize_t baud_store(struct device *dev, struct device_attribute *a,
			  const char *buf, size_t len)
{
	struct hc12 *hc = dev_get_drvdata(dev);
	static const u32 valid[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
	char cmd[16];
	u32 v;
	int i, ret, ok = 0;

	if (kstrtou32(buf, 10, &v))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(valid); i++)
		if (valid[i] == v) { ok = 1; break; }
	if (!ok)
		return -EINVAL;

	scnprintf(cmd, sizeof(cmd), "AT+B%u", v);
	ret = hc12_send_at(hc, cmd, NULL, 0);
	if (ret < 0)
		return ret;

	/*
	 * The HC-12 just acknowledged the baud change; immediately
	 * retune our side of the UART to match so subsequent traffic
	 * is interpretable.
	 */
	serdev_device_set_baudrate(hc->serdev, v);
	hc->baud = v;
	return len;
}
static DEVICE_ATTR_RW(baud);

static ssize_t mode_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct hc12 *hc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", hc->mode);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *a,
			  const char *buf, size_t len)
{
	struct hc12 *hc = dev_get_drvdata(dev);
	char cmd[16];
	int n, ret;

	/* Accept "FU1".."FU4" with optional trailing whitespace */
	if (len < 3 || buf[0] != 'F' || buf[1] != 'U')
		return -EINVAL;
	n = buf[2] - '0';
	if (n < 1 || n > 4)
		return -EINVAL;

	scnprintf(cmd, sizeof(cmd), "AT+FU%d", n);
	ret = hc12_send_at(hc, cmd, NULL, 0);
	if (ret < 0)
		return ret;
	scnprintf(hc->mode, sizeof(hc->mode), "FU%d", n);
	return len;
}
static DEVICE_ATTR_RW(mode);

static ssize_t tx_power_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct hc12 *hc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", hc->power);
}

static ssize_t tx_power_store(struct device *dev, struct device_attribute *a,
			   const char *buf, size_t len)
{
	struct hc12 *hc = dev_get_drvdata(dev);
	char cmd[16];
	u32 v;
	int ret;

	if (kstrtou32(buf, 10, &v) || v < 1 || v > 8)
		return -EINVAL;

	scnprintf(cmd, sizeof(cmd), "AT+P%u", v);
	ret = hc12_send_at(hc, cmd, NULL, 0);
	if (ret < 0)
		return ret;
	hc->power = v;
	return len;
}
/* Named tx_power, not power: every kernel device gets an auto-created
 * power/ directory for runtime PM, so a plain `power` attribute collides. */
static DEVICE_ATTR_RW(tx_power);

static ssize_t version_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct hc12 *hc = dev_get_drvdata(dev);
	char resp[HC12_AT_RESP_SIZE];
	int ret;

	/* version is read-only and queried live from the module each time */
	ret = hc12_send_at(hc, "AT+V", resp, sizeof(resp));
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%s\n", resp);
}
static DEVICE_ATTR_RO(version);

/*
 * Group the attributes so they can be created in one shot. We hand
 * `hc12_groups` to the miscdevice via its `.groups` field; misc_register()
 * then materializes the files under /sys/class/misc/hc12/.
 *
 * Conceptually: groups -> attribute_group -> array of `struct
 * attribute *` -> each one is a `dev_attr_FOO.attr` produced by
 * DEVICE_ATTR_RW/RO. ATTRIBUTE_GROUPS(hc12) is a macro that wraps the
 * `hc12_attrs[]` array below into the boilerplate `hc12_group` and
 * `hc12_groups` symbols expected by the framework.
 */
static struct attribute *hc12_attrs[] = {
	&dev_attr_channel.attr,
	&dev_attr_baud.attr,
	&dev_attr_mode.attr,
	&dev_attr_tx_power.attr,
	&dev_attr_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hc12);

/* ======================================================================
 * probe / remove
 *
 * `probe` runs when the serdev core finds a UART node whose child
 * has a compatible string matching our `of_device_id` table. It is
 * the moment to allocate state, register interfaces, and bring the
 * hardware up. Returning 0 means "this device is mine and it works";
 * returning negative means "decline" or "error" and the core will
 * tear down anything we partially set up.
 *
 * `remove` is the inverse: tear down in reverse order of probe.
 *
 * The `devm_*` family of allocators ties the lifetime of the
 * allocation to the device. When the device is destroyed (or probe
 * fails), `devm_*` resources are freed automatically — no need for
 * manual cleanup in error paths. See
 * Documentation/driver-api/driver-model/devres.rst.
 * ====================================================================== */

/*
 * Apply DT-provided defaults via AT commands. Called from probe.
 * Failures are warnings, not errors — if the HC-12 is unplugged
 * we still want the driver to load so userspace can inspect /sys.
 */
static int hc12_apply_defaults_from_dt(struct hc12 *hc)
{
	struct device *dev = &hc->serdev->dev;
	const char *mode_str;
	char cmd[16];
	u32 v;
	int ret;

	/*
	 * of_property_read_u32() returns 0 on success, -EINVAL if the
	 * property is missing or wrong type. The classic "if not error"
	 * idiom: "if (!of_property_read_u32(np, name, &v)) { use v; }".
	 * See Documentation/devicetree/usage-model.rst.
	 */
	if (!of_property_read_u32(dev->of_node, "hc01,baud-rate", &v)) {
		scnprintf(cmd, sizeof(cmd), "AT+B%u", v);
		ret = hc12_send_at(hc, cmd, NULL, 0);
		if (ret < 0)
			dev_warn(dev, "default baud=%u not applied: %d\n", v, ret);
		else
			serdev_device_set_baudrate(hc->serdev, v);
		hc->baud = v;
	}

	if (!of_property_read_u32(dev->of_node, "hc01,channel", &v) &&
	    v >= 1 && v <= 127) {
		scnprintf(cmd, sizeof(cmd), "AT+C%03u", v);
		ret = hc12_send_at(hc, cmd, NULL, 0);
		if (ret < 0)
			dev_warn(dev, "default channel=%u not applied: %d\n", v, ret);
		hc->channel = v;
	}

	if (!of_property_read_string(dev->of_node, "hc01,mode", &mode_str) &&
	    strlen(mode_str) == 3 && !strncmp(mode_str, "FU", 2) &&
	    mode_str[2] >= '1' && mode_str[2] <= '4') {
		scnprintf(cmd, sizeof(cmd), "AT+FU%c", mode_str[2]);
		ret = hc12_send_at(hc, cmd, NULL, 0);
		if (ret < 0)
			dev_warn(dev, "default mode=%s not applied: %d\n", mode_str, ret);
		scnprintf(hc->mode, sizeof(hc->mode), "%s", mode_str);
	}

	if (!of_property_read_u32(dev->of_node, "hc01,tx-power", &v) &&
	    v >= 1 && v <= 8) {
		scnprintf(cmd, sizeof(cmd), "AT+P%u", v);
		ret = hc12_send_at(hc, cmd, NULL, 0);
		if (ret < 0)
			dev_warn(dev, "default power=%u not applied: %d\n", v, ret);
		hc->power = v;
	}

	return 0;
}

static int hc12_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct hc12 *hc;
	u32 speed = 9600;
	int ret;

	/*
	 * devm_kzalloc(): like kzalloc() but the allocation is tied to
	 * `dev`. When the device is removed (or probe fails) the kernel
	 * frees it automatically. This is the preferred pattern for
	 * per-device state.
	 */
	hc = devm_kzalloc(dev, sizeof(*hc), GFP_KERNEL);
	if (!hc)
		return -ENOMEM;

	hc->serdev = serdev;
	/* Set safe initial values; they may be overwritten by DT below. */
	hc->channel = 1;
	hc->baud = 9600;
	hc->power = 8;
	scnprintf(hc->mode, sizeof(hc->mode), "FU3");

	/*
	 * Initialize the embedded sync primitives. These macros prepare
	 * the internal state so the first lock/wake operation works
	 * correctly. Failure to call these init helpers is a common
	 * subtle bug — see lockdep warnings in Documentation/locking/
	 * lockdep-design.rst.
	 */
	spin_lock_init(&hc->rx_lock);
	mutex_init(&hc->at_lock);
	init_waitqueue_head(&hc->rx_wait);
	init_completion(&hc->at_done);

	/*
	 * kfifo_alloc() allocates the backing buffer. The size is
	 * rounded up to a power of two internally; pass our chosen size
	 * directly. Free with kfifo_free() in remove() or the error
	 * path. (There is also DECLARE_KFIFO + INIT_KFIFO for
	 * stack/static buffers if you'd rather avoid the heap.)
	 */
	ret = kfifo_alloc(&hc->rx_fifo, HC12_RX_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		return ret;

	/*
	 * Acquire the SET GPIO. The "set" name comes from the DT
	 * property `set-gpios = <...>` — devm_gpiod_get() looks for
	 * "<name>-gpios" first, then "<name>-gpio". GPIOD_OUT_LOW
	 * means "configure as output, deasserted" (i.e. line high
	 * given the ACTIVE_LOW polarity we set in DT -> transparent
	 * data mode is the safe default at power-on).
	 *
	 * The HC-12 ignores SET while booting; the line being high at
	 * probe time means it'll come up in transparent mode. We
	 * temporarily flip it low only when sending an AT command.
	 */
	hc->set_gpio = devm_gpiod_get(dev, "set", GPIOD_OUT_LOW);
	if (IS_ERR(hc->set_gpio)) {
		ret = PTR_ERR(hc->set_gpio);
		dev_err(dev, "failed to get set-gpio: %d\n", ret);
		goto err_fifo;
	}

	/*
	 * Tell serdev who we are and what to call. The drvdata cookie
	 * is how receive_buf() finds our struct hc12.
	 */
	serdev_device_set_drvdata(serdev, hc);
	serdev_device_set_client_ops(serdev, &hc12_serdev_ops);

	/*
	 * serdev_device_open() actually brings the UART online (powers
	 * the port, configures interrupts, registers the receive path).
	 * Until this returns successfully, you cannot read or write.
	 */
	ret = serdev_device_open(serdev);
	if (ret) {
		dev_err(dev, "serdev open failed: %d\n", ret);
		goto err_fifo;
	}

	/*
	 * Pull the wire parameters from DT (with a 9600 default if the
	 * `current-speed` property is absent), then apply them. This
	 * must happen AFTER serdev_device_open() — the port has to be
	 * up before you can configure it.
	 */
	of_property_read_u32(dev->of_node, "current-speed", &speed);
	serdev_device_set_baudrate(serdev, speed);
	serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	serdev_device_set_flow_control(serdev, false);

	/*
	 * Register /dev/hc12. MISC_DYNAMIC_MINOR asks the misc subsystem
	 * to pick an unused minor number for us. `parent = dev` ties
	 * the misc device into the device tree (so it shows up under
	 * its parent serdev device in /sys/devices/).
	 *
	 * `.groups` makes misc_register() create our sysfs attribute
	 * files at /sys/class/misc/hc12/{channel,baud,...} as part of
	 * the same call. We don't create a separate standalone class
	 * device: that would try to add `<parent>/hc12` a second time
	 * in the sysfs hierarchy and collide with the miscdevice.
	 */
	hc->miscdev.minor = MISC_DYNAMIC_MINOR;
	hc->miscdev.name  = "hc12";
	hc->miscdev.fops  = &hc12_fops;
	hc->miscdev.parent = dev;
	hc->miscdev.groups = hc12_groups;
	ret = misc_register(&hc->miscdev);
	if (ret) {
		dev_err(dev, "misc_register failed: %d\n", ret);
		goto err_close;
	}

	/*
	 * misc_register() created `this_device` but didn't set drvdata
	 * on it. Our sysfs show/store callbacks call dev_get_drvdata(dev)
	 * to recover the hc12 struct, so wire it up before any of those
	 * could fire (e.g. a `cat` racing us).
	 */
	dev_set_drvdata(hc->miscdev.this_device, hc);

	/* Hardware is up; push the DT-default config to it via AT. */
	hc12_apply_defaults_from_dt(hc);

	/*
	 * dev_info() is the standard "I came up successfully" log line.
	 * It prefixes the message with the device's path automatically
	 * — much nicer than bare printk(). See
	 * Documentation/core-api/printk-basics.rst.
	 */
	dev_info(dev, "HC-12 ready: channel=%u baud=%u mode=%s power=%u\n",
		 hc->channel, hc->baud, hc->mode, hc->power);
	return 0;

/*
 * --- Error unwind ---
 *
 * "goto err_label" is the canonical kernel pattern for error
 * cleanup. It avoids deep nesting and keeps each cleanup step
 * with the resource it's tearing down. devm_* resources don't
 * appear here — they unwind automatically on probe failure.
 * See Documentation/process/coding-style.rst section 7.
 */
err_close:
	serdev_device_close(serdev);
err_fifo:
	kfifo_free(&hc->rx_fifo);
	return ret;
}

static void hc12_remove(struct serdev_device *serdev)
{
	struct hc12 *hc = serdev_device_get_drvdata(serdev);

	/* Tear down in reverse of probe. devm_* and devm-allocated
	 * memory are freed automatically after this function returns. */
	misc_deregister(&hc->miscdev);
	serdev_device_close(serdev);
	kfifo_free(&hc->rx_fifo);
}

/* ======================================================================
 * Driver registration
 *
 * The "of_device_id" table tells the kernel which device tree
 * `compatible` strings this driver claims. When the DT contains
 * `compatible = "hc01,hc-12"` under a serdev-capable UART, the
 * core matches it to this table and calls our probe().
 *
 * MODULE_DEVICE_TABLE() exposes the table to userspace tools
 * (depmod, modaliases) so a hotplugged or DT-discovered device can
 * trigger automatic module loading.
 * See Documentation/driver-api/driver-model/platform.rst and
 * Documentation/devicetree/bindings/submitting-patches.rst.
 * ====================================================================== */

static const struct of_device_id hc12_of_match[] = {
	{ .compatible = "hc01,hc-12" },
	{ },	/* sentinel — the matcher walks until it sees an empty entry */
};
MODULE_DEVICE_TABLE(of, hc12_of_match);

static struct serdev_device_driver hc12_driver = {
	.probe		= hc12_probe,
	.remove		= hc12_remove,
	.driver = {
		.name		= "hc12",
		.of_match_table	= hc12_of_match,
	},
};

/* ======================================================================
 * Module init / exit
 *
 * module_init()/module_exit() register the functions called when the
 * module is loaded (insmod / modprobe) and unloaded (rmmod). They
 * are NOT called per-device — that's probe/remove. The init function
 * sets up module-global state (here: the sysfs class) and registers
 * the driver with its bus framework (here: serdev).
 *
 * See Documentation/driver-api/basics.rst and
 * https://sysprog21.github.io/lkmpg/#hello-world.
 * ====================================================================== */

static int __init hc12_init(void)
{
	return serdev_device_driver_register(&hc12_driver);
}

static void __exit hc12_exit(void)
{
	serdev_device_driver_unregister(&hc12_driver);
}

module_init(hc12_init);
module_exit(hc12_exit);

/*
 * MODULE_* metadata. Mandatory MODULE_LICENSE; the others are
 * informational but conventional. The license string determines
 * which symbols the module can use — GPL-only symbols (EXPORT_SYMBOL_GPL)
 * require a GPL-compatible license here.
 * See include/linux/module.h.
 */
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("a-layer");
MODULE_DESCRIPTION("HC-12 433 MHz wireless UART serdev driver");
MODULE_VERSION("0.1");
