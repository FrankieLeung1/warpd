/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#include "warpd.h"

static void move(screen_t scr, int x, int y)
{
	cursor_draw(scr, x, y);
	mouse_move(scr, x, y);
}

struct input_event *normal_mode(struct input_event *start_ev)
{
	struct input_event *ev;
	screen_t	    scr;
	int		    sh, sw;
	int		    cy, cx;

	input_grab_keyboard();

	mouse_get_position(&scr, &cx, &cy);
	screen_get_dimensions(scr, &sw, &sh);
	cursor_draw(scr, cx, cy);

	mouse_hide();
	mouse_reset();

	while (1) {
		const int cursz = cfg->cursor_size;

		if (start_ev == NULL) {
			ev = input_next_event(10);
		} else {
			ev = start_ev;
			start_ev = NULL;
		}

		scroll_tick();
		if (mouse_process_key(ev, cfg->up, cfg->down, cfg->left, cfg->right)) {
			continue;
		}

		mouse_get_position(&scr, &cx, &cy);

		if (input_event_eq(ev, cfg->scroll_down)) {
			if (ev->pressed) {
				scroll_stop();
				scroll_accelerate(SCROLL_DOWN);
			} else
				scroll_decelerate();
		} else if (input_event_eq(ev, cfg->scroll_up)) {
			if (ev->pressed) {
				scroll_stop();
				scroll_accelerate(SCROLL_UP);
			} else
				scroll_decelerate();
		} else if (!ev->pressed) {
			goto next;
		}

		if (input_event_eq(ev, cfg->top))
			move(scr, cx, 0);
		else if (input_event_eq(ev, cfg->bottom))
			move(scr, cx, sh - cursz);
		else if (input_event_eq(ev, cfg->middle))
			move(scr, cx, sh / 2);
		else if (input_event_eq(ev, cfg->start))
			move(scr, 0, cy);
		else if (input_event_eq(ev, cfg->end))
			move(scr, sw - cursz, cy);
		else if (input_event_eq(ev, cfg->hist_back)) {
			hist_add(cx, cy);
			hist_prev();
			hist_get(&cx, &cy);

			move(scr, cx, cy);
		} else if (input_event_eq(ev, cfg->hist_forward)) {
			hist_next();
			hist_get(&cx, &cy);

			move(scr, cx, cy);
		} else if (input_event_eq(ev, cfg->drag)) {
			toggle_drag();
		} else if (input_event_eq(ev, cfg->copy_and_exit)) {
			copy_selection();
			ev = NULL;
			goto exit;
		} else if (input_event_eq(ev, cfg->exit) ||
			   input_event_eq(ev, cfg->grid) ||
			   input_event_eq(ev, cfg->hint)) {
			goto exit;
		} else { /* Mouse Buttons. */
			size_t i;

			for (i = 0; i < 3; i++) {
				const int btn = i + 1;
				int	  oneshot = 0;
				int	  match = 0;

				if (input_event_eq(ev,
						   cfg->oneshot_buttons[i])) {
					match = 1;
					oneshot = 1;
				} else if (input_event_eq(ev, cfg->buttons[i]))
					match = 1;

				if (match) {
					hist_add(cx, cy);

					mouse_click(btn);

					if (oneshot) {
						const int timeout = cfg->oneshot_timeout;
						int timeleft = timeout;

						while (timeleft--) {
							struct input_event *ev = input_next_event(1);
							if (ev && ev->pressed &&
							    input_event_eq(ev,cfg->oneshot_buttons [i])) {
								mouse_click(btn);
								timeleft = timeout;
							}
						}

						ev = NULL;
						goto exit;
					}
				}
			}
		}
	next:
		mouse_get_position(&scr, &cx, &cy);

		platform_commit();
	}

exit:
	hist_add(cx, cy);

	mouse_show();
	cursor_hide();

	input_ungrab_keyboard();

	platform_commit();
	return ev;
}

void init_normal_mode()
{
	init_cursor(cfg->cursor_color, cfg->cursor_size);
	cursor_hide();
}
