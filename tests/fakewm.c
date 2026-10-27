// SPDX-License-Identifier: MPL-2.0
// A tiny stand-in for a window manager, used by run.sh to test picom's
// workspace-direction tracking without i3. It runs a list of steps on one X
// connection, in order, and flushes after each one, like a real window manager
// would. "sleep" lets the steps land in different picom frames.
//
// Steps:
//   names 1:A,2:B,...     set _NET_DESKTOP_NAMES (comma separated)
//   viewport x,y,x,y,...  set _NET_DESKTOP_VIEWPORT
//   noviewport            delete _NET_DESKTOP_VIEWPORT
//   current N             set _NET_CURRENT_DESKTOP
//   desktop WIN N|sticky  set _NET_WM_DESKTOP on a window
//   map WIN / unmap WIN   map or unmap a window
//   sleep MS              wait
//
// Build: cc -o fakewm fakewm.c $(pkg-config --cflags --libs xcb)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_window_t root;

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r =
	    xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	if (r == NULL) {
		fprintf(stderr, "fakewm: cannot intern %s\n", name);
		exit(1);
	}
	xcb_atom_t a = r->atom;
	free(r);
	return a;
}

static void set_cardinals(xcb_window_t w, const char *prop, const char *list) {
	uint32_t values[256];
	uint32_t n = 0;
	char *copy = strdup(list);
	for (char *tok = strtok(copy, ","); tok != NULL && n < 256; tok = strtok(NULL, ",")) {
		values[n++] = (uint32_t)strtoul(tok, NULL, 0);
	}
	free(copy);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom(prop), XCB_ATOM_CARDINAL, 32,
	                    n, values);
}

static void set_names(const char *list) {
	// Each name ends with a NUL, like i3 does it.
	size_t len = strlen(list) + 1;
	char *buf = strdup(list);
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == ',') {
			buf[i] = '\0';
		}
	}
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, atom("_NET_DESKTOP_NAMES"),
	                    atom("UTF8_STRING"), 8, (uint32_t)len, buf);
	free(buf);
}

static void sleep_ms(long ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
	nanosleep(&ts, NULL);
}

static const char *next(int argc, char **argv, int *i) {
	if (*i + 1 >= argc) {
		fprintf(stderr, "fakewm: %s needs an argument\n", argv[*i]);
		exit(1);
	}
	return argv[++*i];
}

int main(int argc, char **argv) {
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		fprintf(stderr, "fakewm: cannot connect to X\n");
		return 1;
	}
	root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;

	for (int i = 1; i < argc; i++) {
		const char *step = argv[i];
		if (strcmp(step, "names") == 0) {
			set_names(next(argc, argv, &i));
		} else if (strcmp(step, "viewport") == 0) {
			set_cardinals(root, "_NET_DESKTOP_VIEWPORT", next(argc, argv, &i));
		} else if (strcmp(step, "noviewport") == 0) {
			xcb_delete_property(c, root, atom("_NET_DESKTOP_VIEWPORT"));
		} else if (strcmp(step, "current") == 0) {
			set_cardinals(root, "_NET_CURRENT_DESKTOP", next(argc, argv, &i));
		} else if (strcmp(step, "desktop") == 0) {
			xcb_window_t w = (xcb_window_t)strtoul(next(argc, argv, &i), NULL, 0);
			const char *d = next(argc, argv, &i);
			set_cardinals(w, "_NET_WM_DESKTOP",
			              strcmp(d, "sticky") == 0 ? "0xFFFFFFFF" : d);
		} else if (strcmp(step, "map") == 0) {
			xcb_map_window(c, (xcb_window_t)strtoul(next(argc, argv, &i), NULL, 0));
		} else if (strcmp(step, "unmap") == 0) {
			xcb_unmap_window(c, (xcb_window_t)strtoul(next(argc, argv, &i), NULL, 0));
		} else if (strcmp(step, "sleep") == 0) {
			xcb_flush(c);
			sleep_ms(strtol(next(argc, argv, &i), NULL, 10));
		} else {
			fprintf(stderr, "fakewm: unknown step %s\n", step);
			return 1;
		}
		xcb_flush(c);
	}
	// Make sure the server has handled everything before we exit.
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	xcb_disconnect(c);
	return 0;
}
