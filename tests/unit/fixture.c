// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

#include <stdarg.h>

#define TEST_MAIN
#include "fixture.h"

#define MAX_WINDOWS 64

struct ws_state wsd;
char test_log[512];
struct test_start test_starts[64];
unsigned test_n_starts;
static uint64_t now_us;

void ws_log(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(test_log, sizeof(test_log), fmt, args);
	va_end(args);
}

uint64_t ws_now_us(void) {
	return now_us;
}

static bool fake_start_animation(uint32_t window, const char *trigger) {
	if (test_n_starts < sizeof(test_starts) / sizeof(test_starts[0])) {
		test_starts[test_n_starts++] = (struct test_start){window, trigger};
	}
	return true;
}

static const struct picom_api fake_api = {.start_animation = fake_start_animation};

void fx_reset(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		for (unsigned k = 0; k < wsd.tracked[i].n_visits; k++) {
			free(wsd.tracked[i].visits[k].name);
		}
	}
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		free(wsd.clients[i].workspace);
	}
	free(wsd.clients);
	// As plugin.c starts it, with a clock that is not 0: a time of 0 means unknown.
	wsd = (struct ws_state){
	    .api = &fake_api,
	    .screen = {.x2 = 4520, .y2 = 1920},
	    .current = -1,
	    .frame = 1,
	    .frame_time_us = 1000000,
	    .read_frame = UINT64_MAX,
	    .clients = calloc(MAX_WINDOWS, sizeof(struct ws_client)),
	};
	if (wsd.clients == NULL) {
		abort();
	}
	now_us = wsd.frame_time_us;
	test_log[0] = '\0';
	test_n_starts = 0;
}

int fx_monitor(struct ws_rect r) {
	wsd.monitors[wsd.n_monitors] = r;
	return wsd.n_monitors++;
}

unsigned fx_workspace(const char *name, int monitor) {
	auto d = wsd.n_names++;
	wsd.names[d] = name;
	wsd.viewports[d * 2] = (uint32_t)wsd.monitors[monitor].x1;
	wsd.viewports[d * 2 + 1] = (uint32_t)wsd.monitors[monitor].y1;
	wsd.n_viewports = wsd.n_names * 2;
	return d;
}

struct ws_client *fx_window(xcb_window_t id, int64_t desktop, struct ws_rect r) {
	if (wsd.n_clients == MAX_WINDOWS) {
		abort();
	}
	auto c = &wsd.clients[wsd.n_clients++];
	*c = (struct ws_client){
	    .id = id,
	    .desktop = desktop,
	    .rect = r,
	    .workspace = desktop >= 0 ? strdup(wsd.names[desktop]) : NULL,
	};
	return c;
}

struct ws_client *fx_window_on(xcb_window_t id, unsigned desktop) {
	auto monitor = ws_desktop_monitor(desktop);
	auto m = monitor >= 0 ? wsd.monitors[monitor] : wsd.screen;
	int32_t x = (m.x1 + m.x2) / 2, y = (m.y1 + m.y2) / 2;
	return fx_window(
	    id, desktop,
	    (struct ws_rect){.x1 = x - 100, .y1 = y - 50, .x2 = x + 100, .y2 = y + 50});
}

void fx_switch(unsigned desktop) {
	wsd.current = desktop;
	ws_track_switch(true);
}

void fx_pass(uint64_t us) {
	wsd.frame++;
	wsd.frame_time_us += us;
	now_us = wsd.frame_time_us;
}

struct ws_order fx_order(unsigned desktop) {
	return ws_order_of(wsd.names[desktop], desktop);
}

struct ws_monitor *fx_tracked(unsigned desktop) {
	return ws_find_monitor(desktop);
}

struct fx_values fx_compute(xcb_window_t id, const char *trigger) {
	struct picom_animation_start start = {
	    .trigger = trigger, .window = id, .frame = wsd.frame};
	// The frame callback read the root window and the windows in this pass already.
	wsd.read_frame = wsd.frame;
	double v[WS_N_VARIABLES];
	ws_compute(&start, v);
	return (struct fx_values){
	    .direction = v[WS_VAR_DIRECTION],
	    .distance = v[WS_VAR_DISTANCE],
	    .x = v[WS_VAR_MONITOR_X],
	    .y = v[WS_VAR_MONITOR_Y],
	    .width = v[WS_VAR_MONITOR_WIDTH],
	    .height = v[WS_VAR_MONITOR_HEIGHT],
	    .faces = v[WS_VAR_FACES],
	    .face = v[WS_VAR_FACE],
	    .selected = v[WS_VAR_SELECTED],
	    .placed = v[WS_VAR_PLACED],
	};
}
