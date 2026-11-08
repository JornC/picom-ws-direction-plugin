# Changelog

## 0.1.0 (unreleased)

The first version. The commits before it had no version.

It needs plugin API 0.2 of the picom fork (CI builds against the fork at `PICOM_REF`).

Upgrading from the unversioned plugin on `main`:

- picom's `hook-v2` branch, with plugin API 0.2, loads it too, as does the picom fork.
- The variables it had, `direction`, `monitor-x`, `monitor-y`, `monitor-width` and
  `monitor-height`, keep their names and meaning. `distance`, `faces`, `face`,
  `selected` and `placed` are new.
- The `WS_DIRECTION_LOG` line for an animation says `direction = N` where it said
  `workspace-direction = N`; change what greps for it.

Variables:

- Ten animation variables: `direction`, `distance`, `monitor-x`, `monitor-y`,
  `monitor-width`, `monitor-height`, `faces`, `face`, `selected` and `placed`, all under
  `plugin-ws-direction-`.
- `WS_DIRECTION_LOG` logs every decision, in picom's log format. Its first line is the
  plugin's version.

Build and tests:

- `make` finds picom's headers with `pkg-config picom-api`, or takes
  `PICOM_API_CFLAGS`. It uses `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `PKG_CONFIG`.
- `make install` and `make uninstall`, with `DESTDIR`, `PREFIX`, `PLUGINDIR`,
  `STYLEDIR`, `BINDIR` and `MANDIR` for packages.
- The version comes from `VERSION` and `git describe`.
- The harness, `tests/run.sh`, runs on Xvfb with i3 and a fake window manager.
- `make lint`, `make format` and `make format-check`; `SANITIZE=address,undefined`.
- A GitHub Actions workflow. It fetches the picom fork at `PICOM_REF`.
- A README for users and developers, and a CONTRIBUTING.md.
