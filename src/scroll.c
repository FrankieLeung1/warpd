/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#include "warpd.h"
#include <stdio.h>

#ifdef __APPLE__
#define factor 1
#else
#define factor 50
#endif

#define fling_velocity (2000.0 / factor);

/* terminal velocity */
#define vt ((float)config_get_int("scroll_max_speed") / factor)
#define v0 ((float)config_get_int("scroll_speed") / factor)
#define da0                                                                    \
	((float)config_get_int("scroll_deceleration") /                        \
	 factor) /* deceleration */
#define a0 ((float)config_get_int("scroll_acceleration") / factor)

static uint64_t last_tick_us = 0;
static uint64_t last_scroll_us = 0;

/* in scroll units per second. */
static float v = 0;
static float a = 0;

static int direction = 0;
static double traveled_accum = 0; /* fractional scroll clicks accumulated */
static int active_scrolling = 0;

static const char *direction_str(int dir)
{
	switch (dir) {
	case SCROLL_DOWN: return "DOWN";
	case SCROLL_UP: return "UP";
	case SCROLL_LEFT: return "LEFT";
	case SCROLL_RIGHT: return "RIGHT";
	case SCROLL_STOP: return "STOP";
	default: return "UNKNOWN";
	}
}

void scroll_tick()
{
	uint64_t now = get_time_us();
	if (v <= 0)
		return;

	float dt = (float)(now - last_tick_us) / 1000000.0f;
	last_tick_us = now;

	v += a * dt;

	if (v <= 0) {
		if (active_scrolling) {
			warpd_debug("Scroll tick stopped: direction=%s, total_clicks=%.2f",
				direction_str(direction), traveled_accum);
			platform->scroll(SCROLL_STOP);
		}

		v = 0;
		traveled_accum = 0;
		active_scrolling = 0;
		last_scroll_us = 0;

		if (platform->input_resume_keyboard)
			platform->input_resume_keyboard();
		return;
	}

	if (v >= vt) {
		v = vt;
		a = 0;
	}

	if (v > 0) {
		if (platform->scroll_continuous) {
			/* Continuous smooth scrolling directly proportional to velocity */
			double clicks = (double)v * (double)dt;
			if (clicks > 0) {
				traveled_accum += clicks;
				active_scrolling = 1;
				platform->scroll_continuous(direction, clicks);
			}
		} else {
			/* Discrete fallback using pacemaker intervals */
			int cfg_interval = config_get_int("scroll_interval");
			uint64_t interval_us;

			if (cfg_interval > 0) {
				interval_us = (uint64_t)cfg_interval * 1000;
			} else {
				interval_us = (uint64_t)(1000000.0f / v);
				if (interval_us == 0)
					interval_us = 1000;

				interval_us = ((interval_us + 10000) / 20000) * 20000;
				if (interval_us == 0)
					interval_us = 20000;
			}

			if (now - last_scroll_us >= interval_us) {
				if (now - last_scroll_us > interval_us * 2)
					last_scroll_us = now - interval_us;

				while (now - last_scroll_us >= interval_us && v > 0) {
					last_scroll_us += interval_us;
					traveled_accum += 1.0;
					active_scrolling = 1;
					warpd_debug("Scroll tick emitted (discrete): direction=%s, total=%.0f, v=%.2f",
						direction_str(direction), traveled_accum, v);
					platform->scroll(direction);
				}
			}
		}
	}
}

void scroll_stop()
{
	warpd_debug("Scroll stop called: direction=%s, v=%.2f, active=%d", direction_str(direction), v, active_scrolling);
	if (v > 0 && platform->input_resume_keyboard)
		platform->input_resume_keyboard();

	if (active_scrolling)
		platform->scroll(SCROLL_STOP);

	v = 0;
	a = 0;
	traveled_accum = 0;
	active_scrolling = 0;
	last_scroll_us = 0;
}

void scroll_decelerate()
{
	warpd_debug("Scroll decelerate called: da0=%.2f (current v=%.2f)", da0, v);
	a = da0;
}

void scroll_accelerate(int _direction)
{
	direction = _direction;
	a = a0;
	warpd_debug("Scroll accelerate called: direction=%s, initial_v=%.2f, a0=%.2f", direction_str(_direction), v0, a0);

	if (v == 0) {
		v = v0;
		last_tick_us = get_time_us();
		last_scroll_us = last_tick_us;
		traveled_accum = 1.0;
		active_scrolling = 1;

		if (platform->input_suspend_keyboard)
			platform->input_suspend_keyboard();

		warpd_debug("Scroll first tap emitted: direction=%s, v=%.2f", direction_str(direction), v);
		platform->scroll(direction);
	}
}

void scroll_impart_impulse() { v += fling_velocity; }

