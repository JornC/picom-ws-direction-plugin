# Changelog

## 0.1.0 (unreleased)

The first version. The commits before it had no version.

It needs plugin API 0.5 of the picom fork (CI builds against the fork at `PICOM_REF`).

Upgrading from the unversioned plugin on `main`:

- picom's `hook-v2` branch, with plugin API 0.2, can't load it. The picom fork can.
- The variables it had, `direction`, `monitor-x`, `monitor-y`, `monitor-width` and
  `monitor-height`, keep their names and meaning. `distance`, `faces`, `face`,
  `selected` and `placed` are new.
- The `WS_DIRECTION_LOG` line for an animation says `direction = N` where it said
  `workspace-direction = N`; change what greps for it.

Variables:

- Ten animation variables: `direction`, `distance`, `monitor-x`, `monitor-y`,
  `monitor-width`, `monitor-height`, `faces`, `face`, `selected` and `placed`, all under
  `plugin-ws-direction-`.
- The root window and the managed windows are read at most once per pass. When the X
  server is slow to answer, the plugin pauses the screen instead of holding picom up.
- `WS_DIRECTION_LOG` logs every decision, in picom's log format. Its first line is the
  plugin's version.

Switch passes:

- On every switch the plugin starts the trigger `ws-pass` on every window of the
  monitor, in the pass in which it sees the switch, so styles can draw the workspaces in
  between.
- A window that moves to the new workspace with the switch gets no `ws-pass`.
- The plugin declares `ws-pass`, so a config doesn't list it in `animation-triggers`.

Build and tests:

- `make` finds picom's headers with `pkg-config picom-api`, or takes
  `PICOM_API_CFLAGS`. It uses `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `PKG_CONFIG`.
- `make install` and `make uninstall`, with `DESTDIR`, `PREFIX`, `PLUGINDIR`,
  `STYLEDIR`, `BINDIR` and `MANDIR` for packages.
- The version comes from `VERSION` and `git describe`.
- `make check` runs the unit tests without an X server. `make check-harness` runs the
  harness, `tests/run.sh`, on Xvfb with i3 and a fake window manager, and
  `make check-all` both.
- `make lint`, `make format` and `make format-check`; `SANITIZE=address,undefined`.
- A GitHub Actions workflow. It fetches the picom fork at `PICOM_REF`.
- A README for users and developers, and a CONTRIBUTING.md.
