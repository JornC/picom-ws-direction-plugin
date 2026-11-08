// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// ws-direction: commands for a window manager config to call.
///
///   ws-direction overview open [--all | --output NAME] [--wait [--buttons]]
///       Opens the overview on the monitor with the focus, on every monitor, or on the
///       monitor with that RandR name. The plugin in picom does the rest; the style draws
///       it. With --wait, open returns 0 once a click on a window closed the overview,
///       and 1 when a close or toggle command comes, whatever it did. With --buttons
///       also, it returns 10 plus the number of another button pressed over an unfolded
///       monitor: 12 for the middle one, 13 for the right one, 14 and 15 for the wheel
///       up and down, 16 and 17 for left and right. A window manager config binds what
///       they do.
///
///   ws-direction overview close | toggle
///       Closes the overview on every monitor, or does whichever changes the monitor
///       with the focus.
///
///   ws-direction --version
///       Prints the version.
///
/// It talks to the plugin as protocol.h says, and fails with a message when picom does
/// not run with the plugin, or the plugin speaks another version.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

#include "protocol.h"
#include "version.h"

static int usage(void) {
	fputs("usage: ws-direction overview open [--all | --output NAME] [--wait "
	      "[--buttons]]\n"
	      "       ws-direction overview close | toggle\n"
	      "       ws-direction --version\n",
	      stderr);
	return 2;
}

struct command {
	uint32_t command, target, wait;
	const char *output;
};

/// Read the arguments after "overview". Returns false if they are wrong.
static bool parse(int argc, char **argv, struct command *cmd) {
	*cmd = (struct command){0};
	if (argc < 1) {
		return false;
	}
	if (strcmp(argv[0], "close") == 0 || strcmp(argv[0], "toggle") == 0) {
		cmd->command = argv[0][0] == 'c' ? WS_COMMAND_CLOSE : WS_COMMAND_TOGGLE;
		return argc == 1;
	}
	if (strcmp(argv[0], "open") != 0) {
		return false;
	}
	cmd->command = WS_COMMAND_OPEN;
	bool buttons = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0 && cmd->target == WS_TARGET_FOCUS) {
			cmd->target = WS_TARGET_ALL;
		} else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc &&
		           cmd->target == WS_TARGET_FOCUS) {
			cmd->target = WS_TARGET_OUTPUT;
			cmd->output = argv[++i];
		} else if (strcmp(argv[i], "--wait") == 0) {
			cmd->wait |= WS_WAIT;
		} else if (strcmp(argv[i], "--buttons") == 0) {
			buttons = true;
		} else {
			return false;
		}
	}
	if (buttons && !(cmd->wait & WS_WAIT)) {
		return false;
	}
	cmd->wait |= buttons ? WS_WAIT_BUTTONS : 0;
	return true;
}

static xcb_atom_t atom(xcb_connection_t *c, const char *name) {
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
	    c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t ret = reply != NULL ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return ret;
}

static int fail(xcb_connection_t *c, const char *message) {
	fprintf(stderr, "ws-direction: %s\n", message);
	xcb_disconnect(c);
	return 1;
}

static const char *failure(uint32_t reason) {
	switch (reason) {
	case WS_FAILED_NO_CURRENT: return "the window manager sets no current desktop";
	case WS_FAILED_NO_OUTPUT: return "there is no monitor with that name";
	case WS_FAILED_INPUT:
		return "the plugin could not take the clicks on the monitor";
	case WS_FAILED_UNFOLDED: return "a monitor shows the overview: close it first";
	case WS_FAILED_GRAB: return "another program holds the keyboard or the pointer";
	default: return "the plugin could not do it";
	}
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		puts("ws-direction " WS_DIRECTION_VERSION);
		return 0;
	}
	struct command cmd = {0};
	if (argc < 3 || strcmp(argv[1], "overview") != 0 || !parse(argc - 2, argv + 2, &cmd)) {
		return usage();
	}

	int screen_num = 0;
	xcb_connection_t *c = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(c)) {
		return fail(c, "cannot connect to the X server");
	}
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
	for (int i = screen_num; i > 0 && it.rem; i--) {
		xcb_screen_next(&it);
	}
	if (!it.rem) {
		return fail(c, "cannot find the root window");
	}
	xcb_window_t root = it.data->root;
	char selection_name[32];
	snprintf(selection_name, sizeof(selection_name), WS_SELECTION_PREFIX "%d", screen_num);
	xcb_atom_t selection = atom(c, selection_name),
	           version = atom(c, WS_VERSION_PROPERTY),
	           command = atom(c, WS_COMMAND_MESSAGE),
	           answer = atom(c, WS_ANSWER_MESSAGE);
	xcb_atom_t output = cmd.output != NULL ? atom(c, cmd.output) : XCB_ATOM_NONE;
	if (selection == XCB_ATOM_NONE || version == XCB_ATOM_NONE || command == XCB_ATOM_NONE ||
	    answer == XCB_ATOM_NONE || (cmd.output != NULL && output == XCB_ATOM_NONE)) {
		return fail(c, "cannot intern the atoms");
	}

	xcb_get_selection_owner_reply_t *owner_reply =
	    xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, selection), NULL);
	xcb_window_t owner = owner_reply != NULL ? owner_reply->owner : XCB_NONE;
	free(owner_reply);
	if (owner == XCB_NONE) {
		return fail(c, "picom is not running with the ws-direction plugin");
	}
	// Before we read the version, so we see the plugin go away after it.
	xcb_change_window_attributes(c, owner, XCB_CW_EVENT_MASK,
	                             (uint32_t[]){XCB_EVENT_MASK_STRUCTURE_NOTIFY});
	xcb_get_property_reply_t *prop = xcb_get_property_reply(
	    c, xcb_get_property(c, 0, owner, version, XCB_ATOM_CARDINAL, 0, 1), NULL);
	int64_t theirs = -1;
	if (prop != NULL && prop->format == 32 && prop->value_len == 1) {
		theirs = *(uint32_t *)xcb_get_property_value(prop);
	}
	free(prop);
	if (theirs != WS_PROTOCOL_VERSION) {
		char message[128];
		snprintf(message, sizeof(message),
		         "the plugin speaks protocol version %lld, this command %d: "
		         "install "
		         "them from the same version",
		         (long long)theirs, WS_PROTOCOL_VERSION);
		return fail(c, message);
	}

	// A window of our own for the answers.
	xcb_window_t reply = xcb_generate_id(c);
	xcb_create_window(c, 0, reply, root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
	                  XCB_COPY_FROM_PARENT, 0, NULL);
	xcb_client_message_event_t event = {
	    .response_type = XCB_CLIENT_MESSAGE,
	    .format = 32,
	    .window = owner,
	    .type = command,
	    .data.data32 = {cmd.command, cmd.target, output, reply, cmd.wait},
	};
	xcb_send_event(c, 0, owner, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
	xcb_flush(c);

	xcb_generic_event_t *e;
	while ((e = xcb_wait_for_event(c)) != NULL) {
		auto type = e->response_type & 0x7f;
		if (type == XCB_DESTROY_NOTIFY &&
		    ((xcb_destroy_notify_event_t *)e)->window == owner) {
			free(e);
			return fail(c, "picom stopped before the overview closed");
		}
		if (type != XCB_CLIENT_MESSAGE ||
		    ((xcb_client_message_event_t *)e)->type != answer) {
			free(e);
			continue;
		}
		auto data = ((xcb_client_message_event_t *)e)->data.data32;
		uint32_t code = data[0], detail = data[1];
		free(e);
		switch (code) {
		case WS_ANSWER_DONE:
			if (!(cmd.wait & WS_WAIT)) {
				xcb_disconnect(c);
				return 0;
			}
			break;
		case WS_ANSWER_FAILED: return fail(c, failure(detail));
		case WS_ANSWER_PICKED: xcb_disconnect(c); return 0;
		case WS_ANSWER_CLOSED: xcb_disconnect(c); return 1;
		case WS_ANSWER_BUTTON: xcb_disconnect(c); return 10 + (int)detail;
		default: break;
		}
	}
	return fail(c, "lost the connection to the X server");
}
