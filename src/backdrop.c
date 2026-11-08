// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// Backdrops: one window of ours per workspace, the size of its monitor, painted with the
/// part of the wallpaper (_XROOTPMAP_ID, or _XSETROOT_ID) under that monitor, or with the
/// color in WS_DIRECTION_BACKDROP. We start ws-pass, ws-unfold and ws-fold on it with the
/// windows of its workspace, and it gets their variables, so a style can draw a face's
/// background, as the wallpaper on each side of a cube, and a click on an empty part of a
/// tile in the overview picks its workspace (window_at finds the backdrop).
///
/// There are backdrops only when WS_DIRECTION_BACKDROP asks for them. A backdrop is a
/// hidden window: it is stacked below every other window, and mapped only for a moment,
/// with the least _NET_WM_WINDOW_OPACITY there is, so picom gets an image of it, then
/// hidden again. picom draws it only in an animation, and only when the style's rule for
/// it keeps that image (keep-image-when-hidden): a style without a rule for the class
/// "ws-direction-backdrop" never sees it. We show it again when its monitor or the
/// wallpaper changes, and when picom lost the images of hidden windows (see warm.c).

#include <stdlib.h>
#include <string.h>

#include "ws_direction.h"

/// How long we wait for picom to have an image of a backdrop we showed, at most. picom
/// gets it in the next pass or two; when it doesn't composite, never.
#define WS_BACKDROP_WAIT_US (500 * 1000)

#define WS_BACKDROP_CLASS "ws-direction-backdrop\0ws-direction-backdrop"

/// One backdrop.
struct ws_backdrop {
	xcb_window_t window;
	char *workspace;
	/// Where it is: the rect of its workspace's monitor.
	struct ws_rect rect;
	/// Whether we must show it so picom gets an image, whether it is shown for that,
	/// and since when.
	bool stale, showing;
	uint64_t showing_us;
};

/// Read WS_DIRECTION_BACKDROP: "wallpaper" for the wallpaper, a color "#RRGGBB", or
/// "none" (or nothing) for no backdrops. Returns false if it is none of these.
bool ws_backdrops_init(void) {
	wsd.a_rootpmap = ws_atom("_XROOTPMAP_ID");
	wsd.a_setroot = ws_atom("_XSETROOT_ID");
	wsd.a_opacity = ws_atom("_NET_WM_WINDOW_OPACITY");
	wsd.backdrop_color = -1;
	wsd.backdrops_off = true;
	auto value = getenv("WS_DIRECTION_BACKDROP");
	if (value == NULL || *value == '\0' || strcmp(value, "none") == 0) {
		return true;
	}
	wsd.backdrops_off = false;
	if (strcmp(value, "wallpaper") == 0) {
		return true;
	}
	char *end = NULL;
	if (value[0] == '#' && strlen(value) == 7) {
		auto color = strtol(value + 1, &end, 16);
		if (end != NULL && *end == '\0') {
			wsd.backdrop_color = color;
			return true;
		}
	}
	wsd.backdrops_off = true;
	return false;
}

/// The backdrop that is window `w`, or NULL.
static struct ws_backdrop *ws_backdrop_of(xcb_window_t w) {
	for (unsigned i = 0; w != XCB_NONE && i < wsd.n_backdrops; i++) {
		if (wsd.backdrops[i].window == w) {
			return &wsd.backdrops[i];
		}
	}
	return NULL;
}

/// The workspace of backdrop window `w`, NULL if `w` is none of ours.
const char *ws_backdrop_workspace(xcb_window_t w) {
	auto b = ws_backdrop_of(w);
	return b != NULL ? b->workspace : NULL;
}

/// The wallpaper pixmap, as picom reads it, or XCB_NONE.
static xcb_pixmap_t ws_wallpaper(void) {
	xcb_atom_t atoms[] = {wsd.a_rootpmap, wsd.a_setroot};
	for (size_t i = 0; i < sizeof(atoms) / sizeof(atoms[0]); i++) {
		auto prop = ws_get_prop(wsd.root, atoms[i], XCB_ATOM_PIXMAP, 1);
		xcb_pixmap_t ret = prop != NULL && prop->format == 32
		                       ? *(xcb_pixmap_t *)xcb_get_property_value(prop)
		                       : XCB_NONE;
		free(prop);
		if (ret != XCB_NONE) {
			return ret;
		}
	}
	return XCB_NONE;
}

/// Paint backdrop `b` for where it is now: its part of the wallpaper, or the color.
static void ws_backdrop_paint(struct ws_backdrop *b) {
	auto r = b->rect;
	uint16_t width = (uint16_t)(r.x2 - r.x1), height = (uint16_t)(r.y2 - r.y1);
	xcb_configure_window(wsd.c, b->window,
	                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
	                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
	                     (uint32_t[]){(uint32_t)r.x1, (uint32_t)r.y1, width, height});
	if (wsd.backdrop_color >= 0) {
		xcb_change_window_attributes(wsd.c, b->window, XCB_CW_BACK_PIXEL,
		                             (uint32_t[]){(uint32_t)wsd.backdrop_color});
		return;
	}
	auto wallpaper = ws_wallpaper();
	if (wallpaper == XCB_NONE) {
		ws_log("Backdrop: no wallpaper (_XROOTPMAP_ID or _XSETROOT_ID), painting "
		       "black");
		xcb_change_window_attributes(wsd.c, b->window, XCB_CW_BACK_PIXEL,
		                             (uint32_t[]){0});
		return;
	}
	// Our part of it, in a pixmap of our own: the wallpaper's owner may free it.
	auto pixmap = xcb_generate_id(wsd.c);
	auto gc = xcb_generate_id(wsd.c);
	xcb_create_pixmap(wsd.c, XCB_COPY_FROM_PARENT, pixmap, wsd.root, width, height);
	xcb_create_gc(wsd.c, gc, pixmap, 0, NULL);
	xcb_copy_area(wsd.c, wallpaper, pixmap, gc, (int16_t)r.x1, (int16_t)r.y1, 0, 0,
	              width, height);
	xcb_change_window_attributes(wsd.c, b->window, XCB_CW_BACK_PIXMAP,
	                             (uint32_t[]){pixmap});
	xcb_free_gc(wsd.c, gc);
	// The window keeps it.
	xcb_free_pixmap(wsd.c, pixmap);
}

static void ws_backdrop_create(struct ws_backdrop *b) {
	b->window = xcb_generate_id(wsd.c);
	auto r = b->rect;
	xcb_create_window(wsd.c, XCB_COPY_FROM_PARENT, b->window, wsd.root, (int16_t)r.x1,
	                  (int16_t)r.y1, (uint16_t)(r.x2 - r.x1), (uint16_t)(r.y2 - r.y1),
	                  0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
	                  XCB_CW_OVERRIDE_REDIRECT, (uint32_t[]){1});
	xcb_change_property(wsd.c, XCB_PROP_MODE_REPLACE, b->window, XCB_ATOM_WM_CLASS,
	                    XCB_ATOM_STRING, 8, sizeof(WS_BACKDROP_CLASS), WS_BACKDROP_CLASS);
	char name[128];
	auto len = snprintf(name, sizeof(name), "ws-direction backdrop %s", b->workspace);
	xcb_change_property(
	    wsd.c, XCB_PROP_MODE_REPLACE, b->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
	    (uint32_t)(len < (int)sizeof(name) ? len : (int)sizeof(name) - 1), name);
	xcb_configure_window(wsd.c, b->window, XCB_CONFIG_WINDOW_STACK_MODE,
	                     (uint32_t[]){XCB_STACK_MODE_BELOW});
	ws_backdrop_paint(b);
	b->stale = true;
}

/// After a read: a backdrop for every workspace, where its monitor is. Those of
/// workspaces that are gone go; a moved one is painted again and shown again.
void ws_backdrops_update(void) {
	if (wsd.backdrops_off) {
		return;
	}
	for (unsigned i = 0; i < wsd.n_backdrops;) {
		auto b = &wsd.backdrops[i];
		if (ws_find_desktop(b->workspace) < 0) {
			xcb_destroy_window(wsd.c, b->window);
			free(b->workspace);
			wsd.backdrops[i] = wsd.backdrops[--wsd.n_backdrops];
		} else {
			i++;
		}
	}
	for (unsigned d = 0; d < wsd.n_names; d++) {
		auto r = ws_monitor_rect(ws_desktop_monitor(d));
		struct ws_backdrop *b = NULL;
		for (unsigned i = 0; i < wsd.n_backdrops && b == NULL; i++) {
			b = strcmp(wsd.backdrops[i].workspace, wsd.names[d]) == 0
			        ? &wsd.backdrops[i]
			        : NULL;
		}
		if (b == NULL) {
			wsd.backdrops = ws_grow(wsd.backdrops, wsd.n_backdrops + 1,
			                        sizeof(*wsd.backdrops));
			b = &wsd.backdrops[wsd.n_backdrops++];
			*b = (struct ws_backdrop){.rect = r,
			                          .workspace = strdup(wsd.names[d])};
			if (b->workspace == NULL) {
				abort();
			}
			ws_backdrop_create(b);
		} else if (memcmp(&b->rect, &r, sizeof(r)) != 0 || wsd.wallpaper_changed) {
			b->rect = r;
			ws_backdrop_paint(b);
			b->stale = true;
		}
	}
	wsd.wallpaper_changed = false;
	xcb_flush(wsd.c);
}

/// Show every backdrop again, so picom gets its image back.
void ws_backdrops_redo(void) {
	for (unsigned i = 0; i < wsd.n_backdrops; i++) {
		wsd.backdrops[i].stale = true;
	}
}

/// Whether a backdrop is shown for picom to get its image: picom must then run passes,
/// see ws_pending in plugin.c.
bool ws_backdrops_showing(void) {
	for (unsigned i = 0; i < wsd.n_backdrops; i++) {
		if (wsd.backdrops[i].showing || wsd.backdrops[i].stale) {
			return true;
		}
	}
	return false;
}

/// In every pass: show the backdrops picom needs an image of, with the least opacity, and
/// hide them again once picom has one, which it keeps then if the style's rule says so.
void ws_backdrops_advance(void) {
	bool any = false;
	for (unsigned i = 0; i < wsd.n_backdrops; i++) {
		auto b = &wsd.backdrops[i];
		if (b->showing) {
			bool late = wsd.frame_time_us - b->showing_us >= WS_BACKDROP_WAIT_US;
			bool drawn = wsd.api->window_has_image(b->window);
			if (!drawn && !late) {
				continue;
			}
			if (!drawn) {
				ws_log("Backdrop of \"%s\": picom has no image of it",
				       b->workspace);
			}
			xcb_unmap_window(wsd.c, b->window);
			xcb_delete_property(wsd.c, b->window, wsd.a_opacity);
			b->showing = false;
			any = true;
		} else if (b->stale) {
			// The least opacity there is: picom draws nothing a screen can
			// show, but it doesn't drop the image as it does at opacity 0.
			uint32_t transparent = 1;
			xcb_change_property(wsd.c, XCB_PROP_MODE_REPLACE, b->window,
			                    wsd.a_opacity, XCB_ATOM_CARDINAL, 32, 1,
			                    &transparent);
			xcb_map_window(wsd.c, b->window);
			b->stale = false;
			b->showing = true;
			b->showing_us = wsd.frame_time_us;
			any = true;
		}
	}
	if (any) {
		xcb_flush(wsd.c);
	}
}

/// Start `trigger` on the backdrops of the workspaces of monitor `m`, for its switches
/// `first` to `last`. Returns on how many picom started it.
unsigned ws_backdrops_start(const struct ws_monitor *m, const char *trigger,
                            uint64_t first, uint64_t last) {
	auto index = ws_monitor_of_name(m->name);
	unsigned started = 0;
	for (unsigned i = 0; i < wsd.n_backdrops; i++) {
		auto b = &wsd.backdrops[i];
		auto d = ws_find_desktop(b->workspace);
		// Without an image picom ignores the start: the style doesn't keep it.
		if (d >= 0 && ws_desktop_monitor((unsigned)d) == index && !b->showing &&
		    wsd.api->window_has_image(b->window)) {
			started += ws_start(b->window, trigger, m, first, last);
		}
	}
	return started;
}
