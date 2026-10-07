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
static int is_fast = 0;

static float get_speed_factor()
{
	int cfg = config_get_int("scroll_speed_factor");
	if (cfg <= 0)
		return 3.0f;
	if (cfg <= 10)
		return (float)cfg;
	return (float)cfg / 100.0f;
}

void scroll_set_fast(int fast)
{
	if (is_fast == (fast ? 1 : 0))
		return;

	is_fast = fast ? 1 : 0;
	float mult = get_speed_factor();

	if (is_fast) {
		warpd_debug("Scroll fast mode enabled (factor=%.2f)", mult);
		if (v > 0)
			v *= mult;
		if (a > 0)
			a = a0 * mult;
		else if (a < 0)
			a = da0 * mult;
	} else {
		warpd_debug("Scroll fast mode disabled");
		if (v > 0) {
			v /= mult;
			if (v > vt)
				v = vt;
		}
		if (a > 0)
			a = a0;
		else if (a < 0)
			a = da0;
	}
}

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

	float mult = is_fast ? get_speed_factor() : 1.0f;
	float cur_vt = vt * mult;
	if (v >= cur_vt) {
		v = cur_vt;
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
	float mult = is_fast ? get_speed_factor() : 1.0f;
	warpd_debug("Scroll decelerate called: da0=%.2f (current v=%.2f, is_fast=%d)", da0 * mult, v, is_fast);
	a = da0 * mult;
}

void scroll_accelerate(int _direction)
{
	direction = _direction;
	float mult = is_fast ? get_speed_factor() : 1.0f;
	a = a0 * mult;
	warpd_debug("Scroll accelerate called: direction=%s, is_fast=%d, initial_v=%.2f, a0=%.2f",
		direction_str(_direction), is_fast, v0 * mult, a);

	if (v == 0) {
		v = v0 * mult;
		last_tick_us = get_time_us();
		last_scroll_us = last_tick_us;
		traveled_accum = 0;
		active_scrolling = 0;

		if (platform->input_suspend_keyboard)
			platform->input_suspend_keyboard();

		if (!platform->scroll_continuous) {
			/* In discrete mode, emit the initial single click on key press */
			int initial_clicks = (int)mult;
			if (initial_clicks < 1)
				initial_clicks = 1;
			for (int i = 0; i < initial_clicks; i++) {
				traveled_accum += 1.0;
				active_scrolling = 1;
				warpd_debug("Scroll first tap emitted (discrete): direction=%s, v=%.2f", direction_str(direction), v);
				platform->scroll(direction);
			}
		}
	}
}

void scroll_impart_impulse() { v += fling_velocity; }

