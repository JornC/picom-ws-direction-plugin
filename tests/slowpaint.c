// SPDX-License-Identifier: MPL-2.0
// A window that draws itself only a while after it shows, used by run.sh to test that
// the plugin waits for a window to draw all of itself, not for the X server's painting.
// The X server paints the window red when it maps it (its background); the program
// paints all of it blue DELAY milliseconds after each Expose.
//
// Usage: slowpaint INSTANCE TITLE DELAY
// Build: cc -o slowpaint slowpaint.c $(pkg-config --cflags --libs xcb)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

int main(int argc, char **argv) {
	if (argc != 4) {
		fputs("usage: slowpaint INSTANCE TITLE DELAY\n", stderr);
		return 2;
	}
	long delay_ms = strtol(argv[3], NULL, 10);
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		fputs("slowpaint: cannot connect to the X server\n", stderr);
		return 1;
	}
	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_window_t w = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 400, 300, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
	                  (uint32_t[]){0xff0000, XCB_EVENT_MASK_EXPOSURE |
	                                             XCB_EVENT_MASK_STRUCTURE_NOTIFY});
	// WM_CLASS is the instance and the class, each with its NUL.
	char class[256];
	int n = snprintf(class, sizeof(class), "%s%cslowpaint", argv[1], 0);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS,
	                    XCB_ATOM_STRING, 8, (uint32_t)n + 1, class);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
	                    XCB_ATOM_STRING, 8, (uint32_t)strlen(argv[2]), argv[2]);
	xcb_gcontext_t gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, (uint32_t[]){0x0000ff});
	xcb_map_window(c, w);
	xcb_flush(c);

	uint16_t width = 400, height = 300;
	xcb_generic_event_t *e;
	while ((e = xcb_wait_for_event(c)) != NULL) {
		switch (e->response_type & 0x7f) {
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)e;
			width = ce->width;
			height = ce->height;
			break;
		}
		case XCB_EXPOSE:
			if (((xcb_expose_event_t *)e)->count == 0) {
				nanosleep(&(struct timespec){.tv_sec = delay_ms / 1000,
				                             .tv_nsec = delay_ms % 1000 * 1000000},
				          NULL);
				xcb_poly_fill_rectangle(
				    c, w, gc, 1, &(xcb_rectangle_t){0, 0, width, height});
				xcb_flush(c);
			}
			break;
		}
		free(e);
	}
	return 0;
}
