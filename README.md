# ws-direction

A [picom](https://github.com/yshui/picom) plugin that tells animation scripts which way a
workspace switch went, and which monitor a window's workspace is on. With it a `show` or
`hide` animation can slide windows the right way and stay on their monitor. picom itself
knows nothing about workspaces: the plugin reads the window manager's EWMH properties and
hands the scripts five numbers.

It needs a picom with the animation variable hook (plugin API 0.2, yshui/picom pull
request: link to follow). An older picom logs `ws-direction: picom is too old` and runs
without the variables.

## Variables

`workspace-direction`
: 1 if the new workspace sorts after the old one, -1 if before, 0 if the animation was
  not caused by a workspace switch on the window's monitor (a new window, scratchpad,
  fullscreen, a window moved to another workspace).

`workspace-monitor-x`, `workspace-monitor-y`, `workspace-monitor-width`,
`workspace-monitor-height`
: The monitor that shows the window's workspace. For other windows and triggers: the
  monitor that fully contains the window, else the whole screen (like
  `window-monitor-*`). For a window that closes: always the whole screen, as the window
  is gone and can't be looked up; use `window-monitor-*` there.

Workspaces sort in the order of `_NET_DESKTOP_NAMES`; under i3 that is the order of the
workspace numbers, with unnumbered names after them.

## Build and install

    make

builds `build/ws-direction.so` against the installed `<picom/api.h>`. Against a picom
source tree instead:

    make PICOM_API_CFLAGS=-I/path/to/picom/include

Then give picom the plugin's path in its config file:

    plugins = "/path/to/build/ws-direction.so";

or on the command line with `--plugins`. picom releases up to v13 only find a bare file
name in their plugin directories when it comes from `--plugins`, not from the config file
(fixed in yshui/picom#1547).

## Example

Vertical slides on one monitor, and a short fade when the direction is 0. The script
language has no conditionals, so `var-slide = workspace-direction ^ 2` (1 for a slide,
0 for a fade) mixes the two cases.

    rules = ({
      match = "!override_redirect && window_type = 'normal'";
      animations = (
        {
          triggers = ["hide"];
          var-slide = "workspace-direction ^ 2";
          var-time = "0.25 * var-slide + 0.1 * (1 - var-slide)";
          offset-y = { curve = "cubic-bezier(0.25, 0.1, 0.25, 1)"; duration = "var-time"; start = 0; end = "0 - workspace-direction * workspace-monitor-height"; };
          opacity = { curve = "linear"; duration = "var-time"; start = "window-raw-opacity-before"; end = "window-raw-opacity-before * var-slide"; };
          crop-x = "workspace-monitor-x";
          crop-y = "workspace-monitor-y";
          crop-width = "workspace-monitor-width";
          crop-height = "workspace-monitor-height";
        },
        {
          triggers = ["show", "open"];
          var-slide = "workspace-direction ^ 2";
          var-time = "0.25 * var-slide + 0.1 * (1 - var-slide)";
          offset-y = { curve = "cubic-bezier(0.25, 0.1, 0.25, 1)"; duration = "var-time"; start = "workspace-direction * workspace-monitor-height"; end = 0; };
          opacity = { curve = "linear"; duration = "var-time"; start = "window-raw-opacity * var-slide"; end = "window-raw-opacity"; };
          crop-x = "workspace-monitor-x";
          crop-y = "workspace-monitor-y";
          crop-width = "workspace-monitor-width";
          crop-height = "workspace-monitor-height";
        }
      );
    });

`examples/picom-wsdir.conf` is the two-monitor version: vertical slides on one monitor,
horizontal on the other, each cropped to its own monitor.

## How it works

The plugin reads `_NET_CURRENT_DESKTOP`, `_NET_DESKTOP_NAMES` and `_NET_DESKTOP_VIEWPORT`
on the root window and `_NET_WM_DESKTOP` on client windows, the monitors from RandR, and
a window's place with `GetGeometry`. The order in which a window manager changes these
properties and maps or unmaps windows is not specified, and the changes can reach picom
over several frames, so the plugin does not act on events. It remembers, per monitor,
the workspaces shown recently and when it last saw a part of each switch. When a window
starts an animation whose script reads a variable, it looks for a recent switch to or
away from the window's workspace on its monitor. All five variables of a window are
worked out in one go, and the root window is read once per picom update pass
(`start->frame`), however many windows start in it.

## Limits

- A switch is only seen when a window starts an animation whose script reads the
  variables. A switch made while picom is unredirected (`unredir-if-possible`) may not be
  seen; the next switch is then judged against the last workspace the plugin saw.
- Each animation start costs one round trip to the X server, on picom's main thread. On
  a quiet server that is about 20 us (median 17 to 18 us, p99 about 60 us, measured on
  Xvfb with i3). The reply waits behind whatever the server is doing, so under load a
  start can take milliseconds (p99 0.2 to 3 ms and a maximum of 5 to 7 ms in runs with a
  window manager and builds running), and N windows starting in one pass wait N times. A
  workspace switch under i3 starts 4 to 7 animations in one pass.
- A switch stays matched to its windows for 100 ms, or for 2 passes within 250 ms when
  frames are slow. On a heavily loaded machine a switch can take longer; the windows
  that come late then get 0.
- The plugin reads X on its own connection. The read happens after picom has handled the
  events of the pass, but it is not ordered against picom's own request queue.

## Tests

`tests/run.sh` starts a nested Xvfb with two monitors, i3 and a tiny fake window
manager, and checks the direction logged for every window that shows, opens or hides,
plus screenshots of slowed-down slides. It needs Xvfb, xrandr, i3, xterm, xdotool, xprop,
ImageMagick, a C compiler and a picom with the hook:

    make
    PICOM=/path/to/picom tests/run.sh

Run it alone, not next to builds. Set `WS_DIRECTION_LOG` to a file to log every decision
there, in picom's log format.
