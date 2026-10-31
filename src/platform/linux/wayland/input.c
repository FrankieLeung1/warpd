/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */
#include "wayland.h"
#include <dirent.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/inotify.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void kanata_change_layer(const char *layer)
{
	int port = 10000;
	const char *port_env = getenv("KANATA_PORT");
	if (port_env && atoi(port_env) > 0)
		port = atoi(port_env);

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 20000; /* 20ms timeout */
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

	if (connect(sock, (struct sockaddr *)&addr, sizeof addr) == 0) {
		char msg[128];
		int len = snprintf(msg, sizeof msg, "{\"ChangeLayer\":{\"new\":\"%s\"}}\n", layer);
		if (write(sock, msg, len) > 0) {
			fprintf(stderr, "[warpd] Switched kanata layer to: %s\n", layer);
		}
		shutdown(sock, SHUT_WR);
		char dummy[128];
		while (read(sock, dummy, sizeof dummy) > 0) {}
	}
	close(sock);
}

static uint64_t get_time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
#define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, (len))

#define EV_SYN 0x00
#define SYN_REPORT 0x00
#define EV_MAX 0x1f
#define KEY_MAX 0x2ff

#define BTN_LEFT     0x110
#define BTN_JOYSTICK 0x120
#define BTN_GAMEPAD  0x130
#define BTN_TOUCH    0x14a

#define INPUT_PROP_ACCELEROMETER 0x06
#define INPUT_PROP_MAX           0x1f

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
static char keyboard_names[MAX_KEYBOARDS][64];
int nr_keyboards = 0;

struct busy_keyboard {
	char name[64];
	int fd;
};

static struct busy_keyboard busy_keyboards[MAX_KEYBOARDS];
static int nr_busy_keyboards = 0;
static int keyboards_grabbed = 0;

static int mouse_fds[MAX_MICE];
static char mouse_names[MAX_MICE][64];
static int mouse_last_abs_x[MAX_MICE];
static int mouse_last_abs_y[MAX_MICE];
static int nr_mice = 0;

static int input_inotify_fd = -1;
static int input_inotify_wd = -1;

static void noop() {}

static int ctrl_active = 0;
static int shift_active = 0;
static int alt_active = 0;
static int meta_active = 0;

static void sync_virtual_mod(int active, int *state, const char *mod_name)
{
	if (active && !*state) {
		warpd_debug("Wayland emit: sync_virtual_mod PRESS '%s' (running wtype -M %s)", mod_name, mod_name);
		pid_t child = fork();
		if (child == 0) {
			execlp("wtype", "wtype", "-M", mod_name, NULL);
			_exit(1);
		} else if (child > 0) {
			int status;
			waitpid(child, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				*state = 1;
				warpd_debug("Wayland emit: sync_virtual_mod PRESS '%s' succeeded", mod_name);
			} else {
				warpd_debug("Wayland emit: sync_virtual_mod PRESS '%s' failed (status=%d)", mod_name, status);
			}
		}
	} else if (!active && *state) {
		warpd_debug("Wayland emit: sync_virtual_mod RELEASE '%s' (running wtype -m %s)", mod_name, mod_name);
		pid_t child = fork();
		if (child == 0) {
			execlp("wtype", "wtype", "-m", mod_name, NULL);
			_exit(1);
		} else if (child > 0) {
			int status;
			waitpid(child, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				*state = 0;
				warpd_debug("Wayland emit: sync_virtual_mod RELEASE '%s' succeeded", mod_name);
			} else {
				warpd_debug("Wayland emit: sync_virtual_mod RELEASE '%s' failed (status=%d)", mod_name, status);
			}
		}
	}
}

static void sync_virtual_mods()
{
	sync_virtual_mod(x_active_mods & PLATFORM_MOD_CONTROL, &ctrl_active, "ctrl");
	sync_virtual_mod(x_active_mods & PLATFORM_MOD_SHIFT, &shift_active, "shift");
	sync_virtual_mod(x_active_mods & PLATFORM_MOD_ALT, &alt_active, "alt");
	sync_virtual_mod(x_active_mods & PLATFORM_MOD_META, &meta_active, "logo");
}

static void update_mods(uint8_t code, uint8_t pressed)
{
	const char *name = way_input_lookup_name(code, 0);

	if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
	    (name && (strstr(name, "Control") || strstr(name, "Ctrl")))) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_CONTROL;
		else
			x_active_mods &= ~PLATFORM_MOD_CONTROL;
	} else if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
		   (name && strstr(name, "Shift"))) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_SHIFT;
		else
			x_active_mods &= ~PLATFORM_MOD_SHIFT;
	} else if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA ||
		   (name && (strstr(name, "Super") || strstr(name, "Meta") ||
			     strstr(name, "Hyper") || strstr(name, "Win")))) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_META;
		else
			x_active_mods &= ~PLATFORM_MOD_META;
	} else if (code == KEY_LEFTALT || code == KEY_RIGHTALT ||
		   (name && (strstr(name, "Alt") || strstr(name, "ISO_Level3_Shift") ||
			     strstr(name, "Mode_switch")))) {
		if (pressed)
			x_active_mods |= PLATFORM_MOD_ALT;
		else
			x_active_mods &= ~PLATFORM_MOD_ALT;
	}

	sync_virtual_mods();
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

static void handle_key(void *data,
		       struct wl_keyboard *wl_keyboard,
		       uint32_t serial,
		       uint32_t time,
		       uint32_t key,
		       uint32_t state)
{
	(void)data;
	(void)wl_keyboard;
	(void)serial;
	(void)time;

	/*
	 * When evdev keyboards are grabbed, the raw evdev loop is the sole
	 * authoritative event source. Accepting events here too would cause
	 * every keypress to be enqueued twice (and update_mods called twice),
	 * producing double modifier emissions and broken release tracking.
	 */
	if (nr_keyboards > 0)
		return;

	/* value: 0=released, 1=pressed */
	update_mods(key, state);

	if (input_queue_sz < sizeof input_queue / sizeof input_queue[0]) {
		struct input_event *qev = &input_queue[input_queue_sz++];
		qev->code = key;
		qev->pressed = state;
		qev->mods = x_active_mods;
	}
}

static struct wl_keyboard_listener wl_keyboard_listener = {
	.key = handle_key,
	.keymap = handle_keymap,
	.enter = noop,
	.leave = noop,
	.modifiers = noop,
	.repeat_info = noop,
};

static void init_inotify_if_needed()
{
	if (input_inotify_fd < 0) {
		input_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (input_inotify_fd >= 0) {
			input_inotify_wd = inotify_add_watch(input_inotify_fd, "/dev/input",
							     IN_CREATE | IN_DELETE | IN_ATTRIB);
			if (input_inotify_wd < 0) {
				fprintf(stderr, "[warpd debug] Failed to watch /dev/input: %s\n", strerror(errno));
			} else {
				fprintf(stderr, "[warpd debug] Watching /dev/input for device hotplug/changes\n");
			}
		} else {
			fprintf(stderr, "[warpd debug] Failed to init inotify: %s\n", strerror(errno));
		}
	}
}

int way_input_get_inotify_fd()
{
	init_inotify_if_needed();
	return input_inotify_fd;
}

static inline int test_bit(const unsigned long *arr, int bit)
{
	return (arr[bit / (8 * sizeof(long))] & (1UL << (bit % (8 * sizeof(long))))) != 0;
}

static int is_mouse(int fd)
{
	unsigned long evbit[(EV_MAX / (8 * sizeof(long)) + 1)];
	unsigned long relbit[(REL_MAX / (8 * sizeof(long)) + 1)];
	unsigned long absbit[(ABS_MAX / (8 * sizeof(long)) + 1)];
	unsigned long keybit[(KEY_MAX / (8 * sizeof(long)) + 1)];
	unsigned long propbit[(INPUT_PROP_MAX / (8 * sizeof(long)) + 1)];
	char dev_name[256] = "Unknown";

	ioctl(fd, EVIOCGNAME(sizeof dev_name), dev_name);
	/* Ignore virtual keyboards */
	if (strcasecmp(dev_name, "kanata") == 0 ||
	    strcasecmp(dev_name, "kmonad") == 0)
		return 0;

	memset(evbit, 0, sizeof evbit);
	memset(relbit, 0, sizeof relbit);
	memset(absbit, 0, sizeof absbit);
	memset(keybit, 0, sizeof keybit);
	memset(propbit, 0, sizeof propbit);

	if (ioctl(fd, EVIOCGBIT(0, sizeof evbit), evbit) < 0)
		return 0;

	if (ioctl(fd, EVIOCGPROP(sizeof propbit), propbit) >= 0) {
		if (test_bit(propbit, INPUT_PROP_ACCELEROMETER))
			return 0;
	}

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit) >= 0) {
		if (test_bit(keybit, BTN_GAMEPAD) || test_bit(keybit, BTN_JOYSTICK))
			return 0;
	}

	if (test_bit(evbit, EV_REL)) {
		if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbit), relbit) >= 0) {
			if (test_bit(relbit, REL_X) && test_bit(relbit, REL_Y))
				return 1;
		}
	}

	if (test_bit(evbit, EV_ABS)) {
		if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbit), absbit) >= 0) {
			if (test_bit(absbit, ABS_X) && test_bit(absbit, ABS_Y)) {
				/* For absolute pointers, require pointer button capability (BTN_LEFT or BTN_TOUCH) */
				if (test_bit(keybit, BTN_LEFT) || test_bit(keybit, BTN_TOUCH))
					return 1;
			}
		}
	}

	return 0;
}

static int is_keyboard(int fd)
{
	unsigned long evbit[(EV_MAX / (8 * sizeof(long)) + 1)];
	unsigned long keybit[(KEY_MAX / (8 * sizeof(long)) + 1)];
	unsigned long relbit[(REL_MAX / (8 * sizeof(long)) + 1)];
	unsigned long absbit[(ABS_MAX / (8 * sizeof(long)) + 1)];
	int i, has_keys;
	char dev_name[256] = "Unknown";

	ioctl(fd, EVIOCGNAME(sizeof dev_name), dev_name);

	memset(evbit, 0, sizeof evbit);
	memset(keybit, 0, sizeof keybit);
	memset(relbit, 0, sizeof relbit);
	memset(absbit, 0, sizeof absbit);

	if (ioctl(fd, EVIOCGBIT(0, sizeof evbit), evbit) < 0) {
		fprintf(stderr, "[warpd debug] %s: Failed to get evbits\n", dev_name);
		return 0;
	}

	if (!(evbit[0] & (1UL << EV_KEY))) {
		fprintf(stderr, "[warpd debug] %s: Missing EV_KEY capability\n", dev_name);
		return 0;
	}

	int is_explicit_virtual_kbd = (strcasecmp(dev_name, "kanata") == 0 ||
				       strcasecmp(dev_name, "kmonad") == 0);

	/* Ignore devices with relative X/Y axes (mice), unless known virtual keyboard */
	if (!is_explicit_virtual_kbd && (evbit[0] & (1UL << EV_REL))) {
		if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbit), relbit) >= 0) {
			if ((relbit[REL_X / (8 * sizeof(long))] & (1UL << (REL_X % (8 * sizeof(long))))) &&
			    (relbit[REL_Y / (8 * sizeof(long))] & (1UL << (REL_Y % (8 * sizeof(long)))))) {
				fprintf(stderr, "[warpd debug] %s: Ignored because it has relative axes (mouse)\n", dev_name);
				return 0;
			}
		}
	}

	/* Ignore devices with absolute X/Y axes (touchpads/joysticks), unless known virtual keyboard */
	if (!is_explicit_virtual_kbd && (evbit[0] & (1UL << EV_ABS))) {
		if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbit), absbit) >= 0) {
			if ((absbit[ABS_X / (8 * sizeof(long))] & (1UL << (ABS_X % (8 * sizeof(long))))) &&
			    (absbit[ABS_Y / (8 * sizeof(long))] & (1UL << (ABS_Y % (8 * sizeof(long)))))) {
				fprintf(stderr, "[warpd debug] %s: Ignored because it has absolute axes (touchpad/joystick)\n", dev_name);
				return 0;
			}
		}
	}

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit) < 0) {
		fprintf(stderr, "[warpd debug] %s: Failed to get keybits\n", dev_name);
		return 0;
	}

	/* Must have several typical keyboard keys (number row). */
	has_keys = 0;
	for (i = KEY_1; i <= KEY_EQUAL; i++) {
		if (keybit[i / (8 * sizeof(long))] & (1UL << (i % (8 * sizeof(long)))))
			has_keys++;
	}

	fprintf(stderr, "[warpd debug] %s: has_keys count: %d (needs >= 5)\n", dev_name, has_keys);

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

		warpd_debug("Wayland emit: releasing held evdev key code %zu on fd %d", i, fd);
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

static void remove_keyboard_at(int idx)
{
	if (idx < 0 || idx >= nr_keyboards)
		return;

	fprintf(stderr, "[warpd] Device removed: keyboard %s (fd %d)\n",
		keyboard_names[idx], keyboard_fds[idx]);

	ioctl(keyboard_fds[idx], EVIOCGRAB, 0);
	close(keyboard_fds[idx]);

	for (int i = idx; i < nr_keyboards - 1; i++) {
		keyboard_fds[i] = keyboard_fds[i + 1];
		memcpy(keyboard_names[i], keyboard_names[i + 1], sizeof(keyboard_names[i]));
	}
	nr_keyboards--;

	if (nr_keyboards == 0) {
		x_active_mods = 0;
		sync_virtual_mods();
	}
}

static void remove_busy_keyboard_at(int idx)
{
	if (idx < 0 || idx >= nr_busy_keyboards)
		return;

	fprintf(stderr, "[warpd] Device removed: busy keyboard %s (fd %d)\n",
		busy_keyboards[idx].name, busy_keyboards[idx].fd);

	close(busy_keyboards[idx].fd);
	for (int i = idx; i < nr_busy_keyboards - 1; i++) {
		busy_keyboards[i] = busy_keyboards[i + 1];
	}
	nr_busy_keyboards--;
}

static int has_kanata_keyboard(void)
{
	for (int i = 0; i < nr_keyboards; i++) {
		char name[256] = "";
		ioctl(keyboard_fds[i], EVIOCGNAME(sizeof name), name);
		if (strcasecmp(name, "kanata") == 0)
			return 1;
	}
	return 0;
}

static int try_grab_fd(int fd, const char *dev_name, const char *path)
{
	if (nr_keyboards >= MAX_KEYBOARDS)
		return -1;

	char name[256] = "";
	ioctl(fd, EVIOCGNAME(sizeof name), name);

	way_input_release_keys(fd);

	if (ioctl(fd, EVIOCGRAB, 1) < 0) {
		if (errno == EBUSY) {
			if (has_kanata_keyboard()) {
				fprintf(stderr, "[warpd] %s (%s): Device busy (managed by kanata); skipping\n",
					path ? path : dev_name, name);
				close(fd);
				return 0;
			}
			fprintf(stderr, "[warpd] %s (%s): Device busy; queued for retry\n",
				path ? path : dev_name, name);
			if (nr_busy_keyboards < MAX_KEYBOARDS) {
				for (int i = 0; i < nr_busy_keyboards; i++) {
					if (!strcmp(busy_keyboards[i].name, dev_name))
						return 0;
				}
				busy_keyboards[nr_busy_keyboards].fd = fd;
				snprintf(busy_keyboards[nr_busy_keyboards].name,
					 sizeof(busy_keyboards[nr_busy_keyboards].name),
					 "%s", dev_name);
				nr_busy_keyboards++;
				return 0;
			}
		}
		fprintf(stderr, "[warpd] WARNING: Failed to grab %s (%s): %s\n",
			path ? path : dev_name, name, strerror(errno));
		return -1;
	}

	fprintf(stderr, "[warpd] Device added: keyboard %s (%s, fd %d)\n",
		name[0] ? name : "Unknown", path ? path : dev_name, fd);

	snprintf(keyboard_names[nr_keyboards],
		 sizeof keyboard_names[nr_keyboards],
		 "%s", dev_name);
	keyboard_fds[nr_keyboards++] = fd;

	if (strcasecmp(name, "kanata") == 0 && nr_busy_keyboards > 0) {
		for (int b = 0; b < nr_busy_keyboards; b++) {
			fprintf(stderr, "[warpd] Dropping busy device %s (managed by kanata)\n",
				busy_keyboards[b].name);
			close(busy_keyboards[b].fd);
		}
		nr_busy_keyboards = 0;
	}

	return 1;
}

static void retry_busy_keyboards()
{
	int i = 0;
	while (i < nr_busy_keyboards && nr_keyboards < MAX_KEYBOARDS) {
		int fd = busy_keyboards[i].fd;
		char name[256] = "";
		ioctl(fd, EVIOCGNAME(sizeof name), name);

		way_input_release_keys(fd);

		if (ioctl(fd, EVIOCGRAB, 1) == 0) {
			fprintf(stderr, "[warpd] Device added: keyboard %s (%s, fd %d)\n",
				name[0] ? name : "Unknown", busy_keyboards[i].name, fd);

			snprintf(keyboard_names[nr_keyboards],
				 sizeof keyboard_names[nr_keyboards],
				 "%s", busy_keyboards[i].name);
			keyboard_fds[nr_keyboards++] = fd;

			for (int j = i; j < nr_busy_keyboards - 1; j++)
				busy_keyboards[j] = busy_keyboards[j + 1];
			nr_busy_keyboards--;
		} else if (errno == EBUSY) {
			i++;
		} else {
			fprintf(stderr, "[warpd debug] Busy device %s error: %s; dropping\n",
				busy_keyboards[i].name, strerror(errno));
			close(fd);
			for (int j = i; j < nr_busy_keyboards - 1; j++)
				busy_keyboards[j] = busy_keyboards[j + 1];
			nr_busy_keyboards--;
		}
	}
}

static void handle_device_added(const char *dev_name)
{
	if (strncmp(dev_name, "event", 5) != 0)
		return;

	for (int i = 0; i < nr_keyboards; i++) {
		if (!strcmp(keyboard_names[i], dev_name))
			return;
	}
	for (int i = 0; i < nr_busy_keyboards; i++) {
		if (!strcmp(busy_keyboards[i].name, dev_name))
			return;
	}

	char path[512];
	snprintf(path, sizeof path, "/dev/input/%s", dev_name);

	int fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;

	if (is_keyboard(fd)) {
		fprintf(stderr, "[warpd debug] New keyboard device detected: %s\n", path);
		if (keyboards_grabbed) {
			int res = try_grab_fd(fd, dev_name, path);
			if (res < 0)
				close(fd);
		} else {
			close(fd);
		}
	} else if (is_mouse(fd)) {
		fprintf(stderr, "[warpd debug] New mouse device detected: %s\n", path);
		if (nr_mice > 0 && nr_mice < MAX_MICE) {
			int exists = 0;
			for (int i = 0; i < nr_mice; i++) {
				if (!strcmp(mouse_names[i], dev_name)) {
					exists = 1;
					break;
				}
			}
			if (!exists) {
				snprintf(mouse_names[nr_mice], sizeof mouse_names[nr_mice], "%s", dev_name);
				mouse_last_abs_x[nr_mice] = -1;
				mouse_last_abs_y[nr_mice] = -1;
				mouse_fds[nr_mice++] = fd;
				return;
			}
		}
		close(fd);
	} else {
		close(fd);
	}
}

static void handle_device_removed(const char *dev_name)
{
	if (strncmp(dev_name, "event", 5) != 0)
		return;

	for (int i = 0; i < nr_keyboards; i++) {
		if (!strcmp(keyboard_names[i], dev_name)) {
			remove_keyboard_at(i);
			break;
		}
	}

	for (int i = 0; i < nr_busy_keyboards; i++) {
		if (!strcmp(busy_keyboards[i].name, dev_name)) {
			remove_busy_keyboard_at(i);
			break;
		}
	}

	for (int i = 0; i < nr_mice; i++) {
		if (!strcmp(mouse_names[i], dev_name)) {
			fprintf(stderr, "[warpd debug] Device removed: mouse %s (fd %d)\n",
				mouse_names[i], mouse_fds[i]);
			close(mouse_fds[i]);
			for (int j = i; j < nr_mice - 1; j++) {
				mouse_fds[j] = mouse_fds[j + 1];
				memcpy(mouse_names[j], mouse_names[j + 1], sizeof mouse_names[j]);
				mouse_last_abs_x[j] = mouse_last_abs_x[j + 1];
				mouse_last_abs_y[j] = mouse_last_abs_y[j + 1];
			}
			nr_mice--;
			break;
		}
	}
}

void way_input_drain_inotify()
{
	if (input_inotify_fd < 0)
		return;

	char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
	ssize_t len;

	while ((len = read(input_inotify_fd, buf, sizeof buf)) > 0) {
		const struct inotify_event *event;
		for (char *ptr = buf; ptr < buf + len;
		     ptr += sizeof(struct inotify_event) + event->len) {
			event = (const struct inotify_event *)ptr;
			if (event->len > 0) {
				if (event->mask & IN_DELETE) {
					handle_device_removed(event->name);
				} else if (event->mask & (IN_CREATE | IN_ATTRIB)) {
					handle_device_added(event->name);
				}
			}
		}
	}
}

void way_input_grab_keyboard()
{
	DIR *dir;
	struct dirent *ent;
	char path[512];
	int i, j;

	if (keyboards_grabbed)
		return;

	kanata_change_layer("warpd-active");

	init_inotify_if_needed();
	way_input_drain_inotify();

	keyboards_grabbed = 1;

	/*
	 * Re-grab any cached fds first.
	 * If re-grab fails with EBUSY, queue into busy_keyboards for retry.
	 * If re-grab fails with ENODEV/EBADF, close and discard.
	 */
	j = 0;
	for (i = 0; i < nr_keyboards; i++) {
		way_input_release_keys(keyboard_fds[i]);

		if (ioctl(keyboard_fds[i], EVIOCGRAB, 1) < 0) {
			if (errno == EBUSY) {
				fprintf(stderr, "[warpd debug] Cached fd %d (%s) is busy, queuing for retry\n",
					keyboard_fds[i], keyboard_names[i]);
				if (nr_busy_keyboards < MAX_KEYBOARDS) {
					busy_keyboards[nr_busy_keyboards].fd = keyboard_fds[i];
					snprintf(busy_keyboards[nr_busy_keyboards].name,
						 sizeof(busy_keyboards[nr_busy_keyboards].name),
						 "%s", keyboard_names[i]);
					nr_busy_keyboards++;
				} else {
					close(keyboard_fds[i]);
				}
			} else {
				fprintf(stderr, "[warpd debug] Failed to re-grab cached fd %d (%s): %s\n",
					keyboard_fds[i], keyboard_names[i], strerror(errno));
				close(keyboard_fds[i]);
			}
			continue;
		}

		if (j != i) {
			memcpy(keyboard_names[j], keyboard_names[i],
			       sizeof keyboard_names[j]);
			keyboard_fds[j] = keyboard_fds[i];
		}
		j++;
	}
	nr_keyboards = j;

	/*
	 * Scan /dev/input for keyboards that are not already grabbed or busy.
	 */
	dir = opendir("/dev/input");
	if (dir) {
		while ((ent = readdir(dir)) && (nr_keyboards + nr_busy_keyboards) < MAX_KEYBOARDS) {
			int fd, already_tracked = 0;

			if (strncmp(ent->d_name, "event", 5) != 0)
				continue;

			for (i = 0; i < nr_keyboards; i++) {
				if (!strcmp(keyboard_names[i], ent->d_name)) {
					already_tracked = 1;
					break;
				}
			}
			if (already_tracked)
				continue;

			for (i = 0; i < nr_busy_keyboards; i++) {
				if (!strcmp(busy_keyboards[i].name, ent->d_name)) {
					already_tracked = 1;
					break;
				}
			}
			if (already_tracked)
				continue;

			snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);

			fd = open(path, O_RDWR | O_NONBLOCK);
			if (fd < 0)
				fd = open(path, O_RDONLY | O_NONBLOCK);
			if (fd < 0) {
				fprintf(stderr, "[warpd debug] Failed to open %s: %s\n", path, strerror(errno));
				continue;
			}

			if (!is_keyboard(fd)) {
				close(fd);
				continue;
			}

			int res = try_grab_fd(fd, ent->d_name, path);
			if (res < 0)
				close(fd);
		}
		closedir(dir);
	} else {
		fprintf(stderr, "[warpd debug] Failed to open /dev/input: %s\n", strerror(errno));
	}

	if (has_kanata_keyboard() && nr_busy_keyboards > 0) {
		for (int b = 0; b < nr_busy_keyboards; b++)
			close(busy_keyboards[b].fd);
		nr_busy_keyboards = 0;
	}

	/*
	 * If any candidate keyboards were busy (e.g. kanata shutting down via kanata_sync.sh),
	 * retry with short delays to grab them the moment they become free.
	 */
	if (nr_busy_keyboards > 0) {
		fprintf(stderr, "[warpd debug] Waiting for %d busy keyboard(s) to release...\n", nr_busy_keyboards);
		for (int attempt = 0; attempt < 30 && nr_busy_keyboards > 0; attempt++) {
			usleep(10000); /* 10ms */
			retry_busy_keyboards();
			way_input_drain_inotify();
			if (nr_keyboards > 0 && nr_busy_keyboards == 0)
				break;
		}
	}

	if (nr_keyboards == 0) {
		if (nr_busy_keyboards > 0) {
			fprintf(stderr, "[warpd debug] %d keyboard(s) still busy; will continue retrying in background loop\n", nr_busy_keyboards);
		} else {
			fprintf(stderr, "WARNING: No keyboards found to grab (check permissions on /dev/input/). Falling back to Wayland key events (requires window focus).\n");
		}
	} else {
		fprintf(stderr, "[warpd debug] Total active keyboards grabbed: %d (busy pending: %d)\n",
			nr_keyboards, nr_busy_keyboards);
	}

	x_active_mods = 0;
	sync_virtual_mods();
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

	if (!keyboards_grabbed)
		return;

	keyboards_grabbed = 0;

	for (i = 0; i < nr_keyboards; i++)
		ioctl(keyboard_fds[i], EVIOCGRAB, 0);

	for (i = 0; i < nr_busy_keyboards; i++)
		close(busy_keyboards[i].fd);
	nr_busy_keyboards = 0;

	x_active_mods = 0;
	sync_virtual_mods();

	kanata_change_layer("base");
}

struct input_event *way_input_next_event(int timeout)
{
	static struct input_event ev;
	struct pollfd pfds[MAX_KEYBOARDS + 2];
	int nfds, i;

	while (1) {
		if (input_queue_sz) {
			input_queue_sz--;
			ev = input_queue[0];
			memcpy(input_queue, input_queue + 1, sizeof(struct input_event) * input_queue_sz);
			return &ev;
		}

		way_input_drain_inotify();
		if (nr_busy_keyboards > 0)
			retry_busy_keyboards();

		nfds = 0;
		for (i = 0; i < nr_keyboards; i++) {
			pfds[nfds].fd = keyboard_fds[i];
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		/* Also poll the Wayland fd to keep dispatching protocol events */
		int wl_idx = nfds;
		pfds[nfds].fd = wl_display_get_fd(wl.dpy);
		pfds[nfds].events = POLLIN;
		nfds++;

		/* Inotify events for device add/remove */
		int inotify_idx = -1;
		if (input_inotify_fd >= 0) {
			inotify_idx = nfds;
			pfds[nfds].fd = input_inotify_fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		wl_display_flush(wl.dpy);
		wl_display_dispatch_pending(wl.dpy);

		int poll_timeout = timeout ? timeout : -1;
		if (nr_busy_keyboards > 0 || nr_keyboards == 0) {
			if (poll_timeout < 0 || poll_timeout > 30)
				poll_timeout = 30;
		}

		int prc = poll(pfds, nfds, poll_timeout);
		if (prc < 0) {
			if (errno == EINTR)
				continue;
			return NULL;
		}

		if (prc == 0) {
			if (nr_busy_keyboards > 0)
				retry_busy_keyboards();
			if (timeout != 0)
				return NULL;
			continue;
		}

		/* Check inotify */
		if (inotify_idx >= 0 && (pfds[inotify_idx].revents & POLLIN)) {
			way_input_drain_inotify();
			if (nr_busy_keyboards > 0)
				retry_busy_keyboards();
		}

		/* Dispatch any pending Wayland events */
		if (pfds[wl_idx].revents & POLLIN)
			wl_display_dispatch(wl.dpy);

		/* Read evdev events from all grabbed keyboards */
		for (i = 0; i < nr_keyboards; ) {
			if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
				fprintf(stderr, "[warpd] Device removed: keyboard %s (poll revents 0x%x)\n",
					keyboard_names[i], pfds[i].revents);
				remove_keyboard_at(i);
				continue;
			}

			if (pfds[i].revents & POLLIN) {
				struct evdev_event raw;
				ssize_t nread;
				int disconnected = 0;

				while ((nread = read(keyboard_fds[i], &raw, sizeof raw)) == (ssize_t)sizeof raw) {
					if (raw.type != EV_KEY)
						continue;

					static int last_active_kbd_fd = -1;
					if (keyboard_fds[i] != last_active_kbd_fd) {
						char kbd_name[256] = "";
						ioctl(keyboard_fds[i], EVIOCGNAME(sizeof kbd_name), kbd_name);
						fprintf(stderr, "[warpd] Active input switched to keyboard: %s (%s)\n",
							kbd_name[0] ? kbd_name : "Unknown", keyboard_names[i]);
						last_active_kbd_fd = keyboard_fds[i];
					}

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

				if (nread < 0 && (errno == ENODEV || errno == EBADF || errno == EIO)) {
					fprintf(stderr, "[warpd] Device removed: keyboard %s (read: %s)\n",
						keyboard_names[i], strerror(errno));
					disconnected = 1;
				}

				if (disconnected) {
					remove_keyboard_at(i);
					continue;
				}
			}

			i++;
		}
	}
}

void init_input()
{
	wl_keyboard_add_listener(wl_seat_get_keyboard(wl.seat), &wl_keyboard_listener, NULL);
	init_inotify_if_needed();
}

void way_input_open_mice()
{
	DIR *dir;
	struct dirent *ent;
	char path[512];

	init_inotify_if_needed();

	/* Prune any stale/dead mouse fds */
	int j = 0;
	for (int i = 0; i < nr_mice; i++) {
		struct stat st;
		if (fstat(mouse_fds[i], &st) < 0) {
			close(mouse_fds[i]);
			continue;
		}
		if (j != i) {
			mouse_fds[j] = mouse_fds[i];
			memcpy(mouse_names[j], mouse_names[i], sizeof mouse_names[j]);
			mouse_last_abs_x[j] = mouse_last_abs_x[i];
			mouse_last_abs_y[j] = mouse_last_abs_y[i];
		}
		j++;
	}
	nr_mice = j;

	dir = opendir("/dev/input");
	if (!dir)
		return;

	while ((ent = readdir(dir)) && nr_mice < MAX_MICE) {
		int fd, i, dup = 0;

		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		for (i = 0; i < nr_mice; i++) {
			if (!strcmp(mouse_names[i], ent->d_name)) {
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;

		snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);

		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (!is_mouse(fd)) {
			close(fd);
			continue;
		}

		char mname[256] = "";
		ioctl(fd, EVIOCGNAME(sizeof mname), mname);
		strncpy(mouse_names[nr_mice], ent->d_name, sizeof(mouse_names[nr_mice]) - 1);
		mouse_names[nr_mice][sizeof(mouse_names[nr_mice]) - 1] = '\0';
		mouse_last_abs_x[nr_mice] = -1;
		mouse_last_abs_y[nr_mice] = -1;
		mouse_fds[nr_mice++] = fd;
		fprintf(stderr, "[warpd] Device added: mouse %s (/dev/input/%s, fd %d)\n",
			mname[0] ? mname : "Unknown", ent->d_name, fd);
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
	static int last_active_mouse_fd = -1;
	static uint64_t last_hw_mouse_time = 0;

	if (!nr_mice)
		return 0;

	for (i = 0; i < nr_mice; i++) {
		pfds[i].fd = mouse_fds[i];
		pfds[i].events = POLLIN;
	}

	if (poll(pfds, nr_mice, timeout) <= 0)
		return 0;

	int mouse_moved = 0;
	for (i = 0; i < nr_mice; ) {
		if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			fprintf(stderr, "[warpd] Device removed: mouse %s\n", mouse_names[i]);
			close(mouse_fds[i]);
			for (int k = i; k < nr_mice - 1; k++) {
				mouse_fds[k] = mouse_fds[k + 1];
				memcpy(mouse_names[k], mouse_names[k + 1], sizeof mouse_names[k]);
				mouse_last_abs_x[k] = mouse_last_abs_x[k + 1];
				mouse_last_abs_y[k] = mouse_last_abs_y[k + 1];
			}
			nr_mice--;
			continue;
		}

		if (pfds[i].revents & POLLIN) {
			struct evdev_event raw;
			ssize_t n;
			int disconnected = 0;
			while ((n = read(mouse_fds[i], &raw, sizeof raw)) == (ssize_t)sizeof raw) {
				int moved = 0;
				if (raw.type == EV_REL && (raw.code == REL_X || raw.code == REL_Y)) {
					moved = 1;
				} else if (raw.type == EV_ABS) {
					if (raw.code == ABS_X) {
						if (mouse_last_abs_x[i] != -1 && mouse_last_abs_x[i] != raw.value)
							moved = 1;
						mouse_last_abs_x[i] = raw.value;
					} else if (raw.code == ABS_Y) {
						if (mouse_last_abs_y[i] != -1 && mouse_last_abs_y[i] != raw.value)
							moved = 1;
						mouse_last_abs_y[i] = raw.value;
					}
				}

				if (moved) {
					mouse_moved = 1;
					uint64_t now = get_time_ms();
					if (mouse_fds[i] != last_active_mouse_fd || (now - last_hw_mouse_time > 1500)) {
						char mname[256] = "";
						ioctl(mouse_fds[i], EVIOCGNAME(sizeof mname), mname);
						if (mouse_fds[i] != last_active_mouse_fd) {
							fprintf(stderr, "[warpd] Active input switched to mouse: %s (%s)\n",
								mname[0] ? mname : "Unknown", mouse_names[i]);
						} else {
							fprintf(stderr, "[warpd] Hardware cursor movement detected on %s (%s)\n",
								mname[0] ? mname : "Unknown", mouse_names[i]);
						}
						last_active_mouse_fd = mouse_fds[i];
						last_hw_mouse_time = now;
					}
				}
			}
			if (n < 0 && (errno == ENODEV || errno == EBADF || errno == EIO)) {
				disconnected = 1;
			}
			if (disconnected) {
				fprintf(stderr, "[warpd] Device removed: mouse %s\n", mouse_names[i]);
				close(mouse_fds[i]);
				for (int k = i; k < nr_mice - 1; k++) {
					mouse_fds[k] = mouse_fds[k + 1];
					memcpy(mouse_names[k], mouse_names[k + 1], sizeof mouse_names[k]);
					mouse_last_abs_x[k] = mouse_last_abs_x[k + 1];
					mouse_last_abs_y[k] = mouse_last_abs_y[k + 1];
				}
				nr_mice--;
				continue;
			}
		}
		i++;
	}

	return mouse_moved;
}
