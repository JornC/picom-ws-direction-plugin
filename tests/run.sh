#!/usr/bin/env bash
# Test harness for the ws-direction picom plugin, and so for the picom plugin API it builds
# on (animation variables, frame callbacks, start_animation, window_has_image,
# add_animation_trigger, pause_screen, add_wakeup_fd and window_at: plugin API 0.9).
#
# Setup: a nested Xvfb with two monitors (DPT 3440x1440 at +0+480, HDT 1080x1920 at
# +3440+0). It checks that the command prints its version and exits 2 on wrong
# arguments. Before any window manager runs, it checks with picom --diagnostics that
# every style in styles/ loads without an animation-triggers line, and that a config
# that still lists the plugin's triggers loads; and that make uninstall removes what
# make install put in place, the man page too.
#
# Phase 1: stock i3, xterms on several workspaces, and picom with the plugin,
# styles/slide.conf and debug logging. It checks that the plugin's first log line is its
# version, the command's. It runs i3 commands through i3-msg and checks
# the direction (and for some switches the distance and the other variables) that the
# plugin logs for every window that shows, opens or hides. It also takes screenshots in
# the middle of slowed-down slides, to check the slide style slides the right way and
# stays on its monitor. Then it runs picom with
# tests/picom-anim.conf, whose scripts don't read the variables, and checks the plugin
# is loaded but never asked (the hook calls a provider only for scripts that read its
# variable), and with a config without "open" animations to check the startup seed.
#
# Phase 1 also starts a custom animation trigger over picom's D-Bus interface
# (StartAnimations) on an xterm of a hidden workspace, and checks a screenshot shows it
# while the animation runs, and that started right after a switch on the xterms of the
# workspaces in between, it is still part of no switch: the plugin didn't start it. Then
# it checks that the plugin itself starts the custom trigger ws-pass on the xterms of the
# workspaces a switch passes, in the pass of the switch, and takes a screenshot halfway
# through such a switch. The harness runs on a D-Bus session bus of its own.
#
# Phase 2: i3 is replaced by fakewm.c, a tiny stand-in window manager. It can spread one
# workspace switch over several picom frames, and set up cases i3 won't produce on demand
# (no _NET_DESKTOP_VIEWPORT, sticky windows, a window on the other monitor showing during
# a switch, a hidden tab and a window the window manager doesn't list showing during a
# switch), and an X server that is slow to answer during a switch.
#
# Prints PASS/FAIL per case and exits 1 if any case failed. Everything runs on
# $WSDIR_DISPLAY (default :91), never on the real display, and only processes started
# here are stopped.
#
# Run it alone, not next to builds: its screenshots are timed, and on a loaded machine
# they can miss the moment they look for. Rerun a failure alone before treating it as a
# bug.
#
# Usage: make && tests/run.sh, or make check-harness; make check-all runs the unit tests
# first.
# Environment: PICOM (picom binary, default: picom on the PATH; it must have plugin API
# 0.9), I3 (default /usr/bin/i3),
# WSDIR_DISPLAY (default :91), WSDIR_WORK (directory for the i3 socket and logs, default
# $XDG_RUNTIME_DIR/wsdir-<display number>), WSDIR_SETTLE (seconds to wait after each
# step, default 0.7), WSDIR_PLUGIN (the plugin, default build/ws-direction.so).
# $XDG_RUNTIME_DIR/wsdir-<display number>, or /tmp/wsdir-<display number> without
# XDG_RUNTIME_DIR), WSDIR_SETTLE (seconds to wait after each step, default 0.7),
# WSDIR_PLUGIN (the plugin, default build/ws-direction.so).

set -u

# picom's D-Bus interface goes on a session bus of our own.
if [[ -z ${WSDIR_OWN_BUS:-} ]]; then
	command -v dbus-run-session >/dev/null || {
		echo "ERROR: missing tool: dbus-run-session" >&2
		exit 2
	}
	WSDIR_OWN_BUS=1 exec dbus-run-session -- "$0" "$@"
fi

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PICOM=${PICOM:-$(command -v picom)}
I3=${I3:-/usr/bin/i3}
STYLE=$ROOT/styles/slide.conf
PLAIN_CONFIG=$HERE/picom-anim.conf
DISP=${WSDIR_DISPLAY:-:91}
WORK=${WSDIR_WORK:-${XDG_RUNTIME_DIR:-/tmp}/wsdir-${DISP#:}}
# A unix socket path must fit in 107 bytes, so the socket sits directly in $WORK, and the
# default $WORK is short.
SOCK=$WORK/ipc91
OUT=$WORK/picom-wsdir
SETTLE=${WSDIR_SETTLE:-0.7}
PLUGIN=${WSDIR_PLUGIN:-$ROOT/build/ws-direction.so}

export DISPLAY=$DISP

die() {
	echo "ERROR: $*" >&2
	exit 2
}

[[ $DISP != ":0" ]] || die "refusing to run on the real display :0"
[[ ${#SOCK} -le 107 ]] || die "socket path too long (${#SOCK} bytes): $SOCK"
[[ -n $PICOM && -x $PICOM ]] || die "picom not found, set PICOM"
for tool in "$I3" Xvfb xrandr i3-msg xterm xdotool xprop import magick cc pkg-config \
	dbus-send; do
	command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ ! -e /tmp/.X${DISP#:}-lock ]] || die "display $DISP is already in use"
mkdir -p "$OUT" || die "cannot create $OUT"
rm -f "$OUT"/*.log "$OUT"/*.out "$SOCK"
[[ -f $PLUGIN ]] || die "plugin not found: $PLUGIN (run make)"
# picom loads a plugin by its full path, and /proc/<pid>/maps shows that path.
PLUGIN=$(realpath "$PLUGIN")
# The slide style in a rule for normal windows. Copied in, not included, so the checks
# below can change its timing with sed.
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	cat "$STYLE"
	echo '});'
} >"$OUT/wsdir-plugin.conf"
CONFIG=$OUT/wsdir-plugin.conf
# Phase 1 uses the same. The plugin declares ws-pass, but slide has no animation for it, so
# picom ignores the plugin's starts, and every show and hide runs as it would without them.
I3_CONFIG=$CONFIG
# A custom trigger that draws a window, hidden or not, for 2 s, cropped to the monitor
# the plugin gives. Normal windows keep their image while hidden. Nothing else reads a
# variable of the plugin: it sees each workspace switch from its frame callback.
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	echo 'animation-triggers = ["wave"];'
	cat <<'EOF'
rules = ({
  match = "!override_redirect && window_type = 'normal'";
  keep-image-when-hidden = true;
  animations = ({
    triggers = ["wave"];
    opacity = "window-mapped-opacity";
    crop-x = "plugin-ws-direction-monitor-x";
    crop-y = "plugin-ws-direction-monitor-y";
    crop-width = "plugin-ws-direction-monitor-width";
    crop-height = "plugin-ws-direction-monitor-height";
    offset-x = { duration = 2; start = 0; end = 0; };
  });
});
EOF
} >"$OUT/wave.conf"
# One block for every window of a switch, the ones that hide and show and the ones of the
# workspaces it passes (ws-pass): it moves from face `face` to face
# `face - direction * distance`, one monitor height per face, cropped to the monitor.
# Linear and slow, so a screenshot halfway shows the workspace in between. ws-pass
# suppresses show and hide, so a show or hide that comes later does not restart it.
PASS_LENGTH=4
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	cat <<EOF
rules = ({
  match = "!override_redirect && window_type = 'normal'";
  keep-image-when-hidden = true;
  animations = ({
    triggers = ["open", "show", "hide"];
    opacity = "window-mapped-opacity * plugin-ws-direction-placed";
    offset-y = {
      duration = $PASS_LENGTH;
      start = "plugin-ws-direction-face * plugin-ws-direction-monitor-height";
      end = "(plugin-ws-direction-face - plugin-ws-direction-direction * plugin-ws-direction-distance) * plugin-ws-direction-monitor-height";
    };
    crop-x = "plugin-ws-direction-monitor-x";
    crop-y = "plugin-ws-direction-monitor-y";
    crop-width = "plugin-ws-direction-monitor-width";
    crop-height = "plugin-ws-direction-monitor-height";
  }, {
    triggers = ["ws-pass"];
    suppressions = ["show", "hide"];
    opacity = "window-mapped-opacity * plugin-ws-direction-placed";
    offset-y = {
      duration = $PASS_LENGTH;
      start = "plugin-ws-direction-face * plugin-ws-direction-monitor-height";
      end = "(plugin-ws-direction-face - plugin-ws-direction-direction * plugin-ws-direction-distance) * plugin-ws-direction-monitor-height";
    };
    crop-x = "plugin-ws-direction-monitor-x";
    crop-y = "plugin-ws-direction-monitor-y";
    crop-width = "plugin-ws-direction-monitor-width";
    crop-height = "plugin-ws-direction-monitor-height";
  });
});
EOF
} >"$OUT/pass.conf"
# pass_time DISTANCE FRACTION: seconds until FRACTION of a pass.conf switch of DISTANCE
# workspaces is done.
pass_time() {
	awk -v l="$PASS_LENGTH" -v f="$2" 'BEGIN { print l * f }'
}

# --- process handling: only ever stop PIDs started here ---

PIDS=()
LAST_PID=
cleanup() {
	local i p
	for ((i = ${#PIDS[@]} - 1; i >= 0; i--)); do
		kill "${PIDS[i]}" 2>/dev/null
	done
	sleep 0.3
	for p in "${PIDS[@]}"; do
		kill -9 "$p" 2>/dev/null
	done
	wait 2>/dev/null
	rm -f "$SOCK"
}
trap cleanup EXIT

# run_bg LOGFILE CMD...: start CMD in the background and remember its PID.
run_bg() {
	local log=$1
	shift
	"$@" >"$log" 2>&1 &
	LAST_PID=$!
	PIDS+=("$LAST_PID")
}

# stop [-INT] PID...: stop processes started here, and forget them. The default signal
# is SIGTERM; -INT sends SIGINT instead.
stop() {
	local sig=-TERM p q drop keep=()
	if [[ $1 == -INT ]]; then
		sig=$1
		shift
	fi
	for p in "$@"; do
		kill "$sig" "$p" 2>/dev/null
	done
	for p in "$@"; do
		wait "$p" 2>/dev/null
	done
	for q in "${PIDS[@]}"; do
		drop=0
		for p in "$@"; do
			[[ $q == "$p" ]] && drop=1
		done
		((drop)) || keep+=("$q")
	done
	PIDS=("${keep[@]}")
}

# stop_picom: picom handles SIGINT (not SIGTERM), so this way it shuts down in order and
# a sanitizer build runs its leak check at exit.
stop_picom() { stop -INT "$PICOM_PID"; }

# wait_until SECONDS CMD...: retry CMD until it succeeds.
wait_until() {
	local deadline=$((SECONDS + $1))
	shift
	until "$@" >/dev/null 2>&1; do
		((SECONDS < deadline)) || return 1
		sleep 0.1
	done
}

win_id() { xdotool search --name "^$1\$" 2>/dev/null | head -n 1; }
has_window() { [[ -n $(win_id "$1") ]]; }

# start_xterm INSTANCE TITLE [XTERM ARGS...]
start_xterm() {
	local inst=$1 title=$2
	shift 2
	run_bg /dev/null xterm -name "$inst" -T "$title" "$@" -e sleep 1000000
	wait_until 10 has_window "$title" || die "xterm $title did not appear"
}

i3cmd() {
	local out
	out=$(i3-msg -s "$SOCK" "$1" 2>&1)
	[[ $out != *'"success":false'* ]] || echo "  note: i3 command failed: $1: $out"
}

fakewm() { "$OUT/fakewm" "$@" || die "fakewm $* failed"; }

LOG=
PICOM_PID=
# start_picom CONFIG NAME
start_picom() {
	LOG=$OUT/$2.log
	# The plugin logs its decisions into picom's log, in the same format.
	export WS_DIRECTION_LOG=$LOG
	run_bg "$OUT/$2.out" "$PICOM" --config "$1" --dbus --log-level debug --log-file "$LOG"
	PICOM_PID=$LAST_PID
	wait_until 15 grep -q "Screen redirected" "$LOG" || die "picom did not start, see $OUT/$2.out"
	sleep 1
}

# --- checks ---

PASSES=0
FAILS=0
GOT=
pass() {
	PASSES=$((PASSES + 1))
	echo "PASS  $1"
}
fail() {
	FAILS=$((FAILS + 1))
	echo "FAIL  $1"
	shift
	local line
	for line in "$@"; do
		echo "        $line"
	done
}

log_mark() { wc -l <"$LOG"; }

# The directions picom logged after line $1, one "title trigger direction" per line.
# "open" counts as "show": a window's first show after picom started is an open.
directions_since() {
	tail -n +"$(($1 + 1))" "$LOG" |
		sed -nE 's/.*\] (open|show|hide) of window 0x[0-9a-f]+ \((.*)\): direction = (-?[0-9]+) \(.*/\2 \1 \3/p' |
		sed -E 's/ open / show /' | sort -u
}

# The same for the distances, one "title trigger distance" per line.
distances_since() {
	tail -n +"$(($1 + 1))" "$LOG" |
		sed -nE 's/.*\] (open|show|hide) of window 0x[0-9a-f]+ \((.*)\): direction = .*, distance ([0-9]+), .*/\2 \1 \3/p' |
		sed -E 's/ open / show /' | sort -u
}

# All variables picom got after line $1 for the triggers that match $2, one "title
# trigger direction D distance N face F/FACES selected S placed P monitor X,Y WxH" per
# line.
variables_since() {
	tail -n +"$(($1 + 1))" "$LOG" |
		sed -nE 's/.*\] ('"$2"') of window 0x[0-9a-f]+ \((.*)\): direction = (-?[0-9]+) \(.*\), distance ([0-9]+), face (-?[0-9]+) of ([0-9]+), selected ([01]), placed ([01]), monitor ([0-9]+,[0-9]+ [0-9]+x[0-9]+), .*/\2 \1 direction \3 distance \4 face \5\/\6 selected \7 placed \8 monitor \9/p' |
		sed -E 's/ open / show /' | sort -u
}

# The monitors, as variables_since prints them.
DPT_RECT="monitor 0,480 3440x1440"
HDT_RECT="monitor 3440,0 1080x1920"

# expect NAME [none | "title trigger direction"...]: compare $GOT with the expectation.
# The logged set must match exactly: no missing and no extra windows.
expect() {
	local name=$1 want=
	shift
	if [[ $1 != none ]]; then
		want=$(printf '%s\n' "$@" | sort -u)
	fi
	if [[ $GOT == "$want" ]]; then
		pass "$name"
	else
		fail "$name" "expected: $(tr '\n' ' ' <<<"${want:-nothing}")" \
			"got:      $(tr '\n' ' ' <<<"${GOT:-nothing}")"
	fi
}

# i3_case NAME I3-COMMAND EXPECTATION...
i3_case() {
	local name=$1 cmd=$2
	shift 2
	CASE_MARK=$(log_mark)
	i3cmd "$cmd"
	sleep "$SETTLE"
	GOT=$(directions_since "$CASE_MARK")
	expect "$name" "$@"
}

# fake_case NAME "FAKEWM STEPS" EXPECTATION...
fake_case() {
	local name=$1 steps=$2
	shift 2
	CASE_MARK=$(log_mark)
	# shellcheck disable=SC2086
	fakewm $steps
	sleep "$SETTLE"
	GOT=$(directions_since "$CASE_MARK")
	expect "$name" "$@"
}

# distances NAME EXPECTATION...: check the distances of the case before, as "title
# trigger distance".
distances() {
	GOT=$(distances_since "$CASE_MARK")
	expect "$@"
}

# variables NAME EXPECTATION...: check all variables of the shows, opens and hides of the
# case before, as variables_since prints them.
variables() {
	GOT=$(variables_since "$CASE_MARK" 'open|show|hide')
	expect "$@"
}

# pass_case NAME MARK SWITCH WINDOW...: after line MARK, the plugin saw the switch SWITCH
# (as it logs it, like "1:IDE" -> "4:Misc") on the root window, and started ws-pass on each
# WINDOW once, all in that same pass. Shows and hides with a direction are reported.
pass_case() {
	local name=$1 mark=$2 switch=$3 lines switch_pass want='' got='' title passes others
	shift 3
	lines=$(tail -n +"$((mark + 1))" "$LOG")
	switch_pass=$(grep -F "$switch" <<<"$lines" | grep -m1 "(from the root window, in pass" |
		sed -nE 's/.*in pass ([0-9]+)\)$/\1/p')
	for title in "$@"; do
		passes=$(sed -nE 's/.*\] ws-pass of window 0x[0-9a-f]+ \('"$title"'\): .*, pass ([0-9]+)$/\1/p' <<<"$lines" |
			tr '\n' ' ')
		want+=" $switch_pass"
		got+=" ${passes% }"
	done
	others=$(sed -nE 's/.*\] (open|show|hide) of window 0x[0-9a-f]+ \((.*)\): direction = -?1 .*, pass ([0-9]+)$/\2:\3/p' <<<"$lines" |
		tr '\n' ' ')
	if [[ -n $switch_pass && $got == "$want" ]]; then
		pass "$name (pass $switch_pass; shows and hides: $others)"
	else
		fail "$name" "switch in pass ${switch_pass:-none}, ws-pass in passes:$got" \
			"shows and hides: $others"
	fi
}

# Time (ms since midnight) of the first log line after line $1 that matches $2.
log_time_ms() {
	tail -n +"$(($1 + 1))" "$LOG" | grep -m1 -E "$2" |
		sed -nE 's/^\[ [0-9/]+ ([0-9]+):([0-9]+):([0-9]+)\.([0-9]+) .*/\1 \2 \3 \4/p' |
		awk '{ printf "%d\n", ((($1 * 60) + $2) * 60 + $3) * 1000 + $4 }'
}

current_desktop() { xprop -root _NET_CURRENT_DESKTOP | sed -nE 's/.* = ([0-9]+)$/\1/p'; }

# --- set up the X server ---

echo "== setup: Xvfb on $DISP with two monitors"
run_bg "$OUT/xvfb.log" Xvfb "$DISP" -screen 0 4520x1920x24 +extension GLX +extension RANDR \
	-nolisten tcp -noreset
wait_until 10 xprop -root || die "Xvfb did not start"
xrandr --setmonitor DPT 3440/800x1440/337+0+480 screen >/dev/null
xrandr --setmonitor HDT 1080/300x1920/530+3440+0 none >/dev/null

read -ra XCB_FLAGS <<<"$(pkg-config --cflags --libs xcb)"
cc -O2 -Wall -o "$OUT/fakewm" "$HERE/fakewm.c" "${XCB_FLAGS[@]}" || die "cannot build fakewm"

# The command prints its version, and exits 2 on wrong arguments, without picom.
CMD_VERSION=$("$ROOT/build/ws-direction" --version 2>&1)
"$ROOT/build/ws-direction" bogus >/dev/null 2>&1
bogus_status=$?
version_re='^ws-direction [0-9]+\.[0-9]+\.[0-9]+'
if [[ $CMD_VERSION =~ $version_re ]] && ((bogus_status == 2)); then
	pass "ws-direction --version prints $CMD_VERSION, wrong arguments exit 2"
else
	fail "ws-direction --version prints the version, wrong arguments exit 2" \
		"--version printed: $CMD_VERSION" "bogus exited $bogus_status"
fi
# Without picom, the command says so and fails.
no_plugin=$("$ROOT/build/ws-direction" overview open 2>&1)
no_plugin_status=$?
if ((no_plugin_status == 1)) && [[ $no_plugin == *"picom is not running with the ws-direction plugin"* ]]; then
	pass "without picom the command fails, and says why"
else
	fail "without picom the command fails, and says why" \
		"exit status $no_plugin_status, message: $no_plugin"
fi

if "$PICOM" --config "$CONFIG" --diagnostics >"$OUT/diagnostics.out" 2>&1 &&
	grep -q "Config file used: $CONFIG" "$OUT/diagnostics.out" &&
	! grep -qiE "error.*(config|script|parse|line)" "$OUT/diagnostics.out"; then
	pass "slide style loads (picom --diagnostics)"
else
	fail "slide style loads (picom --diagnostics)" "see $OUT/diagnostics.out"
fi
if grep -q "plugin API is not compatible" "$OUT/diagnostics.out"; then
	die "this picom does not have plugin API 0.9, see $OUT/diagnostics.out"
fi
"$PICOM" --config "$OUT/wave.conf" --diagnostics >"$OUT/diagnostics-wave.out" 2>&1
grep -qx "  - wave" "$OUT/diagnostics-wave.out" ||
	die "this picom has no custom animation triggers, see $OUT/diagnostics-wave.out"
# Every shipped style loads with the plugin and no animation-triggers line: the plugin
# declares the triggers the styles use.
bad_styles=
for style in "$ROOT"/styles/*.conf; do
	{
		echo "plugins = \"$PLUGIN\";"
		echo 'backend = "glx";'
		echo 'rules = ({'
		echo "  match = \"!override_redirect && window_type = 'normal'\";"
		cat "$style"
		echo '});'
	} >"$OUT/style-check.conf"
	"$PICOM" --config "$OUT/style-check.conf" --diagnostics >"$OUT/style-check.out" 2>&1
	if ! grep -q "Config file used: $OUT/style-check.conf" "$OUT/style-check.out" ||
		grep -qiE "error.*(config|script|parse|line|trigger)|fatal" "$OUT/style-check.out"; then
		bad_styles+=" $(basename "$style")"
		cp "$OUT/style-check.out" "$OUT/style-check-$(basename "$style" .conf).out"
	fi
done
if [[ -z $bad_styles ]]; then
	pass "every style loads without an animation-triggers line"
else
	fail "every style loads without an animation-triggers line" "failed:$bad_styles"
fi
# Only what this run installs, not files an earlier run left there.
rm -rf "$OUT/installed"
make -s -C "$ROOT" install PICOM_CONFIG_DIR="$OUT/installed/xdg/picom" \
	BINDIR="$OUT/installed/bin" MANDIR="$OUT/installed/man" >"$OUT/install.out" 2>&1
# make uninstall takes away what make install put there, the man page too.
installed=$(find "$OUT/installed/xdg" "$OUT/installed/bin" "$OUT/installed/man" -type f \
	2>/dev/null | wc -l)
man_page=missing
[[ -f $OUT/installed/man/man1/ws-direction.1 ]] && man_page=installed
make -s -C "$ROOT" uninstall PICOM_CONFIG_DIR="$OUT/installed/xdg/picom" \
	BINDIR="$OUT/installed/bin" MANDIR="$OUT/installed/man" >>"$OUT/install.out" 2>&1
left=$(find "$OUT/installed/xdg" "$OUT/installed/bin" "$OUT/installed/man" -type f 2>/dev/null)
if ((installed > 0)) && [[ $man_page == installed && -z $left ]]; then
	pass "make uninstall removes the $installed files make install put, man page too"
else
	mapfile -t left_files <<<"$left"
	fail "make uninstall removes the files make install put, man page too" \
		"installed $installed, man page $man_page, left:" "${left_files[@]}"
fi
# An older config that still lists the plugin's triggers loads too, each trigger once.
{
	echo 'animation-triggers = ["ws-pass", "ws-unfold", "ws-fold"];'
	cat "$OUT/pass.conf"
} >"$OUT/listed.conf"
"$PICOM" --config "$OUT/listed.conf" --diagnostics >"$OUT/diagnostics-listed.out" 2>&1
listed=$(for t in ws-pass ws-unfold ws-fold; do grep -cx "  - $t" "$OUT/diagnostics-listed.out"; done | tr '\n' ' ')
if [[ $listed == "1 1 1 " ]] && ! grep -qiE "error.*(config|script|parse|line|trigger)|fatal" "$OUT/diagnostics-listed.out"; then
	pass "a config that still lists the plugin's triggers loads, each trigger once"
else
	fail "a config that still lists the plugin's triggers loads, each trigger once" \
		"counts: $listed, see $OUT/diagnostics-listed.out"
fi

# --- phase 1: stock i3 ---

echo "== phase 1: stock i3"
cat >"$OUT/i3.conf" <<EOF
ipc-socket $SOCK
font pango:monospace 8
focus_follows_mouse no
mouse_warping none
workspace "1:IDE" output DPT
workspace "2:Web" output DPT
workspace "4:Misc" output DPT
workspace "7:Extra" output DPT
workspace "3:Term" output HDT
workspace "5:Music" output HDT
assign [instance="^ws1\$"] workspace "1:IDE"
assign [instance="^ws2\$"] workspace "2:Web"
assign [instance="^ws3\$"] workspace "3:Term"
assign [instance="^ws4\$"] workspace "4:Misc"
assign [instance="^ws5\$"] workspace "5:Music"
assign [instance="^ws7\$"] workspace "7:Extra"
assign [instance="^hang\$"] workspace "3:Term"
for_window [instance="^hang\$"] floating enable, resize set 888 px 400 px, move position 3677 px 100 px
assign [instance="^notes\$"] workspace "Notes"
assign [instance="^alpha\$"] workspace "Alpha"
for_window [instance="^scratch\$"] move scratchpad
EOF
run_bg "$OUT/i3.log" "$I3" -c "$OUT/i3.conf"
I3_PID=$LAST_PID
wait_until 10 i3-msg -s "$SOCK" -t get_version || die "i3 did not start, see $OUT/i3.log"
outputs=$(i3-msg -s "$SOCK" -t get_outputs)
[[ $outputs == *'"name":"DPT"'* && $outputs == *'"name":"HDT"'* ]] ||
	die "i3 does not see the DPT and HDT outputs: $outputs"

# The colors are for the screenshot checks below.
I3_XTERMS=()
for spec in ws1:ws1-a:ff0000 ws1:ws1-b:ff0000 ws2:ws2-a:0000ff ws3:ws3-a:00ff00 \
	ws4:ws4-a:ffffff ws5:ws5-a:ffff00 ws7:ws7-a:ffffff notes:notes-a:ffffff \
	alpha:alpha-a:ffffff scratch:scratch:ffffff; do
	IFS=: read -r inst title color <<<"$spec"
	color="#$color"
	start_xterm "$inst" "$title" -bg "$color"
	I3_XTERMS+=("$LAST_PID")
done
# HDT shows 5, DPT shows 1 and has the focus. HDT is never focused while picom runs
# before the first case, so picom must learn what HDT shows from the windows.
i3cmd 'workspace 5:Music'
i3cmd 'workspace 1:IDE'
sleep 0.5
echo "   desktops: $(xprop -root _NET_DESKTOP_NAMES | cut -d= -f2)"

start_picom "$I3_CONFIG" picom-i3

# The plugin's first line in WS_DIRECTION_LOG, which is picom's log here, is its version.
plugin_first=$(grep -m1 " ws_direction DEBUG \] " "$LOG" | sed -E 's/^\[[^]]*\] //')
if [[ $plugin_first == "$CMD_VERSION" ]]; then
	pass "the plugin's first log line is its version, the command's ($plugin_first)"
else
	fail "the plugin's first log line is its version, the command's" \
		"expected: $CMD_VERSION" "got:      $plugin_first"
fi

i3_case "HDT 5->3 while the focus is on DPT, first switch after picom started" \
	'workspace 3:Term' "ws5-a hide -1" "ws3-a show -1"
i3_case "focus moves to the other monitor: no animation" \
	'focus output DPT' none
i3_case "DPT 1->2" 'workspace 2:Web' "ws1-a hide 1" "ws1-b hide 1" "ws2-a show 1"
distances "  (distance 1)" "ws1-a hide 1" "ws1-b hide 1" "ws2-a show 1"
i3_case "DPT 2->1" 'workspace 1:IDE' "ws2-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "DPT 1->4 (no wrap-around guessing)" 'workspace 4:Misc' \
	"ws1-a hide 1" "ws1-b hide 1" "ws4-a show 1"
distances "  (distance 2: it passes 2, and 3 is on HDT)" \
	"ws1-a hide 2" "ws1-b hide 2" "ws4-a show 2"
variables "  (faces: 1 leaves from face 0, 4 arrives from face 2 of DPT's 6)" \
	"ws1-a hide direction 1 distance 2 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b hide direction 1 distance 2 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a show direction 1 distance 2 face 2/6 selected 1 placed 1 $DPT_RECT"
i3_case "DPT 4->1" 'workspace 1:IDE' "ws4-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "HDT 3->5" 'workspace 5:Music' "ws3-a hide 1" "ws5-a show 1"
distances "  (distance 1: 4 is on DPT)" "ws3-a hide 1" "ws5-a show 1"
i3_case "HDT 5->3" 'workspace 3:Term' "ws5-a hide -1" "ws3-a show -1"
i3_case "focus moves to the other monitor by switching to its visible workspace" \
	'workspace 1:IDE' none
i3_case "DPT 1->7" 'workspace 7:Extra' "ws1-a hide 1" "ws1-b hide 1" "ws7-a show 1"
distances "  (distance 3: it passes 2 and 4)" "ws1-a hide 3" "ws1-b hide 3" "ws7-a show 3"
variables "  (faces: 7 arrives from face 3)" \
	"ws1-a hide direction 1 distance 3 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b hide direction 1 distance 3 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a show direction 1 distance 3 face 3/6 selected 1 placed 1 $DPT_RECT"
i3_case "HDT 3->5 while the focus is on DPT showing 7" \
	'workspace 5:Music' "ws3-a hide 1" "ws5-a show 1"
variables "  (faces of HDT, which now has the focus)" \
	"ws3-a hide direction 1 distance 1 face 0/2 selected 1 placed 1 $HDT_RECT" \
	"ws5-a show direction 1 distance 1 face 1/2 selected 1 placed 1 $HDT_RECT"
i3_case "DPT 7->1 while the focus is on HDT" \
	'workspace 1:IDE' "ws7-a hide -1" "ws1-a show -1" "ws1-b show -1"
distances "  (distance 3)" "ws7-a hide 3" "ws1-a show 3" "ws1-b show 3"
variables "  (faces: 1 arrives from face -3)" \
	"ws7-a hide direction -1 distance 3 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-a show direction -1 distance 3 face -3/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b show direction -1 distance 3 face -3/6 selected 1 placed 1 $DPT_RECT"
i3_case "HDT 5->3 while the focus is on DPT showing 1" \
	'workspace 3:Term' "ws5-a hide -1" "ws3-a show -1"
i3_case "focus back to DPT: no animation" 'focus output DPT' none
i3_case "DPT 1->8, an empty workspace: the outgoing windows get the sign" \
	'workspace 8' "ws1-a hide 1" "ws1-b hide 1"
distances "  (distance 4: it passes 2, 4 and 7)" "ws1-a hide 4" "ws1-b hide 4"
i3_case "DPT 8->1, the empty workspace is removed" \
	'workspace 1:IDE' "ws1-a show -1" "ws1-b show -1"
distances "  (distance 4, also with 8 gone)" "ws1-a show 4" "ws1-b show 4"
i3_case "DPT 1->6, an empty workspace" 'workspace 6' "ws1-a hide 1" "ws1-b hide 1"
before=$(current_desktop)
i3_case "DPT 6->7: 6 is removed, so only _NET_DESKTOP_NAMES changes" \
	'workspace 7:Extra' "ws7-a show 1"
distances "  (distance 1)" "ws7-a show 1"
after=$(current_desktop)
if [[ -n $before && $before == "$after" ]]; then
	pass "  (_NET_CURRENT_DESKTOP really stayed $before in the case above)"
else
	fail "  (_NET_CURRENT_DESKTOP should have stayed the same)" "before $before, after $after"
fi
i3_case "DPT 7->1" 'workspace 1:IDE' "ws7-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "DPT 1->2" 'workspace 2:Web' "ws1-a hide 1" "ws1-b hide 1" "ws2-a show 1"
i3_case "DPT back_and_forth (2->1)" 'workspace back_and_forth' \
	"ws2-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "two switches in one i3 command, DPT 1->2->4" 'workspace 2:Web; workspace 4:Misc' \
	"ws1-a hide 1" "ws1-b hide 1" "ws4-a show 1"
distances "  (distance 2: picom only sees 1->4)" "ws1-a hide 2" "ws1-b hide 2" "ws4-a show 2"
i3_case "DPT 4->1" 'workspace 1:IDE' "ws4-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "move a window to another workspace: 0" \
	'[title="^ws1-b$"] move container to workspace 4:Misc' "ws1-b hide 0"
distances "  (distance 0)" "ws1-b hide 0"
i3_case "move it back to the visible workspace: 0" \
	'[title="^ws1-b$"] move container to workspace 1:IDE' "ws1-b show 0"
# i3 doesn't place the windows of a hidden workspace, so the window stays on DPT.
i3_case "move a window to a hidden workspace of the other monitor: 0" \
	'[title="^ws1-b$"] move container to workspace 5:Music' "ws1-b hide 0"
variables "  (HDT's monitor and faces, not selected, and not placed: it is still on DPT)" \
	"ws1-b hide direction 0 distance 0 face 1/2 selected 0 placed 0 $HDT_RECT"
i3_case "move it back: 0" '[title="^ws1-b$"] move container to workspace 1:IDE' "ws1-b show 0"
variables "  (placed on DPT again)" \
	"ws1-b show direction 0 distance 0 face 0/6 selected 1 placed 1 $DPT_RECT"
i3_case "scratchpad show: 0" '[instance="^scratch$"] scratchpad show' "scratch show 0"
i3_case "scratchpad hide: 0" '[instance="^scratch$"] scratchpad show' "scratch hide 0"
i3_case "fullscreen on hides the sibling: 0" '[title="^ws1-a$"] fullscreen enable' \
	"ws1-b hide 0"
i3_case "fullscreen off shows the sibling: 0" '[title="^ws1-a$"] fullscreen disable' \
	"ws1-b show 0"

mark=$(log_mark)
start_xterm newwin new-a
sleep "$SETTLE"
GOT=$(directions_since "$mark")
expect "a new window opens on the visible workspace: 0" "new-a show 0"
stop "$LAST_PID"
sleep "$SETTLE"
# A window that opens on the new workspace right after a switch is not part of it.
mark=$(log_mark)
i3cmd 'workspace 2:Web'
start_xterm ws2 new2-a
NEW2_PID=$LAST_PID
sleep "$SETTLE"
GOT=$(directions_since "$mark")
expect "a window that opens right after DPT 1->2 is not part of it: 0" \
	"ws1-a hide 1" "ws1-b hide 1" "ws2-a show 1" "new2-a show 0"
stop "$NEW2_PID"
sleep "$SETTLE"
i3_case "DPT 2->1" 'workspace 1:IDE' "ws2-a hide -1" "ws1-a show -1" "ws1-b show -1"

i3_case "DPT 1->Notes: names without a number sort after numbered ones" \
	'workspace Notes' "ws1-a hide 1" "ws1-b hide 1" "notes-a show 1"
distances "  (distance 4: it passes 2, 4 and 7)" "ws1-a hide 4" "ws1-b hide 4" "notes-a show 4"
# The new workspace 6 moves Notes and Alpha down in _NET_DESKTOP_NAMES, while DPT shows
# Notes.
i3_case "move a hidden window to the new workspace 6: no animation" \
	'[title="^ws1-b$"] move container to workspace 6' none
i3_case "DPT Notes->Alpha: Alpha comes later in _NET_DESKTOP_NAMES" \
	'workspace Alpha' "notes-a hide 1" "alpha-a show 1"
distances "  (distance 1: Notes moved in the list, and is not counted as in between)" \
	"notes-a hide 1" "alpha-a show 1"
i3_case "DPT Alpha->Notes" 'workspace Notes' "alpha-a hide -1" "notes-a show -1"
i3_case "DPT Notes->7" 'workspace 7:Extra' "notes-a hide -1" "ws7-a show -1"
i3_case "move it back, and 6 goes away: no animation" \
	'[title="^ws1-b$"] move container to workspace 1:IDE' none
i3_case "DPT 7->1" 'workspace 1:IDE' "ws7-a hide -1" "ws1-a show -1" "ws1-b show -1"

# fast_case NAME SIGN FIRST LAST GAP: switch DPT through FIRST and two more workspaces
# to LAST, GAP seconds apart, like scrolling on a bar. Which windows in between get to
# show depends on timing, but every window must get SIGN, the first workspace's windows
# must hide and the last one's must show.
fast_case() {
	local name=$1 sign=$2 first=$3 last=$4 gap=$5 mark bad
	shift 5
	mark=$(log_mark)
	for ws in "$@"; do
		i3cmd "workspace $ws"
		sleep "$gap"
	done
	sleep "$SETTLE"
	GOT=$(directions_since "$mark")
	bad=$(grep -v " $sign\$" <<<"$GOT")
	if [[ -z $bad && $GOT == *"$first hide $sign"* && $GOT == *"$last show $sign"* ]]; then
		pass "$name"
	else
		fail "$name" "got: $(tr '\n' ' ' <<<"$GOT")"
	fi
}
for gap in 0.005 0.03 0.06; do
	fast_case "fast switches DPT 1->2->4->7, $gap s apart: all 1" 1 ws1-a ws7-a "$gap" \
		2:Web 4:Misc 7:Extra
	fast_case "fast switches DPT 7->4->2->1, $gap s apart: all -1" -1 ws7-a ws1-a "$gap" \
		4:Misc 2:Web 1:IDE
done

# Screenshot checks: the slide style made slow (6 s) and linear, so a screenshot
# taken about 1.5 s into a switch shows both workspaces, 25% of the way.
# DPT: 1 is red, 2 is blue. HDT: 3 is green, 5 is yellow.
stop_picom
sed -E -e 's/cubic-bezier\([^)]*\)/linear/' -e 's/var-length = [0-9.]+;/var-length = 6;/' \
	-e 's/\+ [0-9.]+ \* \(1 - var-slide\)/+ 3 * (1 - var-slide)/' \
	"$I3_CONFIG" >"$OUT/slow.conf"
start_picom "$OUT/slow.conf" picom-slow
# A floating window on workspace 3 that hangs off the right edge of HDT, so it is not
# fully on any monitor. It must still be cropped to HDT when 3 slides left. It sits
# high up, so a bleed would land above DPT, where no window can cover it.
start_xterm hang hang-a -bg "#ff00ff"
HANG_PID=$LAST_PID
# Let the slow fade-in of the windows already on screen finish.
sleep 3.5

# color_share X Y W H COLOR OTHER: of the pixels in the rectangle that are COLOR or
# OTHER (hex like FF0000), the share that is COLOR, in percent.
color_share() {
	magick "$OUT/shot.png" -crop "$3x$4+$1+$2" +repage txt:- |
		awk -v a="#$5" -v b="#$6" '$3 == a { na++ } $3 == b { nb++ }
			END { if (na + nb == 0) print -1; else printf "%d\n", 100 * na / (na + nb) }'
}
pixel() { magick "$OUT/shot.png" -crop "1x1+$1+$2" +repage txt:- | awk 'NR == 2 { print $3 }'; }
# color_part X Y W H COLOR: the share of the pixels in the rectangle that are COLOR, in
# percent.
color_part() {
	magick "$OUT/shot.png" -crop "$3x$4+$1+$2" +repage txt:- |
		awk -v a="#$5" 'NR > 1 { n++ } $3 == a { na++ }
			END { if (n == 0) print -1; else printf "%d\n", 100 * na / n }'
}

# shot_case NAME I3-COMMAND X Y W H COLOR OTHER MIN MAX: switch, take a screenshot after
# 1.5 s, and check that COLOR's share of the strip is between MIN and MAX percent.
shot_case() {
	local name=$1 cmd=$2 share
	i3cmd "$cmd"
	sleep 1.5
	import -window root "$OUT/shot.png"
	SHOTS=$((SHOTS + 1))
	cp "$OUT/shot.png" "$OUT/shot-$SHOTS.png"
	share=$(color_share "$3" "$4" "$5" "$6" "$7" "$8")
	if ((share >= $9 && share <= ${10})); then
		pass "$name (#$7 share $share%)"
	else
		fail "$name" "#$7 share of the strip is $share%, expected $9..${10}%"
	fi
}

# Pixels just outside the monitor that slides: above DPT there is no monitor, so they
# must stay dark; left of HDT is DPT, which must keep showing its own red workspace.
SHOTS=0
crop=
shot_case "screenshot: DPT 1->2 slides up, 2 comes from below" 'workspace 2:Web' \
	1000 480 1 1440 FF0000 0000FF 55 95
crop+=" above-DPT:$(pixel 1000 300)"
sleep 5
shot_case "screenshot: DPT 2->1 slides down, 1 comes from above" 'workspace 1:IDE' \
	1000 480 1 1440 FF0000 0000FF 5 45
crop+=" above-DPT:$(pixel 1000 300)"
sleep 5
shot_case "screenshot: HDT 3->5 slides left, 5 comes from the right" 'workspace 5:Music' \
	3440 1000 1080 1 00FF00 FFFF00 55 95
crop+=" left-of-HDT:$(pixel 3300 1000) above-DPT:$(pixel 3300 200)"
hang_on_dpt=$(color_share 0 200 3440 1 FF00FF 000000)
if ((hang_on_dpt == 0)); then
	pass "screenshot: a floating window hanging off HDT stays cropped to HDT"
else
	fail "screenshot: a floating window hanging off HDT stays cropped to HDT" \
		"#FF00FF covers $hang_on_dpt% of the strip above DPT"
fi
sleep 5
shot_case "screenshot: HDT 5->3 slides right, 3 comes from the left" 'workspace 3:Term' \
	3440 1000 1080 1 00FF00 FFFF00 5 45
crop+=" left-of-HDT:$(pixel 3300 1000) above-DPT:$(pixel 3300 200)"
sleep 5
crop_ok=1
for item in $crop; do
	case $item in
	above-DPT:#FF0000 | above-DPT:#00FF00) crop_ok=0 ;;
	left-of-HDT:#FF0000) ;;
	left-of-HDT:*) crop_ok=0 ;;
	esac
done
if ((crop_ok)); then
	pass "screenshot: slides stay on their own monitor (crop)"
else
	fail "screenshot: slides stay on their own monitor (crop)" "pixels:$crop"
fi
stop "$HANG_PID"
i3cmd 'focus output DPT'

stop_picom

# start_wave WINDOW...: start the custom trigger "wave" on these windows over D-Bus, and
# put picom's reply in $reply.
start_wave() {
	local ids
	ids=$(IFS=,; echo "$*")
	reply=$(dbus-send --print-reply --dest="com.github.chjj.compton.${DISP//[^[:alnum:]]/_}" \
		/com/github/chjj/compton picom.Compositor.StartAnimations string:wave \
		"array:uint32:$ids" 2>&1)
}

# A custom trigger started over D-Bus draws an xterm of a hidden workspace. ws2-a (blue)
# is shown once under this picom, so it has an image to keep, and so is ws4-a. Then DPT
# shows the empty workspace 8, so nothing else is drawn where ws2-a would be.
start_picom "$OUT/wave.conf" picom-wave
i3cmd 'workspace 4:Misc'
sleep "$SETTLE"
i3cmd 'workspace 2:Web'
sleep "$SETTLE"
mark=$(log_mark)
i3cmd 'workspace 8'
sleep "$SETTLE"
WS2=$(win_id ws2-a)
kept=$(tail -n +"$((mark + 1))" "$LOG" | grep -c "Keeping the image of window .* (ws2-a)")
import -window root "$OUT/shot.png"
before=$(pixel 1720 1200)
mark=$(log_mark)
start_wave "$WS2"
sleep 0.7
import -window root "$OUT/shot.png"
cp "$OUT/shot.png" "$OUT/shot-wave.png"
during=$(pixel 1720 1200)
sleep 2
import -window root "$OUT/shot.png"
after=$(pixel 1720 1200)
asked=$(tail -n +"$((mark + 1))" "$LOG" |
	grep -c "wave of window 0x[0-9a-f]* (ws2-a): direction = 0 ")
if ((kept == 1)) && [[ $reply == *"uint32 1"* && $before != "#0000FF" &&
	$during == "#0000FF" && $after != "#0000FF" ]]; then
	pass "screenshot: StartAnimations draws a hidden xterm only while its animation runs"
else
	fail "screenshot: StartAnimations draws a hidden xterm only while its animation runs" \
		"kept $kept, reply: ${reply//$'\n'/ }" \
		"pixel before $before, during $during, after $after"
fi
if ((asked == 1)); then
	pass "  (the plugin is asked once for the custom trigger, by its name)"
else
	fail "  (the plugin is asked once for the custom trigger, by its name)" \
		"$asked decision lines for wave of ws2-a, see $LOG"
fi
GOT=$(variables_since "$mark" wave)
expect "  (not part of a switch: face -3 from 8, which DPT shows, of its 7)" \
	"ws2-a wave direction 0 distance 0 face -3/7 selected 1 placed 1 $DPT_RECT"
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
# Started right after a switch, on the xterms of the workspaces it passes: the plugin
# didn't start them, so they are part of no switch, and face counts from 7.
mark=$(log_mark)
i3cmd 'workspace 7:Extra'
start_wave "$WS2" "$(win_id ws4-a)"
sleep "$SETTLE"
GOT=$(variables_since "$mark" wave)
expect "StartAnimations right after DPT 1->7: part of no switch, the plugin didn't start it" \
	"ws2-a wave direction 0 distance 0 face -2/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a wave direction 0 distance 0 face -1/6 selected 1 placed 1 $DPT_RECT"
sleep 2
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
stop_picom

# The plugin starts the custom trigger ws-pass from its frame callback, on all xterms of
# the switch's monitor, without StartAnimations. Every workspace with an xterm is shown
# once under this picom, so the xterms have images to keep, and all of them get ws-pass.
# DPT: 1 is red, 2 is blue, 4 and 7 are white. HDT: 3 is green, 5 is yellow.
start_picom "$OUT/pass.conf" picom-pass
for ws in 2:Web 4:Misc 7:Extra 5:Music 3:Term 1:IDE; do
	i3cmd "workspace $ws"
	sleep "$SETTLE"
done
sleep "$PASS_LENGTH"
import -window root "$OUT/shot.png"
before=$(color_part 1000 480 1 1440 0000FF)
mark=$(log_mark)
i3cmd 'workspace 4:Misc'
# Halfway through, 1 has gone up by one monitor height, and 2 is where 1 was.
sleep "$(awk -v l="$PASS_LENGTH" 'BEGIN { print l / 2 }')"
import -window root "$OUT/shot.png"
cp "$OUT/shot.png" "$OUT/shot-pass.png"
during=$(color_part 1000 480 1 1440 0000FF)
sleep "$PASS_LENGTH"
import -window root "$OUT/shot.png"
after=$(color_part 1000 480 1 1440 0000FF)
if ((before == 0 && during >= 50 && after == 0)); then
	pass "screenshot: DPT 1->4 draws 2, which it passes, halfway (#0000FF $before% $during% $after%)"
else
	fail "screenshot: DPT 1->4 draws 2, which it passes, halfway" \
		"#0000FF share of a DPT strip: before $before%, halfway $during% (at least 50), after $after%"
fi
GOT=$(variables_since "$mark" ws-pass)
expect "  (ws-pass on 1 from face 0, on 2 from face 1, on 4 from face 2, and on 7 too)" \
	"ws1-a ws-pass direction 1 distance 2 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction 1 distance 2 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction 1 distance 2 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction 1 distance 2 face 2/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a ws-pass direction 1 distance 2 face 3/6 selected 1 placed 1 $DPT_RECT"
starts=$(tail -n +"$((mark + 1))" "$LOG" |
	sed -nE 's/.*Starting animation ([a-z-]+) for window 0x[0-9a-f]+ \((ws1-a|ws1-b|ws2-a|ws4-a)\)$/\2 \1/p' |
	sort | tr '\n' ' ')
if [[ $starts == "ws1-a ws-pass ws1-b ws-pass ws2-a ws-pass ws4-a ws-pass " ]]; then
	pass "  (picom started ws-pass once on each, and no show or hide)"
else
	fail "  (picom started ws-pass once on each, and no show or hide)" "starts: $starts"
fi
pass_case "  (all in the pass of the root window change)" "$mark" '"1:IDE" -> "4:Misc"' \
	ws1-a ws1-b ws2-a ws4-a

# pass_switch NAME I3-COMMAND EXPECTATION...: switch, and check that the xterms of the
# monitor got ws-pass with these variables, and that none of them started a show or hide.
pass_switch() {
	local name=$1 cmd=$2
	shift 2
	CASE_MARK=$(log_mark)
	i3cmd "$cmd"
	sleep "$SETTLE"
	GOT=$(variables_since "$CASE_MARK" ws-pass)
	expect "$name" "$@"
	GOT=$(directions_since "$CASE_MARK")
	expect "  (no show or hide)" none
}
pass_switch "DPT 4->1" 'workspace 1:IDE' \
	"ws7-a ws-pass direction -1 distance 2 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction -1 distance 2 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction -1 distance 2 face -1/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-a ws-pass direction -1 distance 2 face -2/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction -1 distance 2 face -2/6 selected 1 placed 1 $DPT_RECT"
CASE_MARK=$(log_mark)
i3cmd 'workspace 7:Extra'
sleep "$SETTLE"
GOT=$(directions_since "$CASE_MARK")
expect "DPT 1->7: no show or hide" none
GOT=$(variables_since "$CASE_MARK" ws-pass)
expect "  (ws-pass on 1, 2, 4 and 7, faces 0 to 3)" \
	"ws1-a ws-pass direction 1 distance 3 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction 1 distance 3 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction 1 distance 3 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction 1 distance 3 face 2/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a ws-pass direction 1 distance 3 face 3/6 selected 1 placed 1 $DPT_RECT"
pass_case "  (all in the pass of the root window change)" "$CASE_MARK" '"1:IDE" -> "7:Extra"' \
	ws1-a ws1-b ws2-a ws4-a ws7-a
pass_switch "DPT 7->1" 'workspace 1:IDE' \
	"ws7-a ws-pass direction -1 distance 3 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction -1 distance 3 face -1/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction -1 distance 3 face -2/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-a ws-pass direction -1 distance 3 face -3/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction -1 distance 3 face -3/6 selected 1 placed 1 $DPT_RECT"
sleep "$(pass_time 3 1.1)"
pass_switch "DPT 1->2, which passes nothing: ws-pass on all of DPT's xterms" 'workspace 2:Web' \
	"ws1-a ws-pass direction 1 distance 1 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction 1 distance 1 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction 1 distance 1 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction 1 distance 1 face 2/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a ws-pass direction 1 distance 1 face 3/6 selected 1 placed 1 $DPT_RECT"
pass_switch "HDT 3->5 while DPT shows 2: only HDT's xterms" 'workspace 5:Music' \
	"ws3-a ws-pass direction 1 distance 1 face 0/2 selected 1 placed 1 $HDT_RECT" \
	"ws5-a ws-pass direction 1 distance 1 face 1/2 selected 1 placed 1 $HDT_RECT"
# One command switches HDT back to 3 and goes on to DPT, so _NET_CURRENT_DESKTOP ends on
# DPT, and HDT's xterms are still in the ws-pass of 3->5, which suppresses their show and
# hide. The plugin sees HDT's switch from the map of 3's xterm, and starts ws-pass for
# it. Moving the focus to HDT then starts nothing.
mark=$(log_mark)
i3cmd 'workspace 3:Term; workspace 2:Web'
sleep "$SETTLE"
seen=$(tail -n +"$((mark + 1))" "$LOG" |
	grep -cE 'Workspace switch on monitor at 3440,0: "5:Music" -> "3:Term".*from a map')
passes=$(tail -n +"$((mark + 1))" "$LOG" | grep -c 'Switch "5:Music" -> "3:Term" has 2 windows, started ws-pass on 2')
mark=$(log_mark)
i3cmd 'focus output HDT'
sleep "$SETTLE"
after=$(tail -n +"$((mark + 1))" "$LOG" | grep -cE 'Workspace switch|started ws-pass')
if ((seen == 1 && passes == 1 && after == 0)); then
	pass "HDT 5->3 in a command that goes back to DPT: seen from the map, and focusing HDT then starts nothing"
else
	fail "HDT 5->3 in a command that goes back to DPT: seen from the map, and focusing HDT then starts nothing" \
		"switch lines: $seen, ws-pass lines: $passes (1 each), lines after the focus move: $after (0); see $LOG"
fi
i3cmd 'workspace 1:IDE'
sleep "$(pass_time 1 1.1)"

# A window that moves to the new workspace with the switch stays on screen: it gets no
# ws-pass, and no show or hide either.
pass_switch "DPT 1->2 with ws1-b moved along: no ws-pass for it" \
	'[title="^ws1-b$"] move container to workspace 2:Web; workspace 2:Web' \
	"ws1-a ws-pass direction 1 distance 1 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction 1 distance 1 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction 1 distance 1 face 2/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a ws-pass direction 1 distance 1 face 3/6 selected 1 placed 1 $DPT_RECT"
sleep "$(pass_time 1 1.1)"
pass_switch "DPT 2->1 with ws1-b moved back along" \
	'[title="^ws1-b$"] move container to workspace 1:IDE; workspace 1:IDE' \
	"ws2-a ws-pass direction -1 distance 1 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-a ws-pass direction -1 distance 1 face -1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction -1 distance 1 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a ws-pass direction -1 distance 1 face 2/6 selected 1 placed 1 $DPT_RECT"
sleep "$(pass_time 1 1.1)"
sleep "$PASS_LENGTH"
stop_picom

# The plugin is loaded here too. The hook calls a provider only when an
# animation starts whose script reads its variable, and no script here reads one. So
# animations must start, and the plugin must never be asked: no decision line anywhere
# in this log.
{ echo "plugins = \"$PLUGIN\";"; cat "$PLAIN_CONFIG"; } >"$OUT/plain-plugin.conf"
PLAIN=$OUT/plain-plugin.conf
start_picom "$PLAIN" picom-plain
mark=$(log_mark)
i3cmd 'workspace 2:Web'
sleep "$SETTLE"
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
# For "never asked" to mean anything: the plugin is in this picom, it wrote its
# startup lines into this log (so a decision line would land here too), and it
# added its variables without a complaint. Complaints go to stderr: plugins load
# while the config is parsed, before picom opens its log file.
starts=$(grep -c "Starting animation" <(tail -n +"$((mark + 1))" "$LOG"))
decisions=$(grep -c ": direction = " "$LOG")
if grep -qF "$PLUGIN" "/proc/$PICOM_PID/maps" &&
	grep -q 'Workspace ".*" is shown' "$LOG" &&
	! grep -qE "ws-direction:|Cannot add animation variable" "$OUT/picom-plain.out" &&
	((starts > 0 && decisions == 0)); then
	pass "config without plugin-ws-direction-direction: animations run, plugin is never asked ($starts starts, $decisions decisions)"
else
	fail "config without plugin-ws-direction-direction: animations run, plugin is never asked" \
		"$starts starts, $decisions decisions; see $LOG and $OUT/picom-plain.out"
fi
stop_picom

# Without an "open" animation picom doesn't look at the windows already on screen when
# it starts, so here only the seed (from mapped windows) can know that HDT shows 3.
# DPT shows 7 and has the focus: 7->5 would be -1, but on HDT it is 3->5, +1.
sed 's/triggers = \["show", "open"\];/triggers = ["show"];/' "$I3_CONFIG" >"$OUT/no-open.conf"
i3cmd 'workspace 7:Extra'
sleep "$SETTLE"
start_picom "$OUT/no-open.conf" picom-no-open
i3_case "seed: first switch is HDT 3->5 while DPT shows 7, no open animations" \
	'workspace 5:Music' "ws3-a hide 1"
stop_picom

stop "${I3_XTERMS[@]}"
stop "$I3_PID"
sleep 0.5

# --- phase 2: fake window manager ---

echo "== phase 2: fake window manager"
# Desktops 1:A and 2:B are on DPT, 3:C and 4:D on HDT. fs is on all desktops. fu is on 2:B,
# but the window manager doesn't list it in _NET_CLIENT_LIST.
for spec in fa:+100+600 fb:+100+1000 fb2:+800+1000 fc:+3600+100 fc2:+3600+500 \
	fd:+3600+900 fs:+1500+600 fu:+800+600; do
	start_xterm fake "${spec%%:*}" -geometry "40x10${spec#*:}"
done
FA=$(win_id fa) FB=$(win_id fb) FB2=$(win_id fb2) FC=$(win_id fc) FC2=$(win_id fc2)
FD=$(win_id fd) FS=$(win_id fs) FU=$(win_id fu)
# DPT shows 1:A (fa, and the sticky fs), HDT shows 3:C (fc).
# fb2 is not managed yet: it opens in the first case.
fakewm names 1:A,2:B,3:C,4:D viewport 0,480,0,480,3440,0,3440,0 current 0 \
	clients "$FA:0,$FB:1,$FC:2,$FC2:2,$FD:3,$FS:sticky" desktop "$FB2" 1 desktop "$FU" 1 \
	unmap "$FB" unmap "$FB2" unmap "$FC2" unmap "$FD" unmap "$FU"

start_picom "$CONFIG" picom-fake
mark=$(log_mark)
fake_case "DPT 1:A->2:B as root change, map, and an unmap 360 ms later: no clock, the late hide is part of it" \
	"current 1 sleep 40 map $FB sleep 20 clients $FA:0,$FB:1,$FB2:1,$FC:2,$FC2:2,$FD:3,$FS:sticky map $FB2 sleep 300 unmap $FA" \
	"fb show 1" "fa hide 1" "fb2 show 0"
GOT=$(directions_since "$mark" | grep fb2)
expect "  (a new window that opens 60 ms after the switch is not part of it)" "fb2 show 0"
t_switch=$(log_time_ms "$mark" 'Workspace switch on monitor')
# The decision lines, not picom's own lines about these windows.
t_show=$(log_time_ms "$mark" 'of window 0x[0-9a-f]+ \(fb\): direction = ')
t_hide=$(log_time_ms "$mark" 'of window 0x[0-9a-f]+ \(fa\): direction = ')
if [[ -n $t_switch && -n $t_show && -n $t_hide ]] && ((t_hide - t_show >= 20)); then
	pass "  (those steps were handled in different frames: +$((t_show - t_switch)) ms, +$((t_hide - t_show)) ms)"
else
	fail "  (those steps should be handled in different frames)" \
		"switch $t_switch, show $t_show, hide $t_hide (ms)"
fi
fake_case "DPT 2:B->1:A while a window on HDT (no switch there) shows: it gets 0" \
	"current 0 sleep 30 map $FA sleep 30 unmap $FB unmap $FB2 map $FC2" \
	"fa show -1" "fb hide -1" "fb2 hide -1" "fc2 show 0"
variables "  (faces; the window on HDT is not selected)" \
	"fa show direction -1 distance 1 face -1/2 selected 1 placed 1 $DPT_RECT" \
	"fb hide direction -1 distance 1 face 0/2 selected 1 placed 1 $DPT_RECT" \
	"fb2 hide direction -1 distance 1 face 0/2 selected 1 placed 1 $DPT_RECT" \
	"fc2 show direction 0 distance 0 face 0/2 selected 0 placed 1 $HDT_RECT"
fake_case "a sticky window that hides during a switch gets 0" \
	"current 1 map $FB unmap $FA unmap $FS" "fb show 1" "fa hide 1" "fs hide 0"
variables "  (the sticky window is on no workspace: face 0, the monitor it is on)" \
	"fb show direction 1 distance 1 face 1/2 selected 1 placed 1 $DPT_RECT" \
	"fa hide direction 1 distance 1 face 0/2 selected 1 placed 1 $DPT_RECT" \
	"fs hide direction 0 distance 0 face 0/2 selected 1 placed 1 $DPT_RECT"
fake_case "DPT 2:B->1:A with the map 40 ms before the root change, and the unmap 40 ms after" \
	"map $FA sleep 40 current 0 sleep 40 unmap $FB" "fa show -1" "fb hide -1"
fakewm hidden "$FB2" 1
fake_case "a hidden tab and a window the window manager doesn't list show during a switch: 0" \
	"current 1 map $FB map $FB2 map $FU unmap $FA" "fb show 1" "fb2 show 0" "fu show 0" \
	"fa hide 1"
variables "  (they are on no workspace: face 0, the monitor they are on)" \
	"fb show direction 1 distance 1 face 1/2 selected 1 placed 1 $DPT_RECT" \
	"fb2 show direction 0 distance 0 face 0/2 selected 1 placed 1 $DPT_RECT" \
	"fu show direction 0 distance 0 face 0/2 selected 1 placed 1 $DPT_RECT" \
	"fa hide direction 1 distance 1 face 0/2 selected 1 placed 1 $DPT_RECT"
fakewm hidden "$FB2" 0
fake_case "DPT 2:B->1:A, the tab is no longer hidden" \
	"current 0 map $FA unmap $FB unmap $FB2 unmap $FU" "fa show -1" "fb hide -1" \
	"fb2 hide -1" "fu hide 0"
fake_case "HDT 3:C->4:D" \
	"current 3 map $FD unmap $FC unmap $FC2" "fd show 1" "fc hide 1" "fc2 hide 1"
stop_picom

# Without _NET_DESKTOP_VIEWPORT every desktop spans all monitors, so a switch from 1:A
# (shown on DPT) to 3:C (shown on HDT) slides both.
fakewm noviewport current 0 map "$FA" unmap "$FB" unmap "$FC" unmap "$FC2" unmap "$FD" \
	unmap "$FS"
start_picom "$CONFIG" picom-noviewport
fake_case "no _NET_DESKTOP_VIEWPORT: every desktop spans all monitors, 1:A->3:C" \
	"current 2 sleep 30 map $FC sleep 30 unmap $FA" "fc show 1" "fa hide 1"
distances "  (distance 2: without viewports 2:B counts too)" "fc show 2" "fa hide 2"
variables "  (the whole screen, with all 4 faces)" \
	"fc show direction 1 distance 2 face 2/4 selected 1 placed 1 monitor 0,0 4520x1920" \
	"fa hide direction 1 distance 2 face 0/4 selected 1 placed 1 monitor 0,0 4520x1920"
stop_picom

# The X server can be busy with a switch, and slow to answer the plugin. The plugin then
# doesn't hold picom up: it pauses the screen until the answers are in. fakewm grabs the
# server, so it answers no one else, makes the switch 1:A->2:B, and lets go 80 ms later.
# paused_read MARK: how long the plugin's read after line MARK took with the screen
# paused, in us (empty if it didn't pause).
paused_read() {
	tail -n +"$(($1 + 1))" "$LOG" |
		sed -nE 's/.*Read the root window and [0-9]+ windows in ([0-9]+) us, the screen paused$/\1/p' |
		head -n 1
}
# plugin_step_max LOG: the longest the plugins' frame callbacks took in a frame, in us,
# from the P records of picom's frame timing log.
plugin_step_max() {
	awk -F, '$1 == "P" { d = $4 - $3; if (d > m) m = d } END { print m + 0 }' "$1"
}
# slow_read_case NAME FRAMES MARK: the plugin paused for the read, and no frame callback
# took long.
slow_read_case() {
	local waited longest
	waited=$(paused_read "$3")
	longest=$(plugin_step_max "$2")
	if [[ -n $waited ]] && ((waited >= 60000 && longest < 3000)); then
		pass "$1 (paused $((waited / 1000)) ms; the longest frame callback took $longest us)"
	else
		fail "$1" "read with the screen paused: ${waited:-none} us, longest frame callback: $longest us; see $LOG"
	fi
}
fakewm viewport 0,480,0,480,3440,0,3440,0 current 0 map "$FA" unmap "$FB" unmap "$FB2" \
	unmap "$FS" map "$FC"
# pass-slide: the plugin starts ws-pass; 2:B shows once first, so picom keeps fb's image.
rm -f "$OUT/frames-slow.csv"
PICOM_DEBUG="frame_timing=$OUT/frames-slow.csv;frame_layout=1" start_picom "$OUT/pass.conf" picom-slow
fakewm current 1 map "$FB" unmap "$FA"
sleep "$(pass_time 1 1.2)"
fakewm current 0 map "$FA" unmap "$FB"
sleep "$(pass_time 1 1.2)"
mark=$(log_mark)
fakewm grab current 1 sleep 20 map "$FB" unmap "$FA" sleep 80 ungrab
sleep "$(pass_time 1 1.2)"
slow_read_case "a slow X server: the plugin pauses the screen for its read, not holding up picom" \
	"$OUT/frames-slow.csv" "$mark"
pass_case "  (then it starts the windows of the switch together)" "$mark" '"1:A" -> "2:B"' fa fb
# The root window changes again while the read waits: the plugin reads again, and starts
# the switch once, from the new replies.
mark=$(log_mark)
fakewm grab current 0 sleep 20 map "$FA" unmap "$FB" sleep 20 \
	clients "$FA:0,$FB:1,$FB2:1,$FC:2,$FC2:2,$FD:3,$FS:sticky" sleep 60 ungrab
sleep "$(pass_time 1 1.2)"
pass_case "a slow X server, the root window changing again during the read: one start of the switch" \
	"$mark" '"2:B" -> "1:A"' fa fb
stop_picom
# slide animates a switch with show and hide, which picom starts itself: after the pause,
# with the plugin's variables.
rm -f "$OUT/frames-slow-slide.csv"
PICOM_DEBUG="frame_timing=$OUT/frames-slow-slide.csv;frame_layout=1" start_picom "$CONFIG" picom-slow-slide
mark=$(log_mark)
fakewm grab current 1 sleep 20 map "$FB" unmap "$FA" sleep 80 ungrab
sleep "$SETTLE"
slow_read_case "a slow X server with slide: the plugin pauses the screen for its read" \
	"$OUT/frames-slow-slide.csv" "$mark"
GOT=$(directions_since "$mark")
expect "  (and the show and hide still start, with the switch's direction)" "fb show 1" "fa hide 1"
fakewm current 0 map "$FA" unmap "$FB"
sleep "$SETTLE"
stop_picom

# Errors from before picom opens its log file (config, plugins, script compiling) are in
# the .out files, and so are sanitizer reports.
errors=$(grep -h -E "ERROR|FATAL|runtime error|Sanitizer" "$OUT"/picom-*.log "$OUT"/picom-*.out |
	grep -v "Failed to enable vsync")
if [[ -z $errors ]]; then
	pass "no errors in the picom logs (the vsync error is expected in Xvfb)"
else
	fail "no errors in the picom logs" "$errors"
fi

# The plugin declares ws-pass itself, so no config lists it, but slide has no animation for
# it. The plugin starts it on every switch, picom ignores those starts, and the shows and
# hides run as usual (checked by the cases of phase 1 and 2).
tried=$(grep -cE "windows, started ws-pass on [1-9][0-9]* \(in pass" "$OUT/picom-i3.log")
ignored=$(grep -c "Not starting animation ws-pass .* because it has no animation for it" \
	"$OUT/picom-i3.log")
started=$(grep -c "Starting animation ws-pass" "$OUT/picom-i3.log")
if ((tried > 0 && ignored > 0 && started == 0)); then
	pass "the plugin declares ws-pass, slide has no animation for it: picom ignores the starts ($tried switches, $ignored windows)"
else
	fail "the plugin declares ws-pass, slide has no animation for it: picom ignores the starts" \
		"$tried switches with starts, $ignored starts ignored, $started ws-pass animations started"
fi
tried=$(grep -cE "windows, started ws-pass on [1-9][0-9]* \(in pass" "$OUT/picom-fake.log")
started=$(grep -c "Starting animation ws-pass" "$OUT/picom-fake.log")
if ((tried > 0 && started == 0)); then
	pass "  (the same with fakewm: $tried switches with starts, none animated)"
else
	fail "  (the same with fakewm)" "$tried switches with starts, $started ws-pass animations started"
fi

# Every script in the other configs reads the variables, so picom must have asked
# the plugin at every animation start, and never otherwise. The plugin works out
# all of its variables at the first call for a window in a frame: one decision line
# per start.
counts=
bad=0
for log in "$OUT"/picom-*.log; do
	[[ $log != "$OUT/picom-plain.log" ]] || continue
	starts=$(grep -c "Starting animation" "$log")
	decisions=$(grep -c ": direction = " "$log")
	counts+=" $(basename "$log" .log):$decisions/$starts"
	((starts > 0 && decisions == starts)) || bad=1
done
if ((!bad)); then
	pass "plugin asked exactly once per animation start (decisions/starts:$counts)"
else
	fail "plugin asked exactly once per animation start" "decisions/starts:$counts"
fi

echo "== $PASSES passed, $FAILS failed (logs in $OUT)"
((FAILS == 0))
