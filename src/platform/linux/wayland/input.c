/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */
#include "wayland.h"
#include <dirent.h>
#include <sys/ioctl.h>

/*
 * evdev definitions. We avoid including <linux/input.h> directly because it
 * defines its own 'struct input_event' which collides with ours in platform.h.
 */

struct evdev_event {
	struct timeval time;
	unsigned short type;
	unsigned short code;
	int value;
};

#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define REL_X 0x00
#define REL_Y 0x01
#define REL_MAX 0x0f
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MAX 0x3f
#define EVIOCGRAB _IOW('E', 0x90, int)
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), (len))
#define EVIOCGKEY(len) _IOC(_IOC_READ, 'E', 0x18, (len))
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, (len))

#define EV_SYN 0x00
#define SYN_REPORT 0x00
#define EV_MAX 0x1f
#define KEY_MAX 0x2ff

/* A few key codes for capability detection. */
#define KEY_1 2
#define KEY_EQUAL 13

/* Modifier keycodes (from linux/input-event-codes.h). */
#define KEY_LEFTCTRL   29
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT    56
#define KEY_LEFTMETA   125
#define KEY_RIGHTCTRL  97
#define KEY_RIGHTMETA  126
#define KEY_RIGHTALT   100

/* uinput definitions. */
#define UINPUT_MAX_NAME_SIZE 80
#define UI_DEV_CREATE  _IO('U', 1)
#define UI_DEV_DESTROY _IO('U', 2)
#define UI_SET_EVBIT   _IOW('U', 100, int)
#define UI_SET_KEYBIT  _IOW('U', 101, int)

struct uinput_setup {
	struct input_id {
		uint16_t bustype;
		uint16_t vendor;
		uint16_t product;
		uint16_t version;
	} id;
	char name[UINPUT_MAX_NAME_SIZE];
	uint32_t ff_effects_max;
};

#define UI_DEV_SETUP _IOW('U', 3, struct uinput_setup)
#define BUS_VIRTUAL 0x06

#define MAX_KEYBOARDS 32

static struct input_event input_queue[32];
static size_t input_queue_sz;

static uint8_t x_active_mods = 0;

struct keymap_entry keymap[256] = {0};

static int keyboard_fds[MAX_KEYBOARDS];
static int nr_keyboards = 0;

static void noop() {}

static void update_mods(uint8_t code, uint8_t pressed)
{
	const char *name = way_input_lookup_name(code, 0);

	if (!name)
		return;

	if (strstr(name, "Control") == name) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_CONTROL;
		else
			x_active_mods &= ~PLATFORM_MOD_CONTROL;
	}

	if (strstr(name, "Shift") == name) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_SHIFT;
		else
			x_active_mods &= ~PLATFORM_MOD_SHIFT;
	}

	if (strstr(name, "Super") == name) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_META;
		else
			x_active_mods &= ~PLATFORM_MOD_META;
	}

	if (strstr(name, "Alt") == name) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_ALT;
		else
			x_active_mods &= ~PLATFORM_MOD_ALT;
	}
}

/*
 * We still use the wl_keyboard listener to receive the compositor's keymap,
 * which may differ from system defaults (e.g. custom XKB settings in sway).
 * Key events themselves come from evdev.
 */

static void handle_keymap(void *data,
			  struct wl_keyboard *wl_keyboard,
			  uint32_t format, int32_t fd, uint32_t size)
{
	size_t i;
	char *buf;
	struct xkb_context *ctx;
	struct xkb_keymap *xkbmap;
	struct xkb_state *xkbstate;

	assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

	buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	assert(buf);

	ctx = xkb_context_new(0);
	assert(ctx);

	xkbmap = xkb_keymap_new_from_string(ctx, buf, XKB_KEYMAP_FORMAT_TEXT_V1, 0);

	assert(xkbmap);
	xkbstate = xkb_state_new(xkbmap);
	assert(xkbstate);

	for (i = 0; i < 248; i++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(xkbmap, i+8,
						     xkb_state_key_get_layout(xkbstate, i+8),
						     0, &syms)) {
			xkb_keysym_get_name(syms[0], keymap[i].name, sizeof keymap[i].name);
		}

		if (xkb_keymap_key_get_syms_by_level(xkbmap, i+8,
						     xkb_state_key_get_layout(xkbstate, i+8),
						     1,
						     &syms)) {
			xkb_keysym_get_name(syms[0], keymap[i].shifted_name, sizeof keymap[i].shifted_name);
		}
	}
	xkb_state_unref(xkbstate);
	xkb_keymap_unref(xkbmap);
	xkb_context_unref(ctx);

	munmap(buf, size);
	close(fd);
}

static struct wl_keyboard_listener wl_keyboard_listener = {
	.key = noop,
	.keymap = handle_keymap,
	.enter = noop,
	.leave = noop,
	.modifiers = noop,
	.repeat_info = noop,
};

static int is_mouse(int fd)
{
	unsigned long evbit[(EV_MAX / (8 * sizeof(long)) + 1)];
	unsigned long relbit[(REL_MAX / (8 * sizeof(long)) + 1)];

	memset(evbit, 0, sizeof evbit);
	memset(relbit, 0, sizeof relbit);

	if (ioctl(fd, EVIOCGBIT(0, sizeof evbit), evbit) < 0)
		return 0;

	if (!(evbit[0] & (1UL << EV_REL)))
		return 0;

	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbit), relbit) < 0)
		return 0;

	return (relbit[REL_X / (8 * sizeof(long))] & (1UL << (REL_X % (8 * sizeof(long))))) &&
	       (relbit[REL_Y / (8 * sizeof(long))] & (1UL << (REL_Y % (8 * sizeof(long)))));
}

static int is_keyboard(int fd)
{
	unsigned long evbit[(EV_MAX / (8 * sizeof(long)) + 1)];
	unsigned long keybit[(KEY_MAX / (8 * sizeof(long)) + 1)];
	unsigned long relbit[(REL_MAX / (8 * sizeof(long)) + 1)];
	unsigned long absbit[(ABS_MAX / (8 * sizeof(long)) + 1)];
	int i, has_keys;

	memset(evbit, 0, sizeof evbit);
	memset(keybit, 0, sizeof keybit);
	memset(relbit, 0, sizeof relbit);
	memset(absbit, 0, sizeof absbit);

	if (ioctl(fd, EVIOCGBIT(0, sizeof evbit), evbit) < 0)
		return 0;

	if (!(evbit[0] & (1UL << EV_KEY)))
		return 0;

	/* Ignore devices with relative X/Y axes (mice) */
	if (evbit[0] & (1UL << EV_REL)) {
		if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbit), relbit) >= 0) {
			if ((relbit[REL_X / (8 * sizeof(long))] & (1UL << (REL_X % (8 * sizeof(long))))) &&
			    (relbit[REL_Y / (8 * sizeof(long))] & (1UL << (REL_Y % (8 * sizeof(long)))))) {
				return 0;
			}
		}
	}

	/* Ignore devices with absolute X/Y axes (touchpads/joysticks) */
	if (evbit[0] & (1UL << EV_ABS)) {
		if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbit), absbit) >= 0) {
			if ((absbit[ABS_X / (8 * sizeof(long))] & (1UL << (ABS_X % (8 * sizeof(long))))) &&
			    (absbit[ABS_Y / (8 * sizeof(long))] & (1UL << (ABS_Y % (8 * sizeof(long)))))) {
				return 0;
			}
		}
	}

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit) < 0)
		return 0;

	/* Must have several typical keyboard keys (number row). */
	has_keys = 0;
	for (i = KEY_1; i <= KEY_EQUAL; i++) {
		if (keybit[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long)))))
			has_keys++;
	}

	return has_keys >= 5;
}

/*
 * Use evdev to grab keyboards at the kernel level. This is completely
 * independent of Wayland focus, eliminating the need for the invisible
 * surface hack and the suspend/resume dance for clicks and scrolls.
 */

/*
 * Release all currently pressed keys on the given keyboard fd so the
 * compositor sees them as released before we EVIOCGRAB the device.
 * Without this, the non-modifier part of an activation shortcut
 * (e.g. the 'x' in A-M-x) stays "pressed" in the compositor's view,
 * causing the next shortcut activation to fail.
 */
void way_input_release_keys(int fd)
{
	unsigned char key_state[(KEY_MAX + 7) / 8];
	size_t i;

	memset(key_state, 0, sizeof key_state);
	if (ioctl(fd, EVIOCGKEY(sizeof key_state), key_state) < 0)
		return;

	for (i = 0; i <= KEY_MAX; i++) {
		if (!(key_state[i / 8] & (1 << (i % 8))))
			continue;

		struct evdev_event ev = {0};
		ev.type = EV_KEY;
		ev.code = i;
		ev.value = 0; /* release */
		write(fd, &ev, sizeof ev);
	}

	/* SYN_REPORT to flush. */
	{
		struct evdev_event ev = {0};
		ev.type = EV_SYN;
		ev.code = SYN_REPORT;
		ev.value = 0;
		write(fd, &ev, sizeof ev);
	}
}

void way_input_grab_keyboard()
{
	DIR *dir;
	struct dirent *ent;
	char path[256];
	char name[256];

	nr_keyboards = 0;

	dir = opendir("/dev/input");
	if (!dir) {
		fprintf(stderr, "FATAL: Cannot open /dev/input\n");
		exit(-1);
	}

	while ((ent = readdir(dir)) && nr_keyboards < MAX_KEYBOARDS) {
		int fd;

		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);

		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (!is_keyboard(fd)) {
			close(fd);
			continue;
		}

		/*
		 * Release all pressed keys BEFORE grabbing so the
		 * compositor still sees them on this physical device.
		 */
		way_input_release_keys(fd);

		if (ioctl(fd, EVIOCGRAB, 1) < 0) {
			name[0] = '\0';
			ioctl(fd, EVIOCGNAME(sizeof name), name);
			fprintf(stderr, "WARNING: Failed to grab %s (%s)\n", path, name);
			close(fd);
			continue;
		}

		keyboard_fds[nr_keyboards++] = fd;
	}

	closedir(dir);

	if (nr_keyboards == 0) {
		fprintf(stderr, "FATAL: No keyboards found to grab (check permissions on /dev/input/)\n");
		exit(-1);
	}

	x_active_mods = 0;
}

void way_input_suspend_keyboard()
{
	/* No-op: evdev grab is independent of Wayland surfaces. */
}

void way_input_resume_keyboard()
{
	/* No-op: evdev grab is independent of Wayland surfaces. */
}

void way_input_ungrab_keyboard()
{
	int i;

	for (i = 0; i < nr_keyboards; i++) {
		ioctl(keyboard_fds[i], EVIOCGRAB, 0);
		close(keyboard_fds[i]);
	}
	nr_keyboards = 0;
}

struct input_event *way_input_next_event(int timeout)
{
	static struct input_event ev;
	struct pollfd pfds[MAX_KEYBOARDS + 1];
	int nfds, i;

	while (1) {
		if (input_queue_sz) {
			input_queue_sz--;
			ev = input_queue[0];
			memcpy(input_queue, input_queue + 1, sizeof(struct input_event) * input_queue_sz);
			return &ev;
		}

		nfds = 0;
		for (i = 0; i < nr_keyboards; i++) {
			pfds[nfds].fd = keyboard_fds[i];
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		/* Also poll the Wayland fd to keep dispatching protocol events
		 * (surface configure, etc.) while waiting for keyboard input. */
		pfds[nfds].fd = wl_display_get_fd(wl.dpy);
		pfds[nfds].events = POLLIN;
		nfds++;

		wl_display_flush(wl.dpy);
		wl_display_dispatch_pending(wl.dpy);

		if (!poll(pfds, nfds, timeout ? timeout : -1))
			return NULL;

		/* Dispatch any pending Wayland events. */
		if (pfds[nr_keyboards].revents & POLLIN)
			wl_display_dispatch(wl.dpy);

		/* Read evdev events from all grabbed keyboards. */
		for (i = 0; i < nr_keyboards; i++) {
			struct evdev_event raw;

			if (!(pfds[i].revents & POLLIN))
				continue;

			while (read(keyboard_fds[i], &raw, sizeof raw) == (ssize_t)sizeof raw) {
				if (raw.type != EV_KEY)
					continue;

				/* value: 0=release, 1=press, 2=repeat */
				if (raw.value == 2)
					continue;

				update_mods(raw.code, raw.value);

				if (input_queue_sz < sizeof input_queue / sizeof input_queue[0]) {
					struct input_event *qev = &input_queue[input_queue_sz++];
					qev->code = raw.code;
					qev->pressed = raw.value;
					qev->mods = x_active_mods;
				}
			}
		}
	}
}

void init_input()
{
	wl_keyboard_add_listener(wl_seat_get_keyboard(wl.seat), &wl_keyboard_listener, NULL);
}

static int mouse_fds[MAX_MICE];
static int nr_mice = 0;

void way_input_open_mice()
{
	DIR *dir;
	struct dirent *ent;
	char path[256];

	nr_mice = 0;

	dir = opendir("/dev/input");
	if (!dir)
		return;

	while ((ent = readdir(dir)) && nr_mice < MAX_MICE) {
		int fd;

		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);

		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (!is_mouse(fd)) {
			close(fd);
			continue;
		}

		mouse_fds[nr_mice++] = fd;
	}

	closedir(dir);
}

void way_input_close_mice()
{
	int i;
	for (i = 0; i < nr_mice; i++)
		close(mouse_fds[i]);
	nr_mice = 0;
}

int way_input_get_mouse_fds(int *fds)
{
	int i;
	for (i = 0; i < nr_mice; i++)
		fds[i] = mouse_fds[i];

	return nr_mice;
}

int way_input_poll_mice(int timeout)
{
	struct pollfd pfds[MAX_MICE];
	int i;

	if (!nr_mice)
		return 0;

	for (i = 0; i < nr_mice; i++) {
		pfds[i].fd = mouse_fds[i];
		pfds[i].events = POLLIN;
	}

	if (poll(pfds, nr_mice, timeout) <= 0)
		return 0;

	for (i = 0; i < nr_mice; i++) {
		if (!(pfds[i].revents & POLLIN))
			continue;

		struct evdev_event raw;
		while (read(mouse_fds[i], &raw, sizeof raw) == (ssize_t)sizeof raw) {
			if (raw.type == EV_REL &&
			    (raw.code == REL_X || raw.code == REL_Y))
				return 1;
		}
	}

	return 0;
}
