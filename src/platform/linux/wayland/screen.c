/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */
#include "wayland.h"

static void noop() {}

static void xdg_output_handle_logical_position(void *data,
					       struct zxdg_output_v1
					       *zxdg_output_v1, int32_t x,
					       int32_t y)
{
	struct screen *scr = data;

	scr->x = x;
	scr->y = y;
	scr->state++;
}

static void xdg_output_handle_logical_size(void *data,
					   struct zxdg_output_v1
					   *zxdg_output_v1, int32_t w,
					   int32_t h)
{
	struct screen *scr = data;

	scr->w = w;
	scr->h = h;
	scr->state++;
}

static struct zxdg_output_v1_listener zxdg_output_v1_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.done = noop,
	.name = noop,
	.description = noop,
};

static void handle_pointer_enter(void *data,
				 struct wl_pointer *wl_pointer,
				 uint32_t serial,
				 struct wl_surface *surface,
				 wl_fixed_t wlx, wl_fixed_t wly)
{
	int i;

	if (!ptr.scr) {
		ptr.x = wl_fixed_to_int(wlx);
		ptr.y = wl_fixed_to_int(wly);

		for (i = 0; i < nr_screens; i++) {
			struct screen *scr = &screens[i];
			if (scr->overlay && surface == surface_get_wl_surface(scr->overlay))
				ptr.scr = scr;
		}
	}
}

static struct wl_pointer_listener wl_pointer_listener = {
	.enter = handle_pointer_enter,
	.leave = noop,
	.motion = noop,
	.button = noop,
	.axis = noop,
	.frame = noop,
	.axis_source = noop,
	.axis_stop = noop,
	.axis_discrete = noop,
};

/* 
 * Register a pointer_listener and listen for enter events after
 * creating a full screen surface for each screen in order to capture the initial
 * cursor position. I couldn't find a better way to achieve this :/.
 */
static void discover_pointer_location()
{
	size_t i;
	int attempts = 0;
	int wl_fd;
	struct pollfd pfd;

	wl_pointer_add_listener(wl_seat_get_pointer(wl.seat), &wl_pointer_listener, NULL);

	for (i = 0; i < nr_screens; i++) {
		struct screen *scr = &screens[i];
		scr->overlay = create_surface(scr, 0, 0, scr->w, scr->h, 0, 0);
	}

	wl_fd = wl_display_get_fd(wl.dpy);

	while (!ptr.scr && attempts < 10) {
		/*
		 * Agitate the pointer to precipitate an entry
		 * event. Hyprland appears to require this for
		 * some reason.
		 */
		zwlr_virtual_pointer_v1_motion(wl.ptr, 0,
					       wl_fixed_from_int(1),
					       wl_fixed_from_int(1));
		wl_display_flush(wl.dpy);

		pfd.fd = wl_fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 100) > 0)
			wl_display_dispatch(wl.dpy);
		else
			wl_display_dispatch_pending(wl.dpy);

		attempts++;
	}

	if (!ptr.scr) {
		fprintf(stderr, "WARNING: Could not discover pointer location, defaulting to screen 0\n");
		ptr.scr = &screens[0];
		ptr.x = screens[0].w / 2;
		ptr.y = screens[0].h / 2;
	}

	for (i = 0; i < nr_screens; i++) {
		struct screen *scr = &screens[i];
		destroy_surface(scr->overlay);
		scr->overlay = NULL;
	}
}

void add_screen(struct wl_output *output)
{
	struct screen *scr = &screens[nr_screens++];
	scr->overlay = NULL;
	scr->wl_output = output;
}

void way_screen_draw_box(struct screen *scr, int x, int y, int w, int h, const char *color)
{
	uint8_t r, g, b, a;

	assert(scr->nr_boxes < MAX_BOXES);

	way_hex_to_rgba(color, &r, &g, &b, &a);
	cairo_set_source_rgba(scr->cr, r / 255.0, g / 255.0, b / 255.0, a / 255.0);
	cairo_rectangle(scr->cr, x, y, w, h);
	cairo_fill(scr->cr);

	scr->boxes[scr->nr_boxes++] = create_surface(scr, x, y, w, h, 0, 1);
}


void way_screen_get_dimensions(struct screen *scr, int *w, int *h)
{
	*w = scr->w;
	*h = scr->h;
}

void way_screen_clear(struct screen *scr)
{
	size_t i;
	for (i = 0; i < scr->nr_boxes; i++)
		destroy_surface(scr->boxes[i]);

	destroy_surface(scr->hints);

	scr->nr_boxes = 0;
	scr->hints = NULL;
}

static void init_screen_pool(struct screen *scr)
{
	int fd;
	static int shm_num = 0;
	char shm_path[64];
	size_t bufsz;
	char *buf;
	cairo_surface_t *cairo_surface;

	scr->pw = (scr->w * scr->scale120 + 119) / 120;
	scr->ph = (scr->h * scr->scale120 + 119) / 120;

	scr->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, scr->pw);

	bufsz = scr->stride * scr->ph + scr->pw * 4;
	sprintf(shm_path, "/warpd_%d", shm_num++);

	fd = shm_open(shm_path, O_CREAT|O_TRUNC|O_RDWR, 0600);
	if (fd < 0) {
		perror("shm_open");
		exit(-1);
	}

	ftruncate(fd, bufsz);

	scr->wl_pool = wl_shm_create_pool(wl.shm, fd, bufsz);
	buf = mmap(NULL, bufsz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	cairo_surface = cairo_image_surface_create_for_data(buf,
							    CAIRO_FORMAT_ARGB32, scr->pw,
							    scr->ph, scr->stride);
	scr->cr = cairo_create(cairo_surface);
	cairo_scale(scr->cr, scr->scale120 / 120.0, scr->scale120 / 120.0);
}

static void handle_preferred_scale(void *data,
				   struct wp_fractional_scale_v1 *fs,
				   uint32_t scale)
{
	struct screen *scr = data;
	scr->scale120 = scale;
	scr->state++;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
	.preferred_scale = handle_preferred_scale,
};

struct tmp_scale_surface {
	struct wl_surface *wl_surface;
	struct wl_buffer *wl_buffer;
};

static void tmp_layer_configure(void *data,
				struct zwlr_layer_surface_v1 *layer_surface,
				uint32_t serial, uint32_t width, uint32_t height)
{
	struct tmp_scale_surface *tmp = data;

	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	wl_surface_attach(tmp->wl_surface, tmp->wl_buffer, 0, 0);
	wl_surface_commit(tmp->wl_surface);
}

/*
 * Discover the fractional scale for a screen by creating a temporary
 * surface, attaching a fractional_scale listener, and waiting for the
 * preferred_scale event.
 */
static void discover_screen_scale(struct screen *scr)
{
	struct wl_surface *tmp_surface;
	struct zwlr_layer_surface_v1 *tmp_layer;
	struct wp_fractional_scale_v1 *fs;
	struct wl_shm_pool *tmp_pool;
	struct wl_buffer *tmp_buffer;
	struct tmp_scale_surface tmp_data;
	int prev_state = scr->state;
	int fd;

	if (!wl.fractional_scale_manager || !wl.viewporter) {
		scr->scale120 = 120;
		return;
	}

	fd = shm_open("/warpd_scale_tmp", O_CREAT|O_TRUNC|O_RDWR, 0600);
	if (fd < 0) {
		scr->scale120 = 120;
		return;
	}
	ftruncate(fd, 4);
	tmp_pool = wl_shm_create_pool(wl.shm, fd, 4);
	tmp_buffer = wl_shm_pool_create_buffer(tmp_pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(tmp_pool);
	close(fd);
	shm_unlink("/warpd_scale_tmp");

	tmp_surface = wl_compositor_create_surface(wl.compositor);

	tmp_data.wl_surface = tmp_surface;
	tmp_data.wl_buffer = tmp_buffer;

	tmp_layer = zwlr_layer_shell_v1_get_layer_surface(wl.layer_shell,
							  tmp_surface,
							  scr->wl_output,
							  ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
							  "warpd-scale");

	zwlr_layer_surface_v1_set_size(tmp_layer, 1, 1);
	zwlr_layer_surface_v1_set_exclusive_zone(tmp_layer, -1);
	zwlr_layer_surface_v1_add_listener(tmp_layer,
		&(struct zwlr_layer_surface_v1_listener){
			.configure = tmp_layer_configure,
			.closed = noop,
		}, &tmp_data);

	fs = wp_fractional_scale_manager_v1_get_fractional_scale(
		wl.fractional_scale_manager, tmp_surface);
	wp_fractional_scale_v1_add_listener(fs, &fractional_scale_listener, scr);

	wl_surface_commit(tmp_surface);

	while (scr->state == prev_state)
		wl_display_dispatch(wl.dpy);

	wp_fractional_scale_v1_destroy(fs);
	zwlr_layer_surface_v1_destroy(tmp_layer);
	wl_surface_destroy(tmp_surface);
	wl_buffer_destroy(tmp_buffer);
}

void init_screen()
{
	size_t i;

	for (i = 0; i < nr_screens; i++) {
		struct screen *scr = &screens[i];

		scr->xdg_output =
		    zxdg_output_manager_v1_get_xdg_output(wl.xdg_output_manager,
							  scr->wl_output);

		zxdg_output_v1_add_listener(scr->xdg_output,
					    &zxdg_output_v1_listener, scr);

		scr->state = 0;
		do {
			wl_display_dispatch(wl.dpy);
		} while (scr->state != 2);

		discover_screen_scale(scr);

		scr->ptrx = -1;
		scr->ptry = -1;

		init_screen_pool(scr);
	}

	discover_pointer_location();
}
