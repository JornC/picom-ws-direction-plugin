// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// The plugin's internal header: the types, the state and the functions that more than
/// one of its modules uses, or the unit tests in tests/unit. desktop.c reads the
/// monitors, the workspaces and the managed windows from the X server. passes.c finds
/// switches, starts ws-pass and works out the variables. overview.c unfolds and folds the
/// overview, and does the commands. backdrop.c keeps a backdrop window per workspace.
/// plugin.c hooks them up to picom and calls them in every pass.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <picom/api.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

/// The plugin API the plugin needs is 0.WS_API_MINOR, as the picom fork numbers it.
#define WS_API_MINOR 9
#define WS_STR(x) #x
#define WS_XSTR(x) WS_STR(x)
#define WS_API_VERSION "0." WS_XSTR(WS_API_MINOR)

#if PICOM_API_MAJOR != 0 || PICOM_API_MINOR < WS_API_MINOR
// Not #error, which can't expand WS_API_VERSION.
static_assert(false, "picom/api.h is not compatible: ws-direction needs plugin "
                     "API " WS_API_VERSION " of the picom fork. To build against a "
                     "picom source tree, run make "
                     "PICOM_API_CFLAGS=-I/path/to/picom/include");
#endif

/// The custom trigger we start on the windows of the workspaces a switch passes.
#define WS_PASS_TRIGGER "ws-pass"
/// The custom triggers we start on the windows of a monitor when its workspaces unfold
/// into an overview, and when they fold back.
#define WS_UNFOLD_TRIGGER "ws-unfold"
#define WS_FOLD_TRIGGER "ws-fold"

/// The animation variables, plugin-ws-direction-<name>, with names in ws_variable_names.
enum ws_variable {
	WS_VAR_DIRECTION,
	WS_VAR_DISTANCE,
	WS_VAR_MONITOR_X,
	WS_VAR_MONITOR_Y,
	WS_VAR_MONITOR_WIDTH,
	WS_VAR_MONITOR_HEIGHT,
	WS_VAR_FACES,
	WS_VAR_FACE,
	WS_VAR_SELECTED,
	WS_VAR_PLACED,
	WS_VAR_INDEX,
	WS_VAR_SHOWN,
	WS_VAR_UNFOLDED,
	WS_VAR_BACKDROP,
	WS_N_VARIABLES,
};

/// How many switches we remember per monitor. When switching fast (e.g. scrolling on a
/// bar), the windows of a switch can still be showing and hiding while the next switches
/// are already done.
#define WS_HISTORY 8
#define WS_MAX_MONITORS 16
#define WS_MAX_DESKTOPS 256

struct ws_rect {
	int32_t x1, y1, x2, y2;
};

/// A monitor, as RandR gives it.
struct ws_output {
	xcb_atom_t name;
	struct ws_rect rect;
};

/// A workspace switch as a window sees it: which way it went, and how many workspaces of
/// the monitor it moved. Both are 0 if the window's animation is not part of a switch.
struct ws_move {
	int direction;
	unsigned distance;
};

/// A workspace switch on a monitor.
struct ws_switch {
	/// Counts the switches, from 1. 0 stands for no switch.
	uint64_t id;
	char *from, *to;
	/// The monitor's workspaces in order, as of the switch: those of the read in
	/// which we saw it, and those of the read before that are gone since, after the
	/// one they came after. So a workspace that the window manager removed with the
	/// switch still counts.
	char **list;
	unsigned n_list;
	struct ws_move move;
	/// Whether we started ws-pass for it, or for a later one that includes it.
	bool passes_started;
	/// Whether a show or hide was part of it. A switch that comes before the one
	/// before had any goes on from it: `start` is the first switch of such a chain,
	/// which the window manager makes as one move.
	bool realized;
	uint64_t start;
};

/// What we know about one monitor.
struct ws_monitor {
	/// Its RandR name, XCB_NONE for the whole screen.
	xcb_atom_t name;
	/// The workspace it shows, NULL until we know.
	char *shown;
	/// Its workspaces in order, as of the last read.
	char **list;
	unsigned n_list;
	/// Its switches, oldest first.
	struct ws_switch switches[WS_HISTORY];
	unsigned n_switches;
	/// Whether its workspaces are unfolded into an overview.
	bool unfolded;
	/// The workspace of the window a click in the overview picked, while we wait for
	/// the window manager to show it, else NULL.
	char *pick;
	/// While it is unfolded, the window over it that takes its clicks, and where it
	/// is.
	xcb_window_t input;
	struct ws_rect input_rect;
	/// While it is unfolded, its workspaces and windows when we last started
	/// ws-unfold on them, see ws_overview_refresh.
	char *unfolded_as;
	/// Its windows that picom had no image of when we started ws-unfold, see
	/// ws_overview_retry.
	xcb_window_t *waiting;
	unsigned n_waiting;
};

/// A window the window manager manages, as the last read saw it.
struct ws_client {
	xcb_window_t id;
	/// Its top-level window: the frame the window manager put it in, or itself.
	xcb_window_t frame;
	/// Its desktop, -1 if it is not on one workspace.
	int64_t desktop;
	/// Where it is, in root coordinates.
	struct ws_rect rect;
	/// The name of its workspace, NULL if it is not on one.
	char *workspace;
	/// Whether it is on all desktops, and not hidden.
	bool sticky;
	/// Whether it is viewable: it and its frame are mapped.
	bool viewable;
	/// The switch that its next show or hide is part of, and the switch that its last
	/// show and its last hide were part of. 0 for none.
	uint64_t pending_show, pending_hide, last_show, last_hide;
	/// The last switch to its workspace that it was viewable at: it moved along.
	uint64_t along;
};

/// Why the plugin started an animation on a window in this pass: the trigger, the
/// monitor, and the switches it is for, from `first` to `last` (0 for none).
struct ws_start {
	xcb_window_t window;
	const char *trigger;
	xcb_atom_t monitor;
	uint64_t first, last;
};

/// A command, as the client message's data (protocol.h).
struct ws_command_message {
	uint32_t data[5];
};

/// A command that waits for the overview to close: its window, and its WS_WAIT_* flags.
struct ws_waiter {
	xcb_window_t window;
	uint32_t flags;
};

/// A map or unmap of a top-level window, or a change of _NET_CURRENT_DESKTOP, that our
/// connection got. `sequence` orders it against our requests: the X server made it after
/// our request of that number, and before the next one.
enum ws_event_type { WS_EVENT_MAP, WS_EVENT_UNMAP, WS_EVENT_CURRENT };
struct ws_event {
	enum ws_event_type type;
	xcb_window_t window;
	uint32_t sequence;
};

/// The requests of the first round trip of a read: the root window properties, the
/// screen size, the monitors, the top-level windows and the window list.
struct ws_root_requests {
	xcb_get_property_cookie_t viewports, names, list;
	xcb_get_geometry_cookie_t screen;
	xcb_randr_get_monitors_cookie_t monitors;
	xcb_query_tree_cookie_t tree;
};

/// Where a read of the root window and the managed windows is: it waits for the replies
/// to the first round trip, then for those of the second, which asks about each window in
/// the list, and for the desktop names and the current desktop again.
enum ws_read_step { WS_READ_IDLE, WS_READ_ROOT, WS_READ_WINDOWS };

// Defined in desktop.c and backdrop.c, the only modules that look inside them.
struct ws_window_requests;
struct ws_backdrop;

/// Everything the plugin knows, defined in plugin.c.
struct ws_state {
	const struct picom_api *api;
	xcb_connection_t *c;
	xcb_window_t root;
	xcb_atom_t a_current_desktop, a_desktop_names, a_desktop_viewport, a_wm_desktop,
	    a_utf8_string, a_net_wm_name, a_client_list, a_wm_state, a_wm_state_hidden,
	    a_command, a_answer, a_active_window;
	struct ws_rect screen;
	FILE *log;

	/// The pass picom is in (UINT64_MAX before the first one), and when it started.
	uint64_t frame;
	uint64_t frame_time_us;
	/// The pass in which we last read the root window and the managed windows, and
	/// the one whose read the overview last looked at.
	uint64_t read_frame, refreshed_read;
	/// A read of the root window and the managed windows that waits for its replies,
	/// see ws_read_start.
	struct {
		enum ws_read_step step;
		struct ws_root_requests root;
		/// The replies of the first round trip that the second one needs.
		xcb_get_property_reply_t *names;
		xcb_window_t *toplevels;
		unsigned n_toplevels;
		struct ws_window_requests *windows;
		unsigned n_windows;
		xcb_get_property_cookie_t current, names_again;
		/// The last request of the step: once its reply is in, all of them are.
		xcb_get_input_focus_cookie_t sync;
		/// When it started, and in which pass.
		uint64_t start_us, start_frame;
		/// Whether the screen is paused while it waits.
		bool paused;
		/// Whether the root window or a top-level window changed since the last
		/// read.
		bool needed;
	} read;
	/// How often we waited for the X server while working out the variables of one
	/// window. For the log.
	unsigned n_waits;

	/// _NET_DESKTOP_NAMES, pointers into `names_buf`.
	const char *names[WS_MAX_DESKTOPS];
	unsigned n_names;
	char *names_buf;
	/// _NET_DESKTOP_VIEWPORT, as x, y pairs.
	uint32_t viewports[WS_MAX_DESKTOPS * 2];
	unsigned n_viewports;
	/// Whether the viewports say which monitor each desktop is on: they differ, and
	/// each one is the corner of a monitor. Else every desktop spans the whole
	/// screen.
	bool viewport_monitors;
	/// _NET_CURRENT_DESKTOP, -1 if not set.
	int64_t current;

	struct ws_output monitors[WS_MAX_MONITORS];
	int n_monitors;

	/// The windows in _NET_CLIENT_LIST. Windows whose place could not be read are
	/// left out.
	struct ws_client *clients;
	unsigned n_clients;
	/// The top-level windows, bottom to top, as the last read saw them.
	xcb_window_t *stack;
	unsigned n_stack;

	struct ws_monitor tracked[WS_MAX_MONITORS];
	unsigned n_tracked;
	/// The id of the last switch we recorded.
	uint64_t last_switch;

	/// The maps, unmaps and current desktop changes our connection got that the next
	/// read takes in.
	struct ws_event *events;
	unsigned n_events;
	/// The backdrops, one per workspace (backdrop.c), the atoms they need, the color
	/// they have (-1 for the wallpaper), whether there are none, and whether the
	/// wallpaper changed since we last painted them.
	struct ws_backdrop *backdrops;
	unsigned n_backdrops;
	xcb_atom_t a_rootpmap, a_setroot, a_opacity;
	int64_t backdrop_color;
	bool backdrops_off, wallpaper_changed;
	/// The animations we started in this pass, and why.
	struct ws_start *starts;
	unsigned n_starts;
	uint64_t starts_frame;

	/// The window that owns our selection, which the command sends its commands to.
	xcb_window_t owner;
	/// The commands that came and that we didn't do yet, and the commands that wait
	/// for the overview to close.
	struct ws_command_message *commands;
	unsigned n_commands;
	struct ws_waiter *waiters;
	unsigned n_waiters;
	/// The last click of the first button while a monitor was unfolded: where it was
	/// pressed, whether it was pressed and released since we last acted on one, and
	/// when it was released.
	struct {
		bool pressed, pending;
		int16_t x, y;
		xcb_timestamp_t time;
	} click;
	/// An event that ws_pending took from xcb's queue, not handled yet.
	xcb_generic_event_t *queued;
};

extern struct ws_state wsd;

/// `array` grown to `n` items of `size` bytes. Aborts when out of memory.
static inline void *ws_grow(void *array, size_t n, size_t size) {
	void *grown = realloc(array, n * size);
	if (grown == NULL) {
		abort();
	}
	return grown;
}

// plugin.c
__attribute__((format(printf, 1, 2))) void ws_log(const char *fmt, ...);
uint64_t ws_now_us(void);
void ws_take_events(void);

// desktop.c
xcb_atom_t ws_atom(const char *name);
void *ws_reply(unsigned int sequence);
xcb_get_property_reply_t *
ws_get_prop(xcb_window_t w, xcb_atom_t atom, xcb_atom_t type, uint32_t max_len);
int64_t ws_find_desktop(const char *name);
bool ws_rect_has_point(struct ws_rect r, int32_t x, int32_t y);
void ws_update_viewport_monitors(void);
int ws_desktop_monitor(unsigned desktop);
int ws_monitor_of_name(xcb_atom_t name);
struct ws_rect ws_monitor_rect(int index);
const struct ws_client *ws_find_client(xcb_window_t id);
const struct ws_client *ws_find_client_by_frame(xcb_window_t frame);
void ws_read_start(void);
bool ws_read_advance(uint64_t wait_us);
void ws_read(void);

// passes.c
int ws_rect_monitor(struct ws_rect r);
struct ws_monitor *ws_find_monitor(int index);
struct ws_monitor *ws_monitor_of(unsigned desktop);
struct ws_monitor *ws_get_monitor(int index);
void ws_free_monitor(struct ws_monitor *m);
const struct ws_switch *ws_find_switch(const struct ws_monitor *m, uint64_t id);
unsigned ws_monitor_list(int index, const char *list[static WS_MAX_DESKTOPS]);
int ws_list_find(const char *const *list, unsigned n, const char *name);
struct ws_move
ws_steps(const char *from, const char *to, const char *const *list, unsigned n);
void ws_reconcile(struct ws_client *prev, unsigned n_prev, struct ws_event *events,
                  unsigned n_events, int64_t prev_current);
bool ws_start(xcb_window_t w, const char *trigger, const struct ws_monitor *m,
              uint64_t first, uint64_t last);
void ws_compute(const struct picom_animation_start *start,
                double values[static WS_N_VARIABLES]);
bool ws_start_passes(void);

// backdrop.c
bool ws_backdrops_init(void);
const char *ws_backdrop_workspace(xcb_window_t w);
void ws_backdrops_update(void);
void ws_backdrops_advance(void);
bool ws_backdrops_showing(void);
unsigned ws_backdrops_start(const struct ws_monitor *m, const char *trigger,
                            uint64_t first, uint64_t last);

// overview.c

bool ws_any_unfolded(void);
void ws_answer(xcb_window_t w, uint32_t code, uint32_t detail);
void ws_pick_done(void);
void ws_pick(void);
const char *
ws_overview_switch_trigger(struct ws_monitor *m, const struct ws_switch *s, bool *folded);
void ws_run_commands(void);
void ws_place_inputs(void);
bool ws_overview_member(const struct ws_client *c, int index, bool overview);
void ws_overview_started(struct ws_monitor *m);
void ws_overview_refresh(void);
void ws_overview_retry(void);
void ws_button(uint8_t button);
bool ws_overview_init(int screen);
