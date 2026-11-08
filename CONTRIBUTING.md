# Contributing to ws-direction

This is for working on the plugin itself: building it, testing it, and the rules for
commits. For using it, see the [README](README.md).

## Building against the picom fork

The plugin needs plugin API 0.9 of the picom fork. No picom release has it yet; it is on
the `main` branch of the picom fork the plugin is developed with (github.com/JornC/picom),
which is picom's `next` branch plus one commit per feature. That branch is not published
yet. CI builds against it at `PICOM_REF` in `.github/workflows/ci.yml`.

Build the fork as picom says, with meson and ninja:

    meson setup --buildtype=release build
    ninja -C build

The binary is `build/src/picom`. Install it (`ninja -C build install`), and `make` here
finds its headers with `pkg-config --cflags picom-api`. Or leave it uninstalled and point
`make` at the source tree:

    make PICOM_API_CFLAGS=-I/path/to/picom/include

If neither gives the headers, `make` stops with a message that says so. Headers without
the plugin API the plugin needs stop the build at the check in `src/ws_direction.h`.

The code is C23 (`-std=gnu23`): GCC 14 or clang 18 and later. It needs pkg-config and the
headers of xcb, xcb-damage, xcb-randr and xcb-xfixes.

The version is in the file `VERSION`. In a git checkout, `make` adds what `git describe`
says: `0.1.0-12-gabc1234` once the tag `v0.1.0` exists, `0.1.0-gabc1234` before that,
and `-dirty` for uncommitted changes. A release tarball has no `.git` and builds as plain
`0.1.0`. `make VERSION=x` sets it by hand. It ends up in `build/version.h`, in
`ws-direction --version`, and in the first line of `WS_DIRECTION_LOG`.

## Make targets

- `make` (or `make all`) builds the plugin `build/ws-direction.so` and the command
  `build/ws-direction`.
- `make check` builds and runs the unit tests, `build/unit-tests`. It needs no X server,
  so a package's check step can run it. `build/unit-tests WORD` runs only the tests
  whose name has WORD in it.
- `make check-harness` builds everything and runs the harness, `tests/run.sh`. The
  harness's variables (`PICOM`, `WSDIR_DISPLAY` and the others below) reach it from
  make's command line or the environment.
- `make check-all` runs both.
- `make lint` runs shellcheck on `tests/*.sh` and `docs/*.sh`.
- `make format` formats the C files with clang-format; `make format-check` only checks.
- `make install` and `make uninstall`, see the README.
- `make clean` removes `build/`.

Variables:

- `PICOM_API_CFLAGS`: where picom's headers are, see above.
- `CPPFLAGS`, `CFLAGS` (default `-O2 -g`) and `LDFLAGS` work as usual; the flags the
  code needs are added to them.
- `PKG_CONFIG` is the pkg-config to run (default `pkg-config`).
- `SANITIZE=address,undefined` builds with those sanitizers, and stops at the first error.
  Run `make clean` when you change it, so every object is built the same way. A plugin
  built with ASan, loaded into a picom built without it, needs libasan preloaded
  (`LD_PRELOAD`).
- `CLANG_FORMAT` is the clang-format to run (default `clang-format`).
- `VERSION`, see above.

## Tests

### Unit tests

`tests/unit` has 79 tests of the logic that works on the plugin's state: which monitor a
workspace is on, the order of a monitor's workspaces, the steps of a switch, which
workspace each monitor shows, which switch each show and hide is part of, and the
variables, through `ws_compute`. They run in about 10 ms, without an X server. They link
`desktop.o`, `passes.o`, `overview.o`, `backdrop.o` and `warm.o`; `tests/unit/fixture.c`
stands in for `plugin.c` (the state, a log into a buffer and a fake clock), gives a
picom API that records the animations started, and plays what a window manager does:
maps, unmaps and changes of the current desktop, as the events a read takes in.

`tests/unit/test.h` is the runner, about 100 lines. Write a test as `TEST(name) { ... }`
in any `tests/unit/*.c`; the runner finds it. `CHECK`, `CHECK_INT`, `CHECK_STR` and
`CHECK_HAS` end the test when they fail. CI runs them with gcc and clang, with
`-Werror`, and with ASan and UBSan.

Without X the unit tests can't reach `desktop.c`'s reading of the X server's replies,
`warm.c`, the clicks and the commands in `overview.c`, `plugin.c`'s frame callback
and constructor, or `cmd.c`. The harness covers those.

### The harness

`tests/run.sh` starts a nested Xvfb with two monitors (a landscape 3440x1440 and a
portrait 1080x1920), runs i3 and then a tiny fake window manager (`tests/fakewm.c`) on
it, and picom with the plugin and the styles. It checks the variables the plugin logs
for every window that shows, opens or hides, takes screenshots partway through slowed
down switches, and checks the cases the units so far added, and `make install` and
`make uninstall`. Its header lists every case. It prints PASS or FAIL per case, and
exits 1 if one failed.

It needs Xvfb with GLX (Mesa), xrandr, i3 and i3-msg, xterm, xdotool, xprop,
ImageMagick 7 (`import` and `magick`), dbus-run-session and dbus-send, jq, a C compiler,
make and pkg-config. And a picom with the plugin API the plugin needs: the fork's
`main`. Give it as `PICOM`:
`PICOM`:

    make
    PICOM=/path/to/picom/build/src/picom tests/run.sh

or `make check-harness PICOM=...`. Environment:

- `PICOM`: the picom binary, default picom on the PATH.
- `I3`: default `/usr/bin/i3`.
- `WSDIR_DISPLAY`: the display it runs on, default `:91`. It refuses `:0`, and stops if
  the display is in use.
- `WSDIR_WORK`: where the i3 socket and the logs go, default
  `$XDG_RUNTIME_DIR/wsdir-91` (`/tmp/wsdir-91` without `XDG_RUNTIME_DIR`). The logs
  and screenshots are in `picom-wsdir/` there.
- `WSDIR_SETTLE`: seconds to wait after each step, default 0.7.
- `WSDIR_PLUGIN`: the plugin, default `build/ws-direction.so`.

Run it alone, not next to builds: its screenshots are timed, and on a loaded machine they
can miss the moment they look for. Rerun a failure alone before you treat it as a bug.
Don't edit `tests/run.sh` while it runs: bash reads the script as it goes.

`docs/record-styles.sh WORKDIR` makes the GIFs in `docs/` with `docs/record-demo.sh`,
on display `:205` (`DEMO_DISPLAY`). It needs Xvfb, i3, xterm, ffmpeg and the DejaVu
fonts.

## Code layout

- `src/plugin.c`: the hooks into picom, the frame callback, and the constructor that
  adds the variables and declares the triggers.
- `src/desktop.c`: reads the monitors, the workspaces and the managed windows from the
  X server.
- `src/passes.c`: finds switches, starts `ws-pass`, and works out the variables.
- `src/warm.c`: gets the images of hidden workspaces back.
- `src/overview.c`: unfolds and folds the overview, takes the clicks, and does the
  commands.
- `src/backdrop.c`: the backdrops, one window per workspace.
- `src/cmd.c`: the `ws-direction` command.
- `src/protocol.h`: what the plugin and the command share. `src/ws_direction.h`: the
  plugin's internal header.
- `styles/`: the styles.
- `man/ws-direction.1`: the man page, in plain roff.

## Code style

- C is formatted with clang-format 23, as in `.clang-format`: run `make format`. Other
  major versions format differently. CI checks with 23.1.3, which pip installs:
  `pip install clang-format==23.1.3`.
- `.editorconfig` says how the other files are written: tabs for shell scripts and the
  Makefile, four spaces for Python, two for the styles and YAML.
- `make lint` is clean with shellcheck 0.11.0, the version CI uses. Disable a warning only
  on the line it is about, with a comment that says why.
- Comments: few, and short. A doc comment (`///`) says what a function, type or field is
  for, in a sentence or two. Don't repeat what the code says. Write plain words.
- The plugin's interface (the variables, the triggers, `WS_DIRECTION_LOG` and the
  command) is documented in two places: the README's Reference and the man page. Change
  both. The code names the variables and points there, so the docs don't drift apart.
- New files start with the SPDX line, `MPL-2.0`, as the others do.
- A style explains itself in its header: what it looks like, how to use it, and the
  `var-*` you can change; a part says which `var-*` it sets and which it needs. What
  styles share goes in a part, so it is set in one place.

## Commits

- Never rewrite history: no amend, rebase, squash or force-push, also not for a commit
  that is not pushed yet. Fix a mistake in a new commit on top.
- One topic per commit. The subject says what changes, in plain words; the body says why.
- Before a commit, run `make format-check`, `make lint` and `make check`. Run the
  harness for a change to what the plugin does or to a style.

## A new picom plugin API version

When the plugin starts to use a call from a newer plugin API, say 0.9:

1. `src/ws_direction.h`: raise `WS_API_MINOR`. The build check, the call to
   `picom_api_get_interfaces` and the "plugin API is not compatible" messages take the
   version from it. picom gives no interfaces when its API is older than asked, or of
   another major version; the plugin then adds no variables and no triggers.
2. Change the other places that name the version: the README (Requirements: the
   version, what it adds, and the fork tag it was tested against), the comment at the
   top of the Makefile (the list of calls), the header of `tests/run.sh` and the message
   after its check for "plugin API is not compatible", the man page (THE PLUGIN), and
   the doc comments that name the version of a call. `grep -rn '0\.8'` finds them.
3. Tag the fork commit that has it, named after the next plugin version (for example
   `ws-direction-0.2.0`), rather than writing down its hash: the fork's `main` is rebuilt
   on top of picom's `next`, and old hashes leave it. Push the tag to
   github.com/JornC/picom, or CI can't fetch it, and never move it. Point CI at it:
   `PICOM_REF` in `.github/workflows/ci.yml`. The README (Requirements) and "Building
   against the picom fork" above point at `PICOM_REF`.
4. Add a line to `CHANGELOG.md`.

## Releasing

1. Set the new version in `VERSION`, for example `0.2.0`.
2. In `CHANGELOG.md`, give its heading the date in place of "(unreleased)".
3. In `man/ws-direction.1`, change the date and the version in the `.TH` line, and the
   tag in the README link under SEE ALSO.
4. Commit, tag the commit `v0.2.0`, and push both. `make` then builds as `0.2.0` there.
   A GitHub tag tarball has no `.git`, and builds as what `VERSION` says.
5. Tag the picom fork commit the release was tested against `ws-direction-0.2.0`, and
   push the tag to github.com/JornC/picom. Point CI and the README at it, as in the
   steps above.
