/*
 * kmscon - udev input hotplug and evdev handling
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The main object kmscon_input discovers and monitors input devices, and
 * adds/removes them accordingly from the devices linked list.
 *
 * The udev monitor keeps running even while the object is in INPUT_ASLEEP.
 * We do this because we'll either lose track of the devices, or otherwise
 * have to re-scan the devices at every wakeup.
 *
 * The kmscon_input_device objects hold the file descriptors for their device
 * nodes. All events go through the input-object callback; there is currently
 * no "routing" or any differentiation between them. When the input is put to
 * sleep, all fd's are closed. When woken up, they are opened. There should be
 * not spurious events delivered. The initial state depends on the
 * kmscon_input's state.
 */

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "conf.h"
#include "eloop.h"
#include "input.h"
#include "kbd.h"
#include "log.h"
#include "misc.h"

#define LOG_SUBSYSTEM "input"

/* How many longs are needed to hold \n bits. */
#define NLONGS(n) (((n) + LONG_BIT - 1) / LONG_BIT)

enum input_state {
	INPUT_ASLEEP,
	INPUT_AWAKE,
};

/* See probe_device_features(). */
enum device_feature {
	FEATURE_HAS_KEYS = 0x01,
	FEATURE_HAS_LEDS = 0x02,
};

struct kmscon_input_device {
	struct kmscon_input_device *next;
	struct kmscon_input *input;

	unsigned int features;

	int rfd;
	char *devnode;
	struct ev_fd *fd;

	struct kmscon_kbd *kbd;
};

struct kmscon_input {
	size_t ref;
	enum input_state state;
	struct kmscon_input_device *devices;

	struct ev_eloop *eloop;
	struct kmscon_hook *hook;

	struct udev *udev;
	struct udev_monitor *monitor;
	struct ev_fd *monitor_fd;

	struct kmscon_kbd_desc *desc;
};

static void remove_device(struct kmscon_input *input, const char *node);

static void notify_key(struct kmscon_input_device *device,
				uint16_t type, uint16_t code, int32_t value)
{
	int ret;
	struct kmscon_input_event ev;
	struct kmscon_input *input;

	if (type != EV_KEY)
		return;

	input = device->input;
	ret = kmscon_kbd_process_key(device->kbd, value, code, &ev);

	if (ret && ret != -ENOKEY)
		return;

	if (ret != -ENOKEY)
		kmscon_hook_call(input->hook, input, &ev);
}

static void device_data_arrived(struct ev_fd *fd, int mask, void *data)
{
	int i;
	ssize_t len, n;
	struct kmscon_input_device *device = data;
	struct kmscon_input *input = device->input;
	struct input_event ev[16];

	len = sizeof(ev);
	while (len == sizeof(ev)) {
		len = read(device->rfd, &ev, sizeof(ev));
		if (len < 0) {
			if (errno == EWOULDBLOCK)
				break;

			log_warn("reading device %s failed %d",
						device->devnode, errno);
			remove_device(input, device->devnode);
		} else if (len == 0) {
			log_debug("EOF device %s", device->devnode);
			remove_device(input, device->devnode);
		} else if (len % sizeof(*ev)) {
			log_warn("read invalid input_event");
		} else {
			n = len / sizeof(*ev);
			for (i = 0; i < n; i++)
				notify_key(device, ev[i].type, ev[i].code,
								ev[i].value);
		}
	}
}

int kmscon_input_device_wake_up(struct kmscon_input_device *device)
{
	int ret;
	unsigned long ledbits[NLONGS(LED_CNT)] = { 0 };

	if (!device || !device->input || !device->input->eloop)
		return -EINVAL;

	if (device->rfd >= 0)
		return 0;

	device->rfd = open(device->devnode, O_CLOEXEC | O_NONBLOCK | O_RDONLY);
	if (device->rfd < 0) {
		log_warn("cannot open input device %s: %d",
				device->devnode, errno);
		return -errno;
	}

	if (device->features & FEATURE_HAS_KEYS) {
		if (device->features & FEATURE_HAS_LEDS) {
			errno = 0;
			ioctl(device->rfd, EVIOCGLED(sizeof(ledbits)),
								&ledbits);
			if (errno)
				log_warn("cannot discover state of LEDs %s: %m",
						device->devnode);
		}

		/* rediscover the keyboard state if sth changed during sleep */
		kmscon_kbd_reset(device->kbd, ledbits);

		ret = ev_eloop_new_fd(device->input->eloop, &device->fd,
						device->rfd, EV_READABLE,
						device_data_arrived, device);
		if (ret) {
			close(device->rfd);
			device->rfd = -1;
			return ret;
		}
	}

	return 0;
}

void kmscon_input_device_sleep(struct kmscon_input_device *device)
{
	if (!device)
		return;

	if (device->rfd < 0)
		return;

	ev_eloop_rm_fd(device->fd);
	device->fd = NULL;
	close(device->rfd);
	device->rfd = -1;
}

static int kmscon_input_device_new(struct kmscon_input_device **out,
			struct kmscon_input *input, const char *devnode,
						unsigned int features)
{
	int ret;
	struct kmscon_input_device *device;

	if (!out || !input)
		return -EINVAL;

	device = malloc(sizeof(*device));
	if (!device)
		return -ENOMEM;

	memset(device, 0, sizeof(*device));

	device->devnode = strdup(devnode);
	if (!device->devnode) {
		free(device);
		return -ENOMEM;
	}

	ret = kmscon_kbd_new(&device->kbd, input->desc);
	if (ret) {
		free(device->devnode);
		free(device);
		return ret;
	}

	device->input = input;
	device->features = features;
	device->rfd = -1;

	log_debug("new input device %s", devnode);
	*out = device;
	return 0;
}

static void kmscon_input_device_free(struct kmscon_input_device *device)
{
	if (!device)
		return;

	log_debug("destroying input device %s", device->devnode);
	kmscon_input_device_sleep(device);
	kmscon_kbd_unref(device->kbd);
	free(device->devnode);
	free(device);
}

int kmscon_input_new(struct kmscon_input **out)
{
	int ret;
	struct kmscon_input *input;

	if (!out)
		return -EINVAL;

	input = malloc(sizeof(*input));
	if (!input)
		return -ENOMEM;

	memset(input, 0, sizeof(*input));
	input->ref = 1;
	input->state = INPUT_ASLEEP;

	ret = kmscon_hook_new(&input->hook);
	if (ret)
		goto err_free;

	ret = kmscon_kbd_desc_new(&input->desc,
					conf_global.xkb_layout,
					conf_global.xkb_variant,
					conf_global.xkb_options);
	if (ret) {
		log_warn("cannot create xkb description");
		goto err_hook;
	}

	input->udev = udev_new();
	if (!input->udev) {
		log_warn("cannot create udev object");
		ret = -EFAULT;
		goto err_xkb;
	}

	input->monitor = udev_monitor_new_from_netlink(input->udev, "udev");
	if (!input->monitor) {
		log_warn("cannot create udev monitor");
		ret = -EFAULT;
		goto err_udev;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(input->monitor,
								"input", NULL);
	if (ret) {
		log_warn("cannot add udev filter");
		ret = -EFAULT;
		goto err_monitor;
	}

	ret = udev_monitor_enable_receiving(input->monitor);
	if (ret) {
		log_warn("cannot start udev monitor");
		ret = -EFAULT;
		goto err_monitor;
	}

	log_debug("new input object");
	*out = input;
	return 0;

err_monitor:
	udev_monitor_unref(input->monitor);
err_udev:
	udev_unref(input->udev);
err_xkb:
	kmscon_kbd_desc_unref(input->desc);
err_hook:
	kmscon_hook_free(input->hook);
err_free:
	free(input);
	return ret;
}

void kmscon_input_ref(struct kmscon_input *input)
{
	if (!input)
		return;

	++input->ref;
}

void kmscon_input_unref(struct kmscon_input *input)
{
	if (!input || !input->ref || --input->ref)
		return;

	log_debug("free input object");
	kmscon_input_disconnect_eloop(input);
	udev_monitor_unref(input->monitor);
	udev_unref(input->udev);
	kmscon_kbd_desc_unref(input->desc);
	kmscon_hook_free(input->hook);
	free(input);
}

/*
 * See if the device has anything useful to offer.
 * We go over the desired features and return a mask of enum device_feature's.
 */
static unsigned int probe_device_features(const char *node)
{
	int i, fd;
	unsigned int features = 0;
	unsigned long evbits[NLONGS(EV_CNT)] = { 0 };
	unsigned long keybits[NLONGS(KEY_CNT)] = { 0 };

	fd = open(node, O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return 0;

	/* Which types of input events the device supports. */
	errno = 0;
	ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
	if (errno)
		goto err_ioctl;

	/* Device supports keys/buttons. */
	if (kmscon_evdev_bit_is_set(evbits, EV_KEY)) {
		errno = 0;
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		if (errno)
			goto err_ioctl;

		/*
		 * If the device support any of the normal keyboard keys, we
		 * take it. Even if the keys are not ordinary they can be
		 * mapped to anything by the keyboard backend.
		 */
		for (i = KEY_RESERVED; i <= KEY_MIN_INTERESTING; i++) {
			if (kmscon_evdev_bit_is_set(keybits, i)) {
				features |= FEATURE_HAS_KEYS;
				break;
			}
		}
	}

	if (kmscon_evdev_bit_is_set(evbits, EV_LED))
		features |= FEATURE_HAS_LEDS;

	close(fd);
	return features;

err_ioctl:
	if (errno != ENOTTY)
		log_warn("cannot probe features of device (%s): %m", node);
	close(fd);
	return 0;
}

static void add_device(struct kmscon_input *input,
					struct udev_device *udev_device)
{
	int ret;
	struct kmscon_input_device *device;
	const char *node, *seat;
	unsigned int features;

	if (!input || !udev_device)
		return;

	node = udev_device_get_devnode(udev_device);
	if (!node)
		return;

	seat = udev_device_get_property_value(udev_device, "ID_SEAT");
	if (!seat)
		seat = "seat0";
	if (strcmp(seat, conf_global.seat)) {
		log_debug("ignoring device  %s (wrong seat)", node);
		return;
	}

	features = probe_device_features(node);
	if (!(features & FEATURE_HAS_KEYS)) {
		log_debug("ignoring non-useful device %s", node);
		return;
	}

	ret = kmscon_input_device_new(&device, input, node, features);
	if (ret) {
		log_warn("cannot create input device for %s", node);
		return;
	}

	if (input->state == INPUT_AWAKE) {
		ret = kmscon_input_device_wake_up(device);
		if (ret) {
			log_warn("cannot wake up new device %s", node);
			kmscon_input_device_free(device);
			return;
		}
	}

	device->next = input->devices;
	input->devices = device;
	log_debug("added device %s (features: %#x)", node, features);
}

static void remove_device(struct kmscon_input *input, const char *node)
{
	struct kmscon_input_device *iter, *prev;

	if (!input || !node || !input->devices)
		return;

	iter = input->devices;
	prev = NULL;

	while (iter) {
		if (!strcmp(iter->devnode, node)) {
			if (prev == NULL)
				input->devices = iter->next;
			else
				prev->next = iter->next;

			kmscon_input_device_free(iter);
			log_debug("removed device %s", node);
			break;
		}

		prev = iter;
		iter = iter->next;
	}
}

static void remove_device_udev(struct kmscon_input *input,
					struct udev_device *udev_device)
{
	const char *node;

	node = udev_device_get_devnode(udev_device);
	if (!node)
		return;

	remove_device(input, node);
}

static void device_changed(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_input *input = data;
	struct udev_device *udev_device;
	const char *action;

	udev_device = udev_monitor_receive_device(input->monitor);
	if (!udev_device)
		return;

	action = udev_device_get_action(udev_device);
	if (!action) {
		log_warn("cannot get action field of new device");
		goto err_device;
	}

	if (!strcmp(action, "add"))
		add_device(input, udev_device);
	else if (!strcmp(action, "remove"))
		remove_device_udev(input, udev_device);

err_device:
	udev_device_unref(udev_device);
}

static void add_initial_devices(struct kmscon_input *input)
{
	int ret;
	struct udev_enumerate *e;
	struct udev_list_entry *first;
	struct udev_list_entry *item;
	struct udev_device *udev_device;
	const char *syspath;

	e = udev_enumerate_new(input->udev);
	if (!e) {
		log_warn("cannot create udev enumeration");
		return;
	}

	ret = udev_enumerate_add_match_subsystem(e, "input");
	if (ret) {
		log_warn("cannot add match to udev enumeration");
		goto err_enum;
	}

	if (strcmp(conf_global.seat, "seat0")) {
		ret = udev_enumerate_add_match_tag(e, conf_global.seat);
		if (ret) {
			log_warn("cannot add match to udev enumeration");
			goto err_enum;
		}
	}

	ret = udev_enumerate_scan_devices(e);
	if (ret) {
		log_warn("cannot scan udev enumeration");
		goto err_enum;
	}

	first = udev_enumerate_get_list_entry(e);
	udev_list_entry_foreach(item, first) {
		syspath = udev_list_entry_get_name(item);
		if (!syspath)
			continue;

		udev_device = udev_device_new_from_syspath(input->udev, syspath);
		if (!udev_device)
			continue;

		add_device(input, udev_device);
		udev_device_unref(udev_device);
	}

err_enum:
	udev_enumerate_unref(e);
}

int kmscon_input_connect_eloop(struct kmscon_input *input,
				struct ev_eloop *eloop)
{
	int ret;
	int fd;

	if (!input || !eloop)
		return -EINVAL;

	if (input->eloop)
		return -EALREADY;

	fd = udev_monitor_get_fd(input->monitor);
	ret = ev_eloop_new_fd(eloop, &input->monitor_fd, fd,
				EV_READABLE, device_changed, input);
	if (ret)
		return ret;

	ev_eloop_ref(eloop);
	input->eloop = eloop;

	add_initial_devices(input);

	return 0;
}

void kmscon_input_disconnect_eloop(struct kmscon_input *input)
{
	struct kmscon_input_device *tmp;

	if (!input || !input->eloop)
		return;

	while (input->devices) {
		tmp = input->devices;
		input->devices = tmp->next;
		kmscon_input_device_free(tmp);
	}

	ev_eloop_rm_fd(input->monitor_fd);
	input->monitor_fd = NULL;
	ev_eloop_unref(input->eloop);
	input->eloop = NULL;
}

int kmscon_input_register_cb(struct kmscon_input *input, kmscon_input_cb cb,
				void *data)
{
	if (!input || !cb)
		return -EINVAL;

	return kmscon_hook_add_cast(input->hook, cb, data);
}

void kmscon_input_unregister_cb(struct kmscon_input *input, kmscon_input_cb cb,
				void *data)
{
	if (!input || !cb)
		return;

	kmscon_hook_rm_cast(input->hook, cb, data);
}

void kmscon_input_sleep(struct kmscon_input *input)
{
	struct kmscon_input_device *iter;

	if (!input || input->state == INPUT_ASLEEP)
		return;

	log_debug("going asleep");

	for (iter = input->devices; iter; iter = iter->next)
		kmscon_input_device_sleep(iter);

	input->state = INPUT_ASLEEP;
}

void kmscon_input_wake_up(struct kmscon_input *input)
{
	struct kmscon_input_device *iter, *prev, *tmp;
	int ret;

	if (!input || input->state == INPUT_AWAKE)
		return;

	log_debug("waking up");
	prev = NULL;
	iter = input->devices;

	while (iter) {
		ret = kmscon_input_device_wake_up(iter);
		if (ret) {
			if (!prev)
				input->devices = iter->next;
			else
				prev->next = iter->next;

			tmp = iter;
			iter = iter->next;

			log_warn("device %s does not wake up, removing device",
					tmp->devnode);
			kmscon_input_device_free(tmp);
		} else {
			prev = iter;
			iter = iter->next;
		}
	}

	input->state = INPUT_AWAKE;
}

bool kmscon_input_is_asleep(struct kmscon_input *input)
{
	if (!input)
		return false;

	return input->state == INPUT_ASLEEP;
}
