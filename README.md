# ws-direction

A [picom](https://github.com/yshui/picom) plugin that tells animation scripts which way a
workspace switch went, and which monitor a window's workspace is on. With it a `show` or
`hide` animation can move windows the right way and keep them on their monitor. It also
tells them where a window's workspace is among the monitor's workspaces, for effects
that draw the workspaces a switch passes. picom itself knows nothing about workspaces:
the plugin reads the window manager's EWMH properties and hands the scripts ten numbers.
What the scripts do with them is up to you; `styles/` has seven switch styles to start
from.

![Slide: workspace switches under i3 on two monitors, vertical on the landscape one,
horizontal on the portrait one](docs/slide.gif)

![All seven styles at once, going through the same workspace switches](docs/styles.gif)

The GIFs are recorded on Xvfb with i3 and picom's xrender backend, at a quarter of a
3440x1440 + 1080x1920 layout, with the windows cropped below the i3 bar, and cascade with
`use-damage = false` (see [Troubleshooting](#troubleshooting)); `docs/record-styles.sh`
makes them.

## Requirements

- **picom with plugin API 0.2 of the picom fork**: the animation variable hook. No picom
  release has it yet. It is proposed to picom upstream, and the picom fork this plugin
  is developed with has it. Upstream picom numbers its plugin API on its own (it is at
  0.1); once it has the hook, the plugin will ask for upstream's number. Any other picom
  prints `ws-direction: picom's plugin API is not compatible` and runs without the
  variables, so every animation whose script reads one fails to load.
- **X11**. The monitors come from RandR.
- **An EWMH window manager.** It is tested with i3 only. See
  [Window manager integration](#window-manager-integration) for what the plugin needs
  from it.
- To build: make, pkg-config, a C23 compiler (GCC 14 or clang 18 and later), and the
  headers of xcb and xcb-randr.

## Install

    make
    make install

`make` builds the plugin `build/ws-direction.so`. It finds picom's headers
(`<picom/api.h>`) with `pkg-config --cflags picom-api`. To build against a picom source
tree instead, run `make PICOM_API_CFLAGS=-I/path/to/picom/include`.

`make install` puts the plugin in `~/.config/picom/plugins/` and the styles in
`~/.config/picom/include/ws-direction/`. picom finds them there by name. `~/.config` is
`$XDG_CONFIG_HOME` when that is set. `PICOM_CONFIG_DIR`, `PLUGINDIR`, `STYLEDIR` and
`DESTDIR` change where things go. `make install` overwrites what is in the style
directory, so keep your own copies of styles elsewhere (see [Styles](#styles)).
`make uninstall`, with the same variables, removes what `make install` put in place, and
the style directory if that leaves it empty.

A package installs the plugin in picom's plugin directory, which is `picom/plugins/` under
the library directory picom was installed to, for example `/usr/lib/picom/plugins/`. The
picom fork finds it there by name; picom releases need its full path in `plugins`. The
styles go to `/etc/xdg/picom/include/ws-direction/`:

    make install DESTDIR=... PREFIX=/usr PLUGINDIR=/usr/lib/picom/plugins \
        STYLEDIR=/etc/xdg/picom/include/ws-direction

picom looks for styles there too, after `~/.config/picom/include/`.

Without `make install`, give the plugin and the style by their full path.

## Quick start

In `picom.conf`, load the plugin and include a style in a rule:

    plugins = "ws-direction.so";
    rules = ({
      match = "!override_redirect && window_type = 'normal'";
      @include "ws-direction/slide.conf"
    });

To check that it works:

- Start picom with `WS_DIRECTION_LOG=/path/to/file` in its environment. The plugin writes
  `ws-direction <version>` there when picom loads it, and then a line for every switch it
  sees and for every animation that reads its variables, with the values and why. If the
  plugin could not start, the line after the version says why.

## Styles

Each style is one file in `styles/`. To switch, change the name in the `@include` line;
picom reloads by itself when you save `picom.conf`. Workspaces move along the short side
of each monitor: up and down on a landscape monitor, sideways on a portrait one. When an
animation is not part of a workspace switch, every style gives the window a short fade.
Each style has a `var-length` near the top, in seconds: make it larger for a slower
switch.

- **slide**: the new workspace pushes the old one out.
- **push**: the new workspace slides in over the old one, which drifts back, like a card
  put on a stack.
- **wipe**: nothing moves; an edge sweeps across the monitor and uncovers the new
  workspace.
- **fold**: the old workspace folds into one edge while the new one unfolds from the
  other, like a flat cube turning.
- **cascade**: the old workspace slides out in one piece; the new windows follow one after
  another and land together.
- **warp**: the old workspace sinks into a point on one side of the middle of the
  monitor; then the new one bursts out of a point on the other side.
- **zoom-pan**: the view zooms out, pans to the new workspace and zooms back in, as if
  the workspaces were tiles side by side.

The styles are plain picom animation scripts, and each file explains in its header how
it works and which `var-*` you can change. To change one, copy it into your config
instead of including it. Or copy the file to `ws-direction/` next to your `picom.conf`
(`~/.config/picom/ws-direction/`) and keep the `@include`: picom looks there first, so
the copy wins over both install places, and neither `make install` nor a package writes
there. A copy in `~/.config/picom/include/ws-direction/` also wins over the package's,
but `make install` overwrites it. Save `picom.conf` once after you make a copy: picom
reloads when a file it read changes, and it didn't read the new copy yet.

## Writing a style

A style is a set of picom animations (see picom(1)) whose scripts read the plugin's
variables, listed under [Variables](#variables). A switch style moves a window by
`plugin-ws-direction-direction`, crops it to the `plugin-ws-direction-monitor-*` rect so
it doesn't spill onto the other monitor, and gives it a short fade when the direction is
zero. picom asks the plugin for the values only when an animation whose script reads one
starts.

## Reference

### Variables

`plugin-ws-direction-direction`
: 1 if the new workspace sorts after the old one, -1 if before, 0 if the animation is
  not part of a workspace switch on the window's monitor (a new window, scratchpad,
  fullscreen, a window moved to another workspace).

`plugin-ws-direction-distance`
: How many workspaces of the monitor the switch moved: 1 to the next one, 2 when it
  passes one, and so on; 0 when the direction is 0. Only workspaces that exist count, so
  under i3 an empty workspace that isn't shown doesn't, and neither do the workspaces of
  other monitors. A workspace that the window manager removes with the switch, like the
  empty one i3 leaves, counts where it was.

`plugin-ws-direction-monitor-x`, `plugin-ws-direction-monitor-y`,
`plugin-ws-direction-monitor-width`, `plugin-ws-direction-monitor-height`
: The monitor that shows the window's workspace. For a window that is not on one
  workspace: the monitor that fully contains the window, else the whole screen (like
  `window-monitor-*`). For a window that closes: always the whole screen, as the window
  is gone and can't be looked up; use `window-monitor-*` there.

`plugin-ws-direction-faces`
: How many workspaces that monitor has.

`plugin-ws-direction-face`
: How many of the monitor's workspaces the window's workspace is away from the
  reference workspace, counted like `distance`: positive if it sorts after it, negative
  if before. The reference workspace is the one the monitor showed before the switch, or
  the one it shows now if the animation is not part of a switch. So in a switch the
  windows that leave have 0, the windows that arrive have `direction * distance`, and a
  window of a workspace the switch passes has a value in between. 0 for a window that is
  not on one workspace.

`plugin-ws-direction-selected`
: 1 if that monitor shows the current workspace (`_NET_CURRENT_DESKTOP`, under i3 the
  focused one), else 0.

`plugin-ws-direction-placed`
: 0 if the window is on one workspace but its centre is not on that workspace's
  monitor, else 1. i3 doesn't place the windows of a hidden workspace, so a window keeps
  its old place until its workspace is shown. A window moved to a hidden workspace of
  another monitor is still on the first monitor then. A style that draws hidden
  workspaces should not draw it: multiply its opacity by `placed`.

The workspaces of a monitor are in the order of `_NET_DESKTOP_NAMES`. Under i3 that is
their order on the output: by number, with unnumbered names after them. A window is on
one workspace if the window
manager lists it in `_NET_CLIENT_LIST`, it is on one desktop (`_NET_WM_DESKTOP`, so not
sticky, and not in the scratchpad), and it isn't `_NET_WM_STATE_HIDDEN` (under i3: a tab
that isn't the selected one, which isn't seen on its workspace).

### Environment

`WS_DIRECTION_LOG`
: Set it in picom's environment to a file, and the plugin logs every decision there, in
  picom's log format. The first line is `ws-direction <version>`.

`DISPLAY`
: The plugin connects to the display it names, like picom itself. Without that
  connection the plugin adds no variables.
## Window manager integration

### i3

Under i3, the current workspace (`_NET_CURRENT_DESKTOP`) is the focused one, and each
monitor has workspaces of its own, which i3 tells through `_NET_DESKTOP_VIEWPORT`. i3
removes an empty workspace once it is hidden, so it doesn't count in `distance`. When
one i3 command switches workspaces on two monitors, `_NET_CURRENT_DESKTOP` ends on one
of them; the plugin sees the switch of the other from the windows i3 maps there.

### Other EWMH window managers

The plugin uses only what an EWMH window manager already does. It reads:

- on the root window: `_NET_CURRENT_DESKTOP`, `_NET_DESKTOP_NAMES` (the workspaces and
  their order), `_NET_DESKTOP_VIEWPORT` (which monitor each workspace is on) and
  `_NET_CLIENT_LIST` (the windows it manages);
- on client windows: `_NET_WM_DESKTOP` and `_NET_WM_STATE` (`_NET_WM_STATE_HIDDEN`);
- the monitors from RandR, and a window's place with `GetGeometry`.

## Troubleshooting

- **Nothing animates, and picom logs `variable name "plugin-ws-direction-direction" is
  not defined`.** The plugin's variables aren't there. Either picom didn't load the
  plugin (picom up to v13 finds it by name only from `--plugins`; give the full path),
  or the plugin could not start: then a line that starts with `ws-direction:` on picom's
  standard error says why, and so does `WS_DIRECTION_LOG` (the plugin doesn't write to
  picom's `--log-file`). `picom's plugin API is not compatible` means it needs another
  plugin API of the picom fork; others say it could not connect to the X server.
- **Windows blink when you go back to a workspace while its windows still move away.**
  This happens with picom releases. When a window maps again, picom draws its new pixmap
  from the first damage on. On some drivers (seen on NVIDIA) the X server sends that
  damage itself, before the program draws, while the pixmap still holds what lies under
  the window, so for a few frames the window shows the wallpaper. picom's usual fade-in
  hides this; styles that keep windows opaque during a switch do not. The fork's main
  keeps drawing the old image until the window drew all of its new pixmap, or until its
  animation ends; a window whose size changed while its workspace was hidden shows its
  old image stretched to its new size until then.
- **cascade flickers.** With picom's xrender backend, during a switch, a window that is
  already moving in is sometimes not drawn for a frame or two. That is a bug in the
  xrender backend, not in the style; the glx backend doesn't have it, and with
  `use-damage = false` it doesn't happen either.
- **A switch has no animation, or a window that shows or hides fades.** A switch is only
  seen when a window starts an animation whose script reads the variables, and a window
  belongs to a switch for 100 ms, or for 2 passes within 250 ms when frames are slow. A
  switch made while picom has the screen unredirected (`unredir-if-possible`), and a
  window that comes later on a heavily loaded machine, get 0.
- **A changed copy of a style doesn't take effect.** Save `picom.conf` once after you
  make the copy, so picom reads it.

## How it works

The plugin reads the properties listed under
[Other EWMH window managers](#other-ewmh-window-managers). The order in which a window
manager changes these properties and maps or unmaps windows is not specified, and the
changes can reach picom over several frames, so the plugin does not act on events. It
remembers, per monitor, the workspaces shown recently and when it last saw a part of
each switch. When a window starts an animation whose script reads a variable, it looks
for a recent switch to or away from the window's workspace on its monitor. All
variables of a window are worked out in one go, and the root window is read once per
picom update pass, however many windows start in it.

The plugin reads X on its own connection. The read happens after picom has handled the
events of the pass, but it is not ordered against picom's own request queue.

## Development

[CONTRIBUTING.md](CONTRIBUTING.md) says how to build against the picom fork, run the
harness (`tests/run.sh`), and what the rules for commits are.
[CHANGELOG.md](CHANGELOG.md) lists what changed. The plugin is under the MPL-2.0, see
`LICENSE`.
