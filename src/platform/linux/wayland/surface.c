/*
 * keyd - A key remapping daemon.
 *
 * © 2019 Raheman Vaiya (see also: LICENSE).
 */
#include "wayland.h"

static void noop() {}

/* A 'surface' in the local context can be thought of as a viewport into a
 * rectangular region of the screen's backing buffer (the wl_shm_pool).  It is
 * undergirded by a corresponding wayland surface and wayland layer surface with a
 * wayland buffer object created from the relevant part of the screen's memory
 * pool. Surfaces are visible as long as they exist, and hiding them is achieved
 * by destroying them.
 */
struct surface {
	struct zwlr_layer_surface_v1 *wl_layer_surface;
	struct wl_surface *wl_surface;
	struct wl_buffer *wl_buffer;
	struct wp_viewport *wl_viewport;

	int configured;
	int destroyed;
	int pointer_passthrough;
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1
					   *layer_surface, uint32_t serial,
					   uint32_t width, uint32_t height)
{
	struct surface *sfc = data;

	// Notify the server that we are still alive
	// (i.e a heartbeat).
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

	// The protocol requires us to wait for the first configure call before
	// attaching the buffer to the underlying wl_surface object for the
	// first time.

	wl_surface_attach(sfc->wl_surface, sfc->wl_buffer, 0, 0);
	wl_surface_commit(sfc->wl_surface);

	sfc->configured = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
        .configure = layer_surface_handle_configure,
        .closed = noop,
};

void destroy_surface(struct surface *sfc)
{
	if (sfc) {
		if (sfc->wl_viewport)
			wp_viewport_destroy(sfc->wl_viewport);
		zwlr_layer_surface_v1_destroy(sfc->wl_layer_surface);
		wl_surface_destroy(sfc->wl_surface);
		wl_buffer_destroy(sfc->wl_buffer);

		free(sfc);
	}
}

/* All surfaces which exist are visible. Surface creation/destruction is the means
 * of displaying content on the screen (there is no show/hide) and should be considered a cheap operation
 * which operates on a persistent shared buffer (memory pool). */

struct surface *create_surface(struct screen *scr, int x, int y, int w, int h, int capture_input, int passthrough)
{
	struct surface *sfc = calloc(1, sizeof (struct surface));
	int s120 = scr->scale120;
	int px, py, pw, ph;

	if (x < 0) {
		x = 0;
		w += x;
	}
	if (y < 0) {
		y = 0;
		h += y;
	}
	if ((x+w) > scr->w)
		x = scr->w-w;
	if ((y+h) > scr->h)
		y = scr->h-h;

	/* Convert logical coordinates to physical (buffer) coordinates */
	px = (x * s120 + 119) / 120;
	py = (y * s120 + 119) / 120;
	pw = (w * s120 + 119) / 120;
	ph = (h * s120 + 119) / 120;

	sfc->wl_buffer = wl_shm_pool_create_buffer(scr->wl_pool, py*scr->stride + px*4, pw, ph, scr->stride, WL_SHM_FORMAT_ARGB8888);
	assert(sfc->wl_buffer);
	sfc->wl_surface = wl_compositor_create_surface(wl.compositor);

	assert(sfc->wl_surface);
	sfc->wl_layer_surface =
		zwlr_layer_shell_v1_get_layer_surface(wl.layer_shell, sfc->wl_surface,
						      scr->wl_output,
						      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
						      "warpd");

	assert(sfc->wl_layer_surface);

	zwlr_layer_surface_v1_set_size(sfc->wl_layer_surface, w, h);
	zwlr_layer_surface_v1_set_anchor(sfc->wl_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_margin(sfc->wl_layer_surface, y, 0, 0, x);
	zwlr_layer_surface_v1_set_exclusive_zone(sfc->wl_layer_surface, -1);

	/* Use viewporter to map the physical buffer to logical surface size */
	if (wl.viewporter) {
		sfc->wl_viewport = wp_viewporter_get_viewport(wl.viewporter, sfc->wl_surface);
		wp_viewport_set_destination(sfc->wl_viewport, w, h);
	}

	zwlr_layer_surface_v1_add_listener(sfc->wl_layer_surface, &layer_surface_listener, sfc);

	sfc->configured = 0;

	if (capture_input) {
		zwlr_layer_surface_v1_set_keyboard_interactivity(sfc->wl_layer_surface,
								  ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	}

	sfc->pointer_passthrough = passthrough;

	wl_surface_commit(sfc->wl_surface);

	return sfc;
}

void surface_set_pointer_passthrough(struct surface *sfc)
{
	struct wl_region *region = wl_compositor_create_region(wl.compositor);
	wl_surface_set_input_region(sfc->wl_surface, region);
	wl_region_destroy(region);
	wl_surface_commit(sfc->wl_surface);
}

struct wl_surface *surface_get_wl_surface(struct surface *sfc)
{
	return sfc->wl_surface;
}
