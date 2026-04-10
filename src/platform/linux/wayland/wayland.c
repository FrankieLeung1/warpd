/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */

#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include "wayland.h"

extern const char *input_event_tostr(struct input_event *ev);
extern const char *config_get(const char *key);
extern int saved_x;
extern int saved_y;
extern screen_t saved_scr;

#define UNIMPLEMENTED { \
	fprintf(stderr, "FATAL: wayland: %s unimplemented\n", __func__); \
	exit(-1);							 \
}

static int is_hyprland = 0;
static uint8_t btn_state[3] = {0};

static struct {
	const char *name;
	const char *xname;
} normalization_map[] = {
	{"esc", "Escape"},
	{",", "comma"},
	{".", "period"},
	{"-", "minus"},
	{"/", "slash"},
	{";", "semicolon"},
	{"$", "dollar"},
	{"backspace", "BackSpace"},
};

struct ptr ptr = {0};

/* Input */

uint8_t way_input_lookup_code(const char *name, int *shifted)
{
	size_t i;

	for (i = 0; i < sizeof normalization_map / sizeof normalization_map[0]; i++)
		if (!strcmp(normalization_map[i].name, name))
			name = normalization_map[i].xname;

	for (i = 0; i < 256; i++)
		if (!strcmp(keymap[i].name, name)) {
			*shifted = 0;
			return i;
		} else if (!strcmp(keymap[i].shifted_name, name)) {
			*shifted = 1;
			return i;
		}

	return 0;
}

const char *way_input_lookup_name(uint8_t code, int shifted)
{
	size_t i;
	const char *name = NULL;

	if (shifted && keymap[code].shifted_name[0])
		name = keymap[code].shifted_name;
	else if (!shifted && keymap[code].name[0])
		name = keymap[code].name;
	
	for (i = 0; i < sizeof normalization_map / sizeof normalization_map[0]; i++)
		if (name && !strcmp(normalization_map[i].xname, name))
			name = normalization_map[i].name;

	return name;
}

void way_mouse_move(struct screen *scr, int x, int y)
{
	size_t i;
	int maxx = INT_MIN;
	int maxy = INT_MIN;
	int minx = INT_MAX;
	int miny = INT_MAX;

	for (i = 0; i < nr_screens; i++) {
		int ex = screens[i].x + screens[i].w;
		int ey = screens[i].y + screens[i].h;

		if (screens[i].y < miny)
			miny = screens[i].y;
		if (screens[i].x < minx)
			minx = screens[i].x;

		if (ey > maxy)
			maxy = ey;
		if (ex > maxx)
			maxx = ex;
	}

	if (x == -1 && y == -1) {
		x = maxx - minx - scr->x;
		y = maxy - miny - scr->y;
	}

	ptr.x = x;
	ptr.y = y;
	ptr.scr = scr;

	/*
	 * Virtual pointer space always beings at 0,0, while global compositor
	 * space may have a negative real origin :/.
	 */
	zwlr_virtual_pointer_v1_motion_absolute(wl.ptr, 0,
						wl_fixed_from_int(x+scr->x-minx),
						wl_fixed_from_int(y+scr->y-miny),
						wl_fixed_from_int(maxx-minx),
						wl_fixed_from_int(maxy-miny));
	zwlr_virtual_pointer_v1_frame(wl.ptr);

	wl_display_flush(wl.dpy);
}

#define normalize_btn(btn) \
	switch (btn) { \
		case 1: btn = 272;break; \
		case 2: btn = 274;break; \
		case 3: btn = 273;break; \
	}

void way_mouse_down(int btn)
{
	assert(btn < (int)(sizeof btn_state / sizeof btn_state[0]));
	btn_state[btn-1] = 1;
	normalize_btn(btn);
	zwlr_virtual_pointer_v1_button(wl.ptr, 0, btn, 1);
	zwlr_virtual_pointer_v1_frame(wl.ptr);
	wl_display_flush(wl.dpy);
}

void way_mouse_up(int btn)
{
	assert(btn < (int)(sizeof btn_state / sizeof btn_state[0]));
	btn_state[btn-1] = 0;
	normalize_btn(btn);
	zwlr_virtual_pointer_v1_button(wl.ptr, 0, btn, 0);
	zwlr_virtual_pointer_v1_frame(wl.ptr);
	wl_display_flush(wl.dpy);
}

void way_mouse_click(int btn)
{
	normalize_btn(btn);

	uint32_t t = 0;
	zwlr_virtual_pointer_v1_button(wl.ptr, t, btn, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(wl.ptr);
	zwlr_virtual_pointer_v1_button(wl.ptr, t + 1, btn, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(wl.ptr);

	wl_display_flush(wl.dpy);
}

void way_mouse_get_position(struct screen **scr, int *x, int *y)
{
	if (scr)
		*scr = ptr.scr;
	if (x)
		*x = ptr.x;
	if (y)
		*y = ptr.y;

	// fprintf(stdout, "way_mouse_get_position %d %d\n", x ? *x : -1, y ? *y : -1);
}

void way_mouse_show()
{
	if (is_hyprland)
		system("hyprctl keyword cursor:invisible false >/dev/null 2>&1");
}

void way_mouse_hide()
{
	if (is_hyprland)
		system("hyprctl keyword cursor:invisible true >/dev/null 2>&1");
	else
		fprintf(stderr, "wayland: mouse hiding not implemented\n");
}

static uint32_t get_time_msec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void way_scroll(int direction)
{
	int axis = 0;
	int discrete = 0;

	switch (direction) {
	case SCROLL_DOWN:
		axis = 0;
		discrete = 1;
		break;
	case SCROLL_UP:
		axis = 0;
		discrete = -1;
		break;
	case SCROLL_RIGHT:
		axis = 1;
		discrete = 1;
		break;
	case SCROLL_LEFT:
		axis = 1;
		discrete = -1;
		break;
	default:
		return;
	}

	uint32_t t = get_time_msec();
	zwlr_virtual_pointer_v1_axis_source(wl.ptr, 0); // WL_POINTER_AXIS_SOURCE_WHEEL
	zwlr_virtual_pointer_v1_axis_discrete(wl.ptr, t, axis, wl_fixed_from_int(15 * discrete), discrete);
	zwlr_virtual_pointer_v1_frame(wl.ptr);

	wl_display_flush(wl.dpy);
}

void way_copy_selection() { UNIMPLEMENTED }

/*
 * Global shortcut state for daemon mode (Hyprland only).
 *
 * We register each activation key as a hyprland global shortcut.  The
 * compositor fires pressed/released events which we convert back into
 * struct input_event and return from way_input_wait().
 */

#define MAX_SHORTCUTS 16

struct shortcut_entry {
	struct hyprland_global_shortcut_v1 *shortcut;
	struct input_event event;  /* the activation event we should return */
	int pressed;               /* last pressed state from compositor */
};

static struct {
	struct shortcut_entry entries[MAX_SHORTCUTS];
	size_t n;
	int triggered;             /* index of triggered shortcut, or -1 */
} shortcuts;

static void shortcut_pressed(void *data,
			     struct hyprland_global_shortcut_v1 *shortcut,
			     uint32_t tv_sec_hi, uint32_t tv_sec_lo,
			     uint32_t tv_nsec)
{
	struct shortcut_entry *entry = data;
	entry->pressed = 1;
	shortcuts.triggered = entry - shortcuts.entries;
}

static void shortcut_released(void *data,
			      struct hyprland_global_shortcut_v1 *shortcut,
			      uint32_t tv_sec_hi, uint32_t tv_sec_lo,
			      uint32_t tv_nsec)
{
	struct shortcut_entry *entry = data;
	entry->pressed = 0;
}

static const struct hyprland_global_shortcut_v1_listener shortcut_listener = {
	.pressed = shortcut_pressed,
	.released = shortcut_released,
};

static const char *monitored_file;
static time_t monitored_mtime;

static time_t get_mtime(const char *path)
{
	struct stat st;
	if (stat(path, &st) < 0)
		return 0;
	return st.st_mtime;
}

struct input_event *way_input_wait(struct input_event *events, const char *names[], size_t sz)
{
	static struct input_event result;
	size_t i;

	if (!is_hyprland || !wl.shortcuts_manager) {
		fprintf(stderr,
			"FATAL: wayland daemon mode requires Hyprland with "
			"hyprland-global-shortcuts-v1 support.\n"
			"On other compositors, bind keys in your compositor config to "
			"run warpd --hint / --normal / --grid.\n");
		exit(1);
	}

	/* Destroy previous shortcuts if any. */
	for (i = 0; i < shortcuts.n; i++)
		if (shortcuts.entries[i].shortcut)
			hyprland_global_shortcut_v1_destroy(shortcuts.entries[i].shortcut);

	memset(&shortcuts, 0, sizeof shortcuts);
	shortcuts.triggered = -1;

	/* Register a global shortcut for each activation event. */
	for (i = 0; i < sz && i < MAX_SHORTCUTS; i++) {
		struct shortcut_entry *entry = &shortcuts.entries[i];

		entry->event = events[i];

		entry->shortcut =
			hyprland_global_shortcuts_manager_v1_register_shortcut(
				wl.shortcuts_manager,
				names[i], "warpd",
				names[i],
				config_get(names[i]));

		hyprland_global_shortcut_v1_add_listener(
			entry->shortcut, &shortcut_listener, entry);
	}
	shortcuts.n = i;

	wl_display_flush(wl.dpy);

	/* Record baseline mtime for monitored config file. */
	if (monitored_file)
		monitored_mtime = get_mtime(monitored_file);


	fprintf(stdout, "warpd: waiting for global shortcuts\n");

	way_input_open_mice();

	/* Event loop: wait for a shortcut press or config file change. */
	while (1) {
		struct pollfd pfd;

		pfd.fd = wl_display_get_fd(wl.dpy);
		pfd.events = POLLIN;

		wl_display_flush(wl.dpy);
		wl_display_dispatch_pending(wl.dpy);

		if (!poll(&pfd, 1, 500))  {
			/* Timeout — check for config file changes. */
			if (monitored_file &&
			    get_mtime(monitored_file) != monitored_mtime) {
				way_input_close_mice();
				return NULL;
			}

			/*
			 * If the cursor was hidden (saved_scr set), check for
			 * physical mouse movement via evdev.  Restore and show
			 * the cursor when the user moves their mouse.
			 */
			if (saved_scr && way_input_poll_mice(0)) {
				way_mouse_move(saved_scr, saved_x, saved_y);
				way_mouse_show();
				saved_scr = NULL;
			}

			continue;
		}

		if (pfd.revents & POLLIN)
			wl_display_dispatch(wl.dpy);

		if (shortcuts.triggered >= 0) {
			int idx = shortcuts.triggered;
			result = shortcuts.entries[idx].event;
			result.pressed = 1;
			shortcuts.triggered = -1;
			fprintf(stdout, "warpd: shortcut triggered: %s (%s)\n",
				names[idx], input_event_tostr(&result));
			way_input_close_mice();
			return &result;
		}
	}
}

void way_screen_list(struct screen *scr[MAX_SCREENS], size_t *n)
{
	size_t i;
	for (i = 0; i < nr_screens; i++)
		scr[i] = &screens[i];

	*n = nr_screens;
}

void way_monitor_file(const char *path)
{
	monitored_file = path;
}
void way_commit() { }

static void cleanup()
{
	if (!wl.dpy)
		return;

	way_mouse_show();

	if (btn_state[0])
		zwlr_virtual_pointer_v1_button(wl.ptr, 0, 272, 0);
	if (btn_state[1])
		zwlr_virtual_pointer_v1_button(wl.ptr, 0, 274, 0);
	if (btn_state[2])
		zwlr_virtual_pointer_v1_button(wl.ptr, 0, 273, 0);

	if (wl.ptr) {
		zwlr_virtual_pointer_v1_destroy(wl.ptr);
		wl.ptr = NULL;
	}

	wl_display_flush(wl.dpy);
	wl_display_disconnect(wl.dpy);
	wl.dpy = NULL;
}

static void sig_handler(int sig)
{
	cleanup();
	exit(0);
}

void wayland_init(struct platform *platform)
{
	is_hyprland = getenv("HYPRLAND_INSTANCE_SIGNATURE") != NULL;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	way_init();
	atexit(cleanup);

	platform->monitor_file = way_monitor_file;
	platform->commit = way_commit;
	platform->copy_selection = way_copy_selection;
	platform->hint_draw = way_hint_draw;
	platform->init_hint = way_init_hint;
	platform->input_grab_keyboard = way_input_grab_keyboard;
	platform->input_lookup_code = way_input_lookup_code;
	platform->input_lookup_name = way_input_lookup_name;
	platform->input_next_event = way_input_next_event;
	platform->input_ungrab_keyboard = way_input_ungrab_keyboard;
	platform->input_suspend_keyboard = way_input_suspend_keyboard;
	platform->input_resume_keyboard = way_input_resume_keyboard;
	platform->input_wait = way_input_wait;
	platform->mouse_click = way_mouse_click;
	platform->mouse_down = way_mouse_down;
	platform->mouse_get_position = way_mouse_get_position;
	platform->mouse_hide = way_mouse_hide;
	platform->mouse_move = way_mouse_move;
	platform->mouse_show = way_mouse_show;
	platform->mouse_up = way_mouse_up;
	platform->screen_clear = way_screen_clear;
	platform->screen_draw_box = way_screen_draw_box;
	platform->screen_get_dimensions = way_screen_get_dimensions;
	platform->screen_list = way_screen_list;
	platform->scroll = way_scroll;
}
