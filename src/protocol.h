// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// How the ws-direction command talks to the plugin. While picom runs with the plugin,
/// the plugin owns the selection _WS_DIRECTION_S<screen number> with a window of its own,
/// whose property _WS_DIRECTION_VERSION holds WS_PROTOCOL_VERSION. The command sends that
/// window one client message per command, of type _WS_DIRECTION_COMMAND, and the plugin
/// answers with client messages of type _WS_DIRECTION_ANSWER to the window the command
/// names. Both may change with the version: the command is the interface.
///
/// The command also sets the root window property _WS_DIRECTION_WARM, which says whether
/// the plugin gets the images of hidden windows back by itself: a STRING, "on" (the
/// default, also when the property is not there) or "off". A property, so it holds while
/// picom doesn't run, and across picom restarts.

#pragma once

#define WS_SELECTION_PREFIX "_WS_DIRECTION_S"
#define WS_VERSION_PROPERTY "_WS_DIRECTION_VERSION"
#define WS_PROTOCOL_VERSION 1
#define WS_COMMAND_MESSAGE "_WS_DIRECTION_COMMAND"
#define WS_ANSWER_MESSAGE "_WS_DIRECTION_ANSWER"
#define WS_WARM_PROPERTY "_WS_DIRECTION_WARM"

/// A command, format 32: data32[0] is one of these, data32[1] a target, data32[2] the
/// atom of the RandR name of a monitor for WS_TARGET_OUTPUT, data32[3] the window to
/// answer (0 for none), data32[4] WS_WAIT_* flags.
enum ws_command {
	WS_COMMAND_OPEN = 1,
	WS_COMMAND_CLOSE = 2,
	WS_COMMAND_TOGGLE = 3,
	/// Get the images of hidden windows back now. Answered once that is done.
	WS_COMMAND_WARM = 4,
};

/// Which monitors open: the one with the focus, every one, or one by name.
enum ws_target {
	WS_TARGET_FOCUS = 0,
	WS_TARGET_ALL = 1,
	WS_TARGET_OUTPUT = 2,
};

/// The window to answer waits until the overview closes: a click picks a window, or a
/// close or toggle command comes. With WS_WAIT_BUTTONS also until another button is
/// pressed over an unfolded monitor.
#define WS_WAIT 1
#define WS_WAIT_BUTTONS 2

/// An answer, format 32: data32[0] is one of these, data32[1] a detail.
enum ws_answer {
	/// The command is done.
	WS_ANSWER_DONE = 1,
	/// The plugin could not do it; the detail is a WS_FAILED_* reason.
	WS_ANSWER_FAILED = 2,
	/// A click picked a window and closed the overview.
	WS_ANSWER_PICKED = 3,
	/// A close or toggle command came.
	WS_ANSWER_CLOSED = 4,
	/// Another button was pressed over an unfolded monitor; the detail is its number.
	WS_ANSWER_BUTTON = 5,
};

enum ws_failure {
	WS_FAILED_NO_CURRENT = 1,
	WS_FAILED_NO_OUTPUT = 2,
	WS_FAILED_INPUT = 3,
	WS_FAILED_UNKNOWN = 4,
	/// A monitor is unfolded into the overview, so the plugin doesn't switch
	/// workspaces.
	WS_FAILED_UNFOLDED = 5,
	/// Another program holds the keyboard or the pointer.
	WS_FAILED_GRAB = 6,
};
