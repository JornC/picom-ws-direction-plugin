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
/// The windows as the last read saw them, and the events since.
static struct ws_client *read_clients;
static unsigned n_read_clients;
static struct ws_event events[256];
static unsigned n_events;
static int64_t read_current;
static uint32_t sequence;

void ws_log(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(test_log, sizeof(test_log), fmt, args);
	va_end(args);
}

uint64_t ws_now_us(void) {
	return now_us;
}

void ws_take_events(void) {
}

static bool fake_start_animation(uint32_t window, const char *trigger) {
	if (test_n_starts < sizeof(test_starts) / sizeof(test_starts[0])) {
		test_starts[test_n_starts++] = (struct test_start){window, trigger};
	}
	return true;
}

static bool fake_window_has_image([[maybe_unused]] uint32_t window) {
	return true;
}

static const struct picom_api fake_api = {
    .start_animation = fake_start_animation,
    .window_has_image = fake_window_has_image,
};

static void free_clients(struct ws_client *clients, unsigned n) {
	for (unsigned i = 0; i < n; i++) {
		free(clients[i].workspace);
	}
	free(clients);
}

/// Remove window `id` from the windows the window manager manages: it closed.
void fx_close(xcb_window_t id) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		if (wsd.clients[i].id == id) {
			free(wsd.clients[i].workspace);
			wsd.clients[i] = wsd.clients[--wsd.n_clients];
			return;
		}
	}
	abort();
}

static struct ws_client *copy_clients(const struct ws_client *clients, unsigned n) {
	struct ws_client *ret = calloc(MAX_WINDOWS, sizeof(*ret));
	if (ret == NULL) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		ret[i] = clients[i];
		ret[i].workspace = clients[i].workspace ? strdup(clients[i].workspace) : NULL;
	}
	return ret;
}

void fx_reset(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		ws_free_monitor(&wsd.tracked[i]);
	}
	free_clients(wsd.clients, wsd.n_clients);
	free_clients(read_clients, n_read_clients);
	free_clients(wsd.gone, wsd.n_gone);
	free(wsd.starts);
	read_clients = NULL;
	n_read_clients = 0;
	n_events = 0;
	read_current = -1;
	// As plugin.c starts it, with a clock that is not 0.
	wsd = (struct ws_state){
	    .api = &fake_api,
	    .screen = {.x2 = 4520, .y2 = 1920},
	    .screen_width_mm = 1196,
	    .screen_height_mm = 508,
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
	wsd.monitors[wsd.n_monitors] = (struct ws_output){
	    .name = wsd.n_monitors + 1,
	    .rect = r,
	    .width_mm = (uint32_t)(r.x2 - r.x1) / 4,
	    .height_mm = (uint32_t)(r.y2 - r.y1) / 4,
	};
	auto ret = wsd.n_monitors++;
	ws_update_viewport_monitors();
	return ret;
}

unsigned fx_workspace(const char *name, int monitor) {
	auto d = wsd.n_names++;
	wsd.names[d] = name;
	wsd.viewports[d * 2] = (uint32_t)wsd.monitors[monitor].rect.x1;
	wsd.viewports[d * 2 + 1] = (uint32_t)wsd.monitors[monitor].rect.y1;
	wsd.n_viewports = wsd.n_names * 2;
	ws_update_viewport_monitors();
	return d;
}

struct ws_client *fx_window(xcb_window_t id, int64_t desktop, struct ws_rect r) {
	if (wsd.n_clients == MAX_WINDOWS) {
		abort();
	}
	auto c = &wsd.clients[wsd.n_clients++];
	*c = (struct ws_client){
	    .id = id,
	    .frame = id,
	    .desktop = desktop,
	    .rect = r,
	    .workspace = desktop >= 0 ? strdup(wsd.names[desktop]) : NULL,
	};
	return c;
}

struct ws_client *fx_window_on(xcb_window_t id, unsigned desktop) {
	auto r = ws_monitor_rect(ws_desktop_monitor(desktop));
	int32_t x = (r.x1 + r.x2) / 2, y = (r.y1 + r.y2) / 2;
	return fx_window(
	    id, desktop,
	    (struct ws_rect){.x1 = x - 100, .y1 = y - 50, .x2 = x + 100, .y2 = y + 50});
}

struct ws_client *fx_client(xcb_window_t id) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		if (wsd.clients[i].id == id) {
			return &wsd.clients[i];
		}
	}
	abort();
}

static void event(enum ws_event_type type, xcb_window_t window) {
	if (n_events == sizeof(events) / sizeof(events[0])) {
		abort();
	}
	events[n_events++] = (struct ws_event){type, window, ++sequence};
}

void fx_map(xcb_window_t id) {
	auto c = fx_client(id);
	c->viewable = true;
	event(WS_EVENT_MAP, c->frame);
}

void fx_unmap(xcb_window_t id) {
	auto c = fx_client(id);
	c->viewable = false;
	event(WS_EVENT_UNMAP, c->frame);
}

void fx_current(unsigned desktop) {
	wsd.current = desktop;
	event(WS_EVENT_CURRENT, XCB_NONE);
}

void fx_read(void) {
	auto prev = read_clients;
	auto n_prev = n_read_clients;
	ws_reconcile(prev, n_prev, events, n_events, read_current);
	n_events = 0;
	read_current = wsd.current;
	ws_keep_gone(prev, n_prev);
	read_clients = copy_clients(wsd.clients, wsd.n_clients);
	n_read_clients = wsd.n_clients;
	wsd.read_frame = wsd.frame;
}

void fx_start(const unsigned *shown, unsigned n_shown, unsigned current) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		for (unsigned k = 0; k < n_shown; k++) {
			c->viewable |= c->desktop == shown[k];
		}
	}
	wsd.current = current;
	fx_read();
}

void fx_switch_events(unsigned desktop) {
	auto index = ws_desktop_monitor(desktop);
	fx_current(desktop);
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		if (c->desktop == desktop && !c->viewable) {
			fx_map(c->id);
		}
	}
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		if (c->desktop >= 0 && c->desktop != desktop && c->viewable &&
		    ws_desktop_monitor((unsigned)c->desktop) == index) {
			fx_unmap(c->id);
		}
	}
}

void fx_switch(unsigned desktop) {
	fx_switch_events(desktop);
	fx_read();
}

void fx_pass(void) {
	wsd.frame++;
	wsd.frame_time_us += 16667;
	now_us = wsd.frame_time_us;
}

struct ws_monitor *fx_tracked(unsigned desktop) {
	return ws_monitor_of(desktop);
}

uint64_t fx_last_switch(unsigned desktop) {
	auto m = fx_tracked(desktop);
	return m != NULL && m->n_switches > 0 ? m->switches[m->n_switches - 1].id : 0;
}

static struct fx_values compute(struct picom_animation_start start) {
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
	    .index = v[WS_VAR_INDEX],
	    .shown = v[WS_VAR_SHOWN],
	    .unfolded = v[WS_VAR_UNFOLDED],
	    .width_mm = v[WS_VAR_MONITOR_WIDTH_MM],
	    .height_mm = v[WS_VAR_MONITOR_HEIGHT_MM],
	    .backdrop = v[WS_VAR_BACKDROP],
	};
}

struct fx_values fx_compute(xcb_window_t id, const char *trigger) {
	return compute((struct picom_animation_start){
	    .trigger = trigger, .window = id, .client = id, .frame = wsd.frame});
}

struct fx_values fx_compute_frame(xcb_window_t frame, const char *trigger) {
	return compute((struct picom_animation_start){
	    .trigger = trigger, .window = frame, .frame = wsd.frame});
}
