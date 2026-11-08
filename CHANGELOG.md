# Changelog

## 0.1.0 (unreleased)

The first version. The commits before it had no version.

It needs plugin API 0.9 of the picom fork (CI builds against the fork at `PICOM_REF`).

Upgrading from the unversioned plugin on `main`:

- picom's `hook-v2` branch, with plugin API 0.2, can't load it. The picom fork can.
- The variables it had, `direction`, `monitor-x`, `monitor-y`, `monitor-width` and
  `monitor-height`, keep their names and meaning. `distance`, `faces`, `face`,
  `selected`, `placed`, `index`, `shown`, `unfolded`, `monitor-width-mm`,
  `monitor-height-mm` and `backdrop` are new.
- `var-length` is set in every part of a style; the old README said once, near the top.
  For a slower switch, change it in every part.
- Copies of the old styles keep working. They have no `ws-pass` part, so they don't draw
  the workspaces a switch passes, as the shipped styles do.
- `make install` now also installs the command, `ws-direction`, and its man page.
- The `WS_DIRECTION_LOG` line for an animation says `direction = N` where it said
  `workspace-direction = N`; change what greps for it.

Variables:

- Sixteen animation variables: `direction`, `distance`, `monitor-x`, `monitor-y`,
  `monitor-width`, `monitor-height`, `faces`, `face`, `selected`, `placed`, `index`,
  `shown`, `unfolded`, `monitor-width-mm`, `monitor-height-mm` and `backdrop`, all under
  `plugin-ws-direction-`.
- They don't depend on the order in which the window manager changes its properties and
  maps or unmaps windows, nor on how long it takes: the plugin follows the maps and
  unmaps on its own connection, and a show or hide is part of a switch when the window
  manager showed or hid the window to make it.
- The root window and the managed windows are read at most once per pass. When the X
  server is slow to answer, the plugin pauses the screen instead of holding picom up.
- A window that closes, or hides as it closes, gets the monitor, faces and index it had.
- `WS_DIRECTION_LOG` logs every decision, in picom's log format. Its first line is the
  plugin's version.

Styles:

- Seven switch styles that move all workspaces of the monitor through `ws-pass`, with
  one strip they all carry: slide, push, wipe, fold, cascade, warp and zoom-pan. What a
  family of styles shares is in one part each in `styles/parts/`.
- Workspaces move along the short side of each monitor. `var-length` sets how long a
  switch takes. A switch made while one runs goes on from where the windows are.
- What a switch that interrupts another does is the user's choice: `var-interrupt` and
  `var-follow` in `parts/switch.conf`.
- The pass styles pass-slide, pass-zoom-pan and pass-zoom-cascade.
- cube and flip, turned in perspective through the corner outputs.

Switch passes:

- On every switch the plugin starts the trigger `ws-pass` on every window of the
  monitor, in the pass in which it sees the switch, so styles can draw the workspaces in
  between.
- A window that moves to the new workspace with the switch gets no `ws-pass`.
- The plugin declares `ws-pass`, so a config doesn't list it in `animation-triggers`.

Getting images back:

- When picom loses the images of hidden windows, after a config change or a window
  manager restart, the plugin gets them back: it freezes the screen, the keyboard and
  the pointer, has the window manager show each such workspace once, follows what the
  windows draw, and goes back.
- It leaves a monitor alone while that shows a workspace without windows.
- `ws-direction warm` gets them back when asked, and `ws-direction warm --auto off` stops
  the plugin from doing it by itself.

Overview:

- `ws-direction overview open` unfolds the monitor with the focus into an overview of its
  workspaces (`ws-unfold`), `close` folds every open monitor (`ws-fold`), and `toggle`
  does one or the other. `--all` opens every monitor, `--output NAME` one by its name.
- A switch while the overview is open starts `ws-unfold` again, and so does a change of
  its workspaces or windows. Windows on all desktops take part.
- An InputOnly window over each unfolded monitor takes its clicks. A left click on a
  window picks it: the window manager shows and focuses it, and the overview closes.
- The plugin declares `ws-unfold` and `ws-fold`.
- Backdrops, with `WS_DIRECTION_BACKDROP=wallpaper` or a color in picom's environment: a
  window per workspace that styles can draw, and a click on an empty tile picks its
  workspace.

Command:

- The command sends each command to the plugin as an event, through a selection and
  client messages, and fails with a message when picom doesn't run with the plugin.
- `ws-direction overview open --wait` returns 0 once a click picked a window, and 1 when
  a close or toggle command comes; with `--buttons` also when another button is pressed.
- `ws-direction --version`.
- A man page, `ws-direction(1)`.

Build and tests:

- `make` finds picom's headers with `pkg-config picom-api`, or takes
  `PICOM_API_CFLAGS`. It uses `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `PKG_CONFIG`.
- `make install` and `make uninstall`, with `DESTDIR`, `PREFIX`, `PLUGINDIR`,
  `STYLEDIR`, `BINDIR` and `MANDIR` for packages.
- The version comes from `VERSION` and `git describe`.
- `make check` runs the unit tests without an X server. `make check-harness` runs the
  harness, `tests/run.sh`, on Xvfb with i3 and a fake window manager, and
  `make check-all` both.
- `tests/check-frames.py` and `tests/scene.sh` check an animation frame by frame.
- `make lint`, `make format` and `make format-check`; `SANITIZE=address,undefined`.
- A GitHub Actions workflow. It fetches the picom fork at `PICOM_REF`.
- A README for users and developers, and a CONTRIBUTING.md.
