#!/usr/bin/env bash
# Test harness for the ws-direction picom plugin, and so for the picom plugin API it builds
# on (animation variables, frame callbacks, start_animation, window_has_image,
# add_animation_trigger, pause_screen, add_wakeup_fd, window_at and window_keeps_image:
# plugin API 0.9).
#
# Setup: a nested Xvfb with two monitors (DPT 3440x1440 at +0+480, HDT 1080x1920 at
# +3440+0). It checks that the command prints its version and exits 2 on wrong
# arguments. Before any window manager runs, it checks with picom --diagnostics that
# every style in styles/ loads without an animation-triggers line, and that cube and flip
# load from where make install puts them; and that make uninstall removes what make
# install put in place, the man page too.
#
# Phase 1: stock i3, xterms on several workspaces, and picom with the plugin,
# tests/showhide.conf (a slide made of show and hide only) and debug logging. It checks that the plugin's first log line is its
# version, the command's. It runs i3 commands through i3-msg and checks
# the direction (and for some switches the distance and the other variables) that the
# plugin logs for every window that shows, opens or hides. It also takes screenshots in
# the middle of slowed-down slides, to check that it slides the right way and
# stays on its monitor. Then it runs picom with
# tests/picom-anim.conf, whose scripts don't read the variables, and checks the plugin
# is loaded but never asked (the hook calls a provider only for scripts that read its
# variable), and with a config without "open" animations to check the startup seed.
#
# Phase 1 also starts a custom animation trigger over picom's D-Bus interface
# (StartAnimations) on an xterm of a hidden workspace, and checks a screenshot shows it
# while the animation runs, and that started right after a switch on the xterms of the
# workspaces in between, it is still part of no switch: the plugin didn't start it. Then
# it runs picom with styles/pass-slide.conf, slowed down, and checks that the plugin
# itself starts the custom trigger ws-pass on all xterms of the switch's monitor, in the
# pass of the switch, and that none of them starts a show or hide on top. Screenshots
# partway through switches show the xterms of the workspaces passed on the monitor, in
# order, but not an xterm whose place is still on the other monitor (placed 0), and that a
# switch made while one runs carries the workspaces on as one strip. With styles/wall.conf
# next to pass-slide, "ws-direction overview" opens and closes the overview, a switch
# while it is open moves the bright tile, and a click on a window picks it ("open --wait"
# returns 0), while a click on no window does nothing ("open --wait" returns 1 on close).
# Two toggles at once are both done, --output and --all open the monitors they name, and
# "open --wait --buttons" returns for the other buttons. With styles/cube.conf and
# styles/flip.conf, slowed down, a screenshot partway through a switch shows the
# workspaces in perspective, on their monitor, and not the back of the flip's card, and
# check-frames.py checks every frame of those switches in picom's frame timing log. With
# styles/unfold.conf next to cube, the overview opens the box into a flat strip and closes
# it again, and check-frames.py checks those frames too. Then every style runs as it
# ships, at its own length, through switches that interrupt each other the same way and
# the other way, on both monitors (and with wall and unfold the overview opened halfway
# into a switch), and check-frames.py checks every frame. Last it restarts i3 and changes
# picom's config, which leave picom without images of the hidden workspaces, and checks
# that the plugin gets them back by itself, with the screen, the keyboard and the pointer
# frozen, that it puts the pointer back, and that it waits for a window that paints itself
# late (tests/slowpaint.c). And that "ws-direction warm" gets them back when asked, also
# at picom's start, and "warm --auto off" stops the plugin from doing it by itself.
# The harness runs on a D-Bus session bus of its own.
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
# 0.9, custom animation triggers and PICOM_DEBUG=frame_layout), I3 (default /usr/bin/i3),
# WSDIR_DISPLAY (default :91), WSDIR_WORK (directory for the i3 socket and logs, default
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
STYLE=$HERE/showhide.conf
PASS_STYLE=$ROOT/styles/pass-slide.conf
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
	dbus-send jq python3; do
	command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ ! -e /tmp/.X${DISP#:}-lock ]] || die "display $DISP is already in use"
mkdir -p "$OUT" || die "cannot create $OUT"
rm -f "$OUT"/*.log "$OUT"/*.out "$SOCK"
rm -rf "$WORK/i3"
[[ -f $PLUGIN ]] || die "plugin not found: $PLUGIN (run make)"
# picom loads a plugin by its full path, and /proc/<pid>/maps shows that path.
PLUGIN=$(realpath "$PLUGIN")
# The show and hide test style in a rule for normal windows. Copied in, not included, so the checks
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
# Phase 1 uses the same. The plugin declares ws-pass, but showhide.conf has no animation
# for it, so picom ignores the plugin's starts, and every show and hide runs as it would
# without them.
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
# The pass-slide style, which also draws the workspaces a switch passes (ws-pass), made
# linear and slow: a switch to the next workspace takes PASS_LENGTH seconds, so a
# screenshot partway shows the workspaces in between.
PASS_LENGTH=4
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	sed -E -e 's/cubic-bezier\([^)]*\)/linear/' -e "s/var-length = [0-9.]+;/var-length = $PASS_LENGTH;/" \
		"$PASS_STYLE"
	echo '});'
} >"$OUT/pass.conf"
# pass-slide as slow as pass.conf, but with its own curve, for check-frames.py: a linear
# curve stops at full speed.
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	sed -E "s/var-length = [0-9.]+;/var-length = $PASS_LENGTH;/" "$PASS_STYLE"
	echo '});'
} >"$OUT/pass-eased.conf"
# The cube and flip styles, as slow as pass.conf, with their own curves so that
# check-frames.py judges them as they ship. They include ws-direction/parts/prism.conf,
# which picom looks for next to the config first, so the copy in $OUT is the one under
# test, also for the style checks below.
mkdir -p "$OUT/ws-direction/parts"
cp "$ROOT"/styles/parts/*.conf "$OUT/ws-direction/parts/"
for style in cube flip; do
	{
		echo "plugins = \"$PLUGIN\";"
		echo 'backend = "glx";'
		echo 'vsync = true;'
		echo 'use-damage = true;'
		echo 'unredir-if-possible = false;'
		echo 'rules = ({'
		echo "  match = \"!override_redirect && window_type = 'normal'\";"
		sed -E "s/var-length = [0-9.]+;/var-length = $PASS_LENGTH;/" "$ROOT/styles/$style.conf"
		echo '});'
	} >"$OUT/$style.conf"
done
# sleep_until T0 SECONDS: sleep until SECONDS after T0 (from date +%s.%N).
sleep_until() {
	local left
	left=$(awk -v t0="$1" -v s="$2" -v now="$(date +%s.%N)" \
		'BEGIN { d = t0 + s - now; print (d > 0 ? d : 0) }')
	sleep "$left"
}
# pass_time DISTANCE FRACTION: seconds until FRACTION of a pass-slide switch of DISTANCE
# workspaces is done. Each workspace passed adds a fifth of var-length.
pass_time() {
	awk -v l="$PASS_LENGTH" -v d="$1" -v f="$2" 'BEGIN { print l * (1 + 0.2 * (d - 1)) * f }'
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
# right_edge TITLE: the x just right of the window, in root coordinates.
right_edge() {
	xdotool getwindowgeometry --shell "$(win_id "$1")" |
		awk -F= '$1 == "X" { x = $2 } $1 == "WIDTH" { w = $2 } END { print x + w }'
}

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

# check_frames NAME LOG [OPTION...]: check every frame in picom's frame timing log LOG,
# written with PICOM_DEBUG="frame_timing=LOG;frame_layout=1", with tests/check-frames.py
# and its OPTIONs.
check_frames() {
	local report lines
	if report=$("$HERE/check-frames.py" "$2" "${MONITORS[@]}" "${@:3}" 2>&1); then
		pass "$1"
	else
		mapfile -t lines <<<"$report"
		fail "$1" "${lines[@]}"
	fi
}

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
# The same two monitors, for check-frames.py.
MONITORS=(--monitor "0,480,3440,1440" --monitor "3440,0,1080,1920")

read -ra XCB_FLAGS <<<"$(pkg-config --cflags --libs xcb)"
cc -O2 -Wall -o "$OUT/fakewm" "$HERE/fakewm.c" "${XCB_FLAGS[@]}" || die "cannot build fakewm"
cc -O2 -Wall -o "$OUT/slowpaint" "$HERE/slowpaint.c" "${XCB_FLAGS[@]}" ||
	die "cannot build slowpaint"

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
	pass "the show and hide test style loads (picom --diagnostics)"
else
	fail "the show and hide test style loads (picom --diagnostics)" "see $OUT/diagnostics.out"
fi
if grep -q "plugin API is not compatible" "$OUT/diagnostics.out"; then
	die "this picom does not have plugin API 0.9 of the picom fork, see $OUT/diagnostics.out"
fi
"$PICOM" --config "$OUT/wave.conf" --diagnostics >"$OUT/diagnostics-wave.out" 2>&1
grep -qx "  - wave" "$OUT/diagnostics-wave.out" ||
	die "this picom has no custom animation triggers, see $OUT/diagnostics-wave.out"
if "$PICOM" --config "$OUT/pass.conf" --diagnostics >"$OUT/diagnostics-pass.out" 2>&1 &&
	grep -qx "  - ws-pass" "$OUT/diagnostics-pass.out" &&
	! grep -qiE "error.*(config|script|parse|line|trigger)" "$OUT/diagnostics-pass.out"; then
	pass "pass-slide style loads (picom --diagnostics)"
else
	fail "pass-slide style loads (picom --diagnostics)" "see $OUT/diagnostics-pass.out"
fi
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
# Installed by make install, cube and flip find their parts through picom's include
# directories, with no copy next to picom.conf (and none in /etc/xdg).
# Only what this run installs, not files an earlier run left there.
rm -rf "$OUT/installed"
mkdir -p "$OUT/installed/conf" "$OUT/installed/none"
if make -s -C "$ROOT" install PICOM_CONFIG_DIR="$OUT/installed/xdg/picom" \
	BINDIR="$OUT/installed/bin" MANDIR="$OUT/installed/man" >"$OUT/install.out" 2>&1; then
	{
		echo "plugins = \"$PLUGIN\";"
		echo 'backend = "glx";'
		echo 'rules = ({'
		echo "  match = \"name = 'a'\";"
		echo '  @include "ws-direction/cube.conf"'
		echo '}, {'
		echo "  match = \"name = 'b'\";"
		echo '  @include "ws-direction/flip.conf"'
		echo '});'
	} >"$OUT/installed/conf/picom.conf"
	XDG_CONFIG_HOME=$OUT/installed/xdg XDG_CONFIG_DIRS=$OUT/installed/none "$PICOM" \
		--config "$OUT/installed/conf/picom.conf" --diagnostics >"$OUT/installed.out" 2>&1
fi
if grep -q "Config file used: $OUT/installed/conf/picom.conf" "$OUT/installed.out" 2>/dev/null &&
	! grep -qiE "error.*(config|script|parse|line|trigger|include)|fatal" "$OUT/installed.out"; then
	pass "cube and flip load as make install puts them, parts and all"
else
	fail "cube and flip load as make install puts them, parts and all" \
		"see $OUT/install.out and $OUT/installed.out"
fi
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
assign [instance="^slow\$"] workspace "7:Extra"
assign [instance="^hang\$"] workspace "3:Term"
for_window [instance="^hang\$"] floating enable, resize set 888 px 400 px, move position 3677 px 100 px
assign [instance="^notes\$"] workspace "Notes"
assign [instance="^alpha\$"] workspace "Alpha"
for_window [instance="^scratch\$"] move scratchpad
EOF
# i3 keeps its error log, and its layout across a restart, in $XDG_RUNTIME_DIR/i3. Keep
# them here, not next to the files of the real i3.
run_bg "$OUT/i3.log" env XDG_RUNTIME_DIR="$WORK" "$I3" -c "$OUT/i3.conf"
I3_PID=$LAST_PID
wait_until 10 i3-msg -s "$SOCK" -t get_version || die "i3 did not start, see $OUT/i3.log"
outputs=$(i3-msg -s "$SOCK" -t get_outputs)
[[ $outputs == *'"name":"DPT"'* && $outputs == *'"name":"HDT"'* ]] ||
	die "i3 does not see the DPT and HDT outputs: $outputs"

# The colors are for the screenshot checks below.
I3_XTERMS=()
for spec in ws1:ws1-a:ff0000 ws1:ws1-b:ff0000 ws2:ws2-a:0000ff ws3:ws3-a:00ff00 \
	ws4:ws4-a:00ffff ws5:ws5-a:ffff00 ws7:ws7-a:ffffff notes:notes-a:ffffff \
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
# xrandr --setmonitor above gave DPT 800x337 mm and HDT 300x530 mm.
mm=$(tail -n +"$((CASE_MARK + 1))" "$LOG" |
	sed -nE 's/.*\] (show|hide) of window 0x[0-9a-f]+ \((ws1-a|ws4-a)\): .*, ([0-9]+x[0-9]+) mm, .*/\2 \3/p' |
	sort -u | tr '\n' ' ')
if [[ $mm == "ws1-a 800x337 ws4-a 800x337 " ]]; then
	pass "  (DPT's size in millimetres, from RandR: 800x337)"
else
	fail "  (DPT's size in millimetres, from RandR: 800x337)" "got: ${mm:-nothing}"
fi
i3_case "HDT 3->5" 'workspace 5:Music' "ws3-a hide 1" "ws5-a show 1"
mm=$(tail -n +"$((CASE_MARK + 1))" "$LOG" |
	sed -nE 's/.*\] (show|hide) of window 0x[0-9a-f]+ \((ws3-a|ws5-a)\): .*, ([0-9]+x[0-9]+) mm, .*/\3/p' |
	sort -u | tr '\n' ' ')
if [[ $mm == "300x530 " ]]; then
	pass "  (HDT's size in millimetres: 300x530)"
else
	fail "  (HDT's size in millimetres: 300x530)" "got: ${mm:-nothing}"
fi
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

# Screenshot checks: the show and hide test style made slow (6 s) and linear, so a screenshot
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

# A window that closes gets the monitor it was on, as the plugin's last read saw it. i3
# may unmap its frame before it destroys it: then picom fires hide.
{
	sed '/^rules = ({/,$d' "$OUT/wave.conf"
	cat <<'EOF'
rules = ({
  match = "!override_redirect && window_type = 'normal'";
  animations = ({
    triggers = ["close", "hide"];
    opacity = { duration = 0.3; start = 1; end = 0; };
    crop-x = "plugin-ws-direction-monitor-x";
    crop-y = "plugin-ws-direction-monitor-y";
    crop-width = "plugin-ws-direction-monitor-width";
    crop-height = "plugin-ws-direction-monitor-height";
  });
});
EOF
} >"$OUT/close.conf"
start_picom "$OUT/close.conf" picom-close
start_xterm newwin close-a
sleep "$SETTLE"
mark=$(log_mark)
stop "$LAST_PID"
sleep "$SETTLE"
closed=$(tail -n +"$((mark + 1))" "$LOG" | grep -E '(close|hide) of window 0x[0-9a-f]+ \((close-a)?\): direction = 0 ' |
	sed -nE 's/.*face (-?[0-9]+ of [0-9]+).*(monitor [0-9]+,[0-9]+ [0-9]+x[0-9]+),.* index ([0-9]+),.*/\1 \2 index \3/p' |
	head -n 1)
if [[ $closed == "0 of 6 $DPT_RECT index 0" ]]; then
	pass "a window that closes on DPT's 1:IDE gets DPT ($closed)"
else
	fail "a window that closes on DPT's 1:IDE gets DPT" "got: ${closed:-no close line}; see $LOG"
fi
stop_picom

# styles/pass-slide.conf, slowed down. The plugin starts the custom trigger ws-pass from
# its frame callback, on all xterms of the switch's monitor, without StartAnimations. Every workspace
# with an xterm is shown once under this picom, so the xterms have images to keep, and
# all of them get ws-pass. DPT: 1 is red, 2 is blue, 4 is cyan, 7 is white. HDT: 3 is
# green, 5 is yellow.
start_picom "$OUT/pass.conf" picom-pass
for ws in 2:Web 4:Misc 7:Extra 5:Music 3:Term 1:IDE; do
	i3cmd "workspace $ws"
	sleep "$SETTLE"
done
sleep "$(pass_time 3 1.1)"

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
import -window root "$OUT/shot.png"
before=$(color_part 1000 480 1 1440 0000FF)
mark=$(log_mark)
t0=$(date +%s.%N)
i3cmd 'workspace 4:Misc'
# A third of the way, the lower third of 1 is at the top of DPT and the upper two thirds
# of 2 below it. Halfway, 2 is where 1 was. At three quarters, the lower half of 2 is on
# top and the upper half of 4 below it. Black would be a gap between two workspaces; a
# few rows of it are where an xterm doesn't fill its frame.
sleep_until "$t0" "$(pass_time 2 0.33)"
import -window root "$OUT/shot-pass-1.png"
sleep_until "$t0" "$(pass_time 2 0.5)"
import -window root "$OUT/shot-pass.png"
sleep_until "$t0" "$(pass_time 2 0.75)"
import -window root "$OUT/shot-pass-3.png"
sleep_until "$t0" "$(pass_time 2 1.1)"
import -window root "$OUT/shot-pass-after.png"
# Read only now: reading a screenshot takes long enough to make the next one late.
cp "$OUT/shot-pass-1.png" "$OUT/shot.png"
q1_top=$(color_part 1000 480 1 360 FF0000)
q1_bottom=$(color_part 1000 1560 1 360 0000FF)
q1_gap=$(color_part 1000 480 1 1440 000000)
cp "$OUT/shot-pass.png" "$OUT/shot.png"
during=$(color_part 1000 480 1 1440 0000FF)
cp "$OUT/shot-pass-3.png" "$OUT/shot.png"
q3_top=$(color_part 1000 480 1 360 0000FF)
q3_bottom=$(color_part 1000 1560 1 360 00FFFF)
q3_gap=$(color_part 1000 480 1 1440 000000)
cp "$OUT/shot-pass-after.png" "$OUT/shot.png"
after=$(color_part 1000 480 1 1440 0000FF)
if ((before == 0 && during >= 50 && after == 0)); then
	pass "screenshot: DPT 1->4 draws 2, which it passes, halfway (#0000FF $before% $during% $after%)"
else
	fail "screenshot: DPT 1->4 draws 2, which it passes, halfway" \
		"#0000FF share of a DPT strip: before $before%, halfway $during% (at least 50), after $after%"
fi
if ((q1_top >= 90 && q1_bottom >= 90 && q1_gap <= 3 && q3_top >= 90 && q3_bottom >= 90 &&
	q3_gap <= 3)); then
	pass "screenshot: DPT 1->4 moves 1, 2 and 4 as one strip (black $q1_gap% and $q3_gap%)"
else
	fail "screenshot: DPT 1->4 moves 1, 2 and 4 as one strip" \
		"a third: top quarter #FF0000 $q1_top%, bottom quarter #0000FF $q1_bottom% (at least 90), black $q1_gap% (at most 3)" \
		"three quarters: top quarter #0000FF $q3_top%, bottom quarter #00FFFF $q3_bottom% (at least 90), black $q3_gap% (at most 3)"
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

pass_switch "DPT 4->1" 'workspace 1:IDE' \
	"ws7-a ws-pass direction -1 distance 2 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction -1 distance 2 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction -1 distance 2 face -1/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-a ws-pass direction -1 distance 2 face -2/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction -1 distance 2 face -2/6 selected 1 placed 1 $DPT_RECT"
sleep "$(pass_time 2 1)"
# Halfway through 1->7, the strip has moved one and a half monitor heights: the lower half
# of 2 is in the top half of DPT, the upper half of 4 in the bottom half. HDT stays green.
CASE_MARK=$(log_mark)
t0=$(date +%s.%N)
i3cmd 'workspace 7:Extra'
sleep_until "$t0" "$(pass_time 3 0.5)"
import -window root "$OUT/shot.png"
cp "$OUT/shot.png" "$OUT/shot-pass-7.png"
top=$(color_part 1000 480 1 360 0000FF)
bottom=$(color_part 1000 1560 1 360 00FFFF)
hdt=$(pixel 4000 1000)
if ((top >= 90 && bottom >= 90)) && [[ $hdt == "#00FF00" ]]; then
	pass "screenshot: DPT 1->7 draws 2 and then 4, which it passes, halfway (#0000FF $top% on top, #00FFFF $bottom% below)"
else
	fail "screenshot: DPT 1->7 draws 2 and then 4, which it passes, halfway" \
		"#0000FF share of the top quarter of a DPT strip $top%, #00FFFF of the bottom quarter $bottom% (at least 90 each)" \
		"HDT pixel $hdt (expected #00FF00)"
fi
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
sleep_until "$t0" "$(pass_time 3 1.1)"
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

# A switch that comes while one runs. Every xterm of DPT gets ws-pass again, and
# pass-slide's var-pos carries on from where each one is, so the workspaces still move as
# one strip. 1->2, and a quarter of the way 2->4: the strip is then a quarter of a
# workspace past 1 and goes on to 4 (linear, in pass_time 1). Halfway through the second
# switch it is 1.125 workspaces past 1: 2 fills DPT but for its top eighth, and the top
# eighth of 4 is below it. Without the carry-over, 4 would start a whole workspace below
# and cover the lower half of 2.
mark=$(log_mark)
t0=$(date +%s.%N)
i3cmd 'workspace 2:Web'
sleep_until "$t0" "$(pass_time 1 0.25)"
CASE_MARK=$(log_mark)
i3cmd 'workspace 4:Misc'
sleep_until "$t0" "$(pass_time 1 0.75)"
import -window root "$OUT/shot.png"
cp "$OUT/shot.png" "$OUT/shot-interrupt.png"
blue=$(color_part 1000 600 1 1050 0000FF)
cyan=$(color_part 1000 1830 1 90 00FFFF)
gap=$(color_part 1000 480 1 1440 000000)
if ((blue >= 90 && cyan >= 90 && gap <= 3)); then
	pass "screenshot: DPT 1->2, then 2->4 a quarter of the way, carries the strip on (black $gap%)"
else
	fail "screenshot: DPT 1->2, then 2->4 a quarter of the way, carries the strip on" \
		"#0000FF share of DPT rows 600-1650 $blue%, #00FFFF of rows 1830-1920 $cyan% (at least 90 each), black $gap% (at most 3)"
fi
GOT=$(variables_since "$CASE_MARK" ws-pass)
expect "  (ws-pass on all of DPT's xterms again)" \
	"ws1-a ws-pass direction 1 distance 1 face -1/6 selected 1 placed 1 $DPT_RECT" \
	"ws1-b ws-pass direction 1 distance 1 face -1/6 selected 1 placed 1 $DPT_RECT" \
	"ws2-a ws-pass direction 1 distance 1 face 0/6 selected 1 placed 1 $DPT_RECT" \
	"ws4-a ws-pass direction 1 distance 1 face 1/6 selected 1 placed 1 $DPT_RECT" \
	"ws7-a ws-pass direction 1 distance 1 face 2/6 selected 1 placed 1 $DPT_RECT"
pass_case "  (all in the pass of the root window change)" "$CASE_MARK" '"2:Web" -> "4:Misc"' \
	ws1-a ws1-b ws2-a ws4-a ws7-a
GOT=$(directions_since "$mark")
expect "  (no show or hide in either switch)" none
sleep_until "$t0" "$(pass_time 1 1.4)"
i3cmd 'workspace 1:IDE'
sleep "$(pass_time 2 1.1)"

# A window moved to a hidden workspace of the other monitor keeps its old place (placed
# 0), so pass-slide must not draw it when a switch passes its workspace. stale-a (orange)
# is the right column of DPT's 1, moved to HDT's 5. Then HDT goes from the empty 4:Gap
# to the empty 6. A quarter of the way, pass-slide would draw stale-a on HDT where the
# empty 4:Gap is, just right of DPT, with 5's yellow xterm to the right of it.
right=ws1-a
(($(right_edge ws1-b) > $(right_edge ws1-a))) && right=ws1-b
i3cmd "[title=\"^$right\$\"] focus"
start_xterm stale stale-a -bg "#ff8000"
STALE_PID=$LAST_PID
sleep "$SETTLE"
stale_edge=$(right_edge stale-a)
i3cmd '[title="^stale-a$"] move container to workspace 5:Music'
i3cmd 'focus output HDT'
i3cmd 'workspace 4:Gap'
sleep "$(pass_time 1 1.1)"
mark=$(log_mark)
t0=$(date +%s.%N)
i3cmd 'workspace 6'
sleep_until "$t0" "$(pass_time 2 0.25)"
import -window root "$OUT/shot.png"
cp "$OUT/shot.png" "$OUT/shot-placed.png"
stale=$(color_part 0 1000 4520 1 FF8000)
yellow=$(color_part 3440 1000 1080 1 FFFF00)
if ((stale_edge >= 3400 && stale == 0 && yellow >= 25 && yellow <= 90)); then
	pass "screenshot: HDT 4:Gap->6 draws 5's xterm, but not the one whose place is on DPT (#FFFF00 $yellow% of HDT)"
else
	fail "screenshot: HDT 4:Gap->6 draws 5's xterm, but not the one whose place is on DPT" \
		"#FF8000 share of the row $stale% (expected 0), #FFFF00 share of HDT $yellow% (25..90)" \
		"stale-a ended at x $stale_edge before the move (at least 3400, so it would show on HDT)"
fi
GOT=$(variables_since "$mark" ws-pass)
expect "  (ws-pass on HDT's xterms, stale-a not placed)" \
	"stale-a ws-pass direction 1 distance 2 face 1/3 selected 1 placed 0 $HDT_RECT" \
	"ws3-a ws-pass direction 1 distance 2 face -1/3 selected 1 placed 1 $HDT_RECT" \
	"ws5-a ws-pass direction 1 distance 2 face 1/3 selected 1 placed 1 $HDT_RECT"
pass_case "  (in the pass of the root window change)" "$mark" '"4:Gap" -> "6"' stale-a ws5-a
sleep_until "$t0" "$(pass_time 2 1.1)"
stop "$STALE_PID"
i3cmd 'workspace 3:Term'
i3cmd 'focus output DPT'
sleep "$(pass_time 2 1.1)"
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

# styles/wall.conf next to pass-slide. "ws-direction overview open" unfolds the focused
# monitor into an overview of its workspaces, "close" folds it, "toggle" toggles it. No
# animation-triggers line: the plugin declares ws-pass, ws-unfold and ws-fold. DPT is
# landscape, so its workspaces lie in a column (the strip runs along the short side). The
# shown workspace is bright, the others are at 70% opacity.
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	cat "$PASS_STYLE"
	echo '}, {'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	cat "$ROOT/styles/wall.conf"
	echo '});'
} >"$OUT/wall.conf"
unfold() { "$ROOT/build/ws-direction" overview "$1" || die "ws-direction overview $1 failed"; }
# tile_y FACES INDEX: the y of the middle of DPT's tile INDEX when unfolded.
tile_y() {
	awk -v n="$1" -v i="$2" 'BEGIN { k = 0.92 / (n * 1.05 - 0.05);
		printf "%d\n", 1200 + k * (i - (n - 1) / 2) * 1440 * 1.05 }'
}
# is_color COLOR COLORS: COLOR is one of the space-separated COLORS.
is_color() { [[ " $2 " == *" $1 "* ]]; }
# wall_case NAME MARK COLORS0 COLORS1: tiles 0 (1:IDE, red) and 1 (2:Web, blue) of DPT
# have one of COLORS0 and one of COLORS1 (one bright, one dim), and HDT is untouched.
wall_case() {
	local faces y0 y1 c0 c1 hdt
	faces=$(tail -n +"$(($2 + 1))" "$LOG" | grep -m1 -oE 'ws-unfold of window .* face -?[0-9]+ of [0-9]+' |
		sed -E 's/.* of ([0-9]+)$/\1/')
	y0=$(tile_y "${faces:-1}" 0)
	y1=$(tile_y "${faces:-1}" 1)
	import -window root "$OUT/shot.png"
	cp "$OUT/shot.png" "$OUT/shot-wall-$SHOTS.png"
	SHOTS=$((SHOTS + 1))
	c0=$(pixel 1600 "$y0")
	c1=$(pixel 1600 "$y1")
	hdt=$(pixel 3900 900)
	if [[ -n $faces ]] && is_color "$c0" "$3" && is_color "$c1" "$4" && [[ $hdt == "#00FF00" ]]; then
		pass "$1 ($faces faces; tile 0 $c0, tile 1 $c1)"
	else
		fail "$1" "$faces faces; tile 0 at y $y0: $c0, tile 1 at y $y1: $c1, HDT $hdt"
	fi
}
start_picom "$OUT/wall.conf" picom-wall
# Every workspace with an xterm shows once under this picom, so picom keeps their images.
for ws in 2:Web 4:Misc 7:Extra 5:Music 3:Term 1:IDE; do
	i3cmd "workspace $ws"
	sleep "$SETTLE"
done
sleep "$(pass_time 3 1.1)"
mark=$(log_mark)
unfold open
sleep 1
wall_case "wall: DPT unfolds, 1:IDE bright, 2:Web dimmed, HDT untouched" "$mark" \
	"#FF0000 #FF0000" "#0000B3 #0000B2"
started=$(tail -n +"$((mark + 1))" "$LOG" | grep -oE 'started ws-unfold on [0-9]+ of [0-9]+ windows' | head -1)
# Notes and Alpha were never shown under this picom, so picom has no image of their
# xterms and doesn't start the animation for them.
held=$(tail -n +"$((mark + 1))" "$LOG" | grep -c "ended, holding its pose")
unseen=$(tail -n +"$((mark + 1))" "$LOG" | grep -c "Not starting animation ws-unfold .* will not be rendered")
if [[ $started =~ on\ ([0-9]+)\ of\ ([0-9]+) ]] && ((BASH_REMATCH[1] > 0)) &&
	((held + unseen >= BASH_REMATCH[1] && unseen <= 2)); then
	pass "  ($started, $held hold their pose, $unseen have no image)"
else
	fail "  (the plugin starts ws-unfold on DPT's windows, and they hold)" \
		"start line: ${started:-none}, $held holding, $unseen without an image"
fi
# A window that opens on the unfolded monitor: the plugin starts ws-unfold again on all
# of DPT's windows, the new one too, and again when it closes.
case_mark=$(log_mark)
start_xterm newwin refresh-a
REFRESH_PID=$LAST_PID
sleep 1
refreshed=$(tail -n +"$((case_mark + 1))" "$LOG" | grep -c 'Unfold: the workspaces or windows of an unfolded monitor changed')
new_tile=$(tail -n +"$((case_mark + 1))" "$LOG" | grep -cE 'ws-unfold of window 0x[0-9a-f]+ \(refresh-a\): .*, unfolded 1,')
case_mark=$(log_mark)
stop "$REFRESH_PID"
sleep 1
closed=$(tail -n +"$((case_mark + 1))" "$LOG" | grep -c 'Unfold: the workspaces or windows of an unfolded monitor changed')
if ((refreshed >= 1 && new_tile >= 1 && closed >= 1)); then
	pass "  (a window that opens or closes on DPT while it is unfolded starts ws-unfold again)"
else
	fail "  (a window that opens or closes on DPT while it is unfolded starts ws-unfold again)" \
		"refreshes on open: $refreshed, ws-unfold of the new window: $new_tile, refreshes on close: $closed"
fi
# While DPT is unfolded the plugin takes only DPT's clicks: a click on HDT's xterm
# focuses it, and DPT stays open.
focused_output() {
	i3-msg -s "$SOCK" -t get_workspaces | jq -r '.[] | select(.focused) | .output'
}
case_mark=$(log_mark)
xdotool mousemove 3900 900 click 1
sleep 0.3
if [[ $(focused_output) == HDT ]] && ! tail -n +"$((case_mark + 1))" "$LOG" | grep -qE 'started ws-fold|Unfold: the click'; then
	pass "  (a click on HDT while DPT is unfolded focuses HDT's xterm, and DPT stays open)"
else
	fail "  (a click on HDT while DPT is unfolded focuses HDT's xterm, and DPT stays open)" \
		"the focus is on $(focused_output); see $LOG"
fi
if tail -n +"$((mark + 1))" "$LOG" |
	grep -qE 'ws-unfold of window 0x[0-9a-f]+ \(ws2-a\): .*, index 1, shown 0, unfolded 1,'; then
	pass "  (ws2-a: index 1, DPT shows index 0, unfolded)"
else
	fail "  (ws2-a: index 1, DPT shows index 0, unfolded)" "see $LOG"
fi
mark=$(log_mark)
i3cmd 'workspace 2:Web'
sleep 1
wall_case "wall: switching while unfolded moves the bright tile to 2:Web" "$mark" \
	"#B30000 #B20000" "#0000FF #0000FF"
others=$(tail -n +"$((mark + 1))" "$LOG" | grep -cE "Starting animation (show|hide) for window")
if ((others == 0)) && tail -n +"$((mark + 1))" "$LOG" | grep -q 'started ws-unfold on'; then
	pass "  (the switch starts ws-unfold again, and no show or hide)"
else
	fail "  (the switch starts ws-unfold again, and no show or hide)" \
		"$others shows or hides; see $LOG"
fi
mark=$(log_mark)
unfold close
sleep 1
import -window root "$OUT/shot.png"
folded=$(pixel 1000 1200)
if [[ $folded == "#0000FF" ]] && tail -n +"$((mark + 1))" "$LOG" | grep -q 'started ws-fold on'; then
	pass "wall: folding shows 2:Web full size again"
else
	fail "wall: folding shows 2:Web full size again" "pixel $folded"
fi
xdotool mousemove 3900 900 click 1
sleep 0.3
if [[ $(focused_output) == HDT ]]; then
	pass "  (after folding, a click focuses HDT's xterm again)"
else
	fail "  (after folding, a click focuses HDT's xterm again)" "focus on $(focused_output)"
fi
# Back over DPT: i3 focuses the monitor under the pointer when it restarts (below).
xdotool mousemove 2260 960
i3cmd 'focus output DPT'
sleep "$SETTLE"
mark=$(log_mark)
unfold toggle
sleep 1
wall_case "wall: toggle opens it" "$mark" "#B30000 #B20000" "#0000FF #0000FF"
unfold toggle
sleep 1
import -window root "$OUT/shot.png"
folded=$(pixel 1000 1200)
if [[ $folded == "#0000FF" ]]; then
	pass "wall: toggle again closes it"
else
	fail "wall: toggle again closes it" "pixel $folded"
fi
# Each command is an event of its own: two toggles at the same moment are both done.
mark=$(log_mark)
"$ROOT/build/ws-direction" overview toggle &
t1=$!
"$ROOT/build/ws-direction" overview toggle &
t2=$!
wait "$t1" "$t2"
sleep 1
toggles=$(tail -n +"$((mark + 1))" "$LOG" | grep -c 'Unfold: command 3')
opened=$(tail -n +"$((mark + 1))" "$LOG" | grep -c 'Monitor at 0,480: started ws-unfold')
folds=$(tail -n +"$((mark + 1))" "$LOG" | grep -c 'Monitor at 0,480: started ws-fold')
if ((toggles == 2 && opened == 1 && folds == 1)); then
	pass "wall: two toggles at once are both done, in order"
else
	fail "wall: two toggles at once are both done, in order" \
		"$toggles toggle commands, $opened unfolds, $folds folds (2, 1, 1)"
fi
# open --output opens the monitor of that name, --all every monitor.
mark=$(log_mark)
"$ROOT/build/ws-direction" overview open --output HDT
output_status=$?
sleep 1
hdt_only=$(tail -n +"$((mark + 1))" "$LOG" | grep -cE 'Monitor at (0,480|3440,0): started ws-unfold')
hdt=$(tail -n +"$((mark + 1))" "$LOG" | grep -c 'Monitor at 3440,0: started ws-unfold')
unfold close
sleep 1
mark=$(log_mark)
"$ROOT/build/ws-direction" overview open --all
all_status=$?
sleep 1
both=$(tail -n +"$((mark + 1))" "$LOG" | grep -cE 'Monitor at (0,480|3440,0): started ws-unfold')
unfold close
sleep 1
nope=$("$ROOT/build/ws-direction" overview open --output NOPE 2>&1)
nope_status=$?
if ((output_status == 0 && hdt == 1 && hdt_only == 1 && all_status == 0 && both == 2 &&
	nope_status == 1)) && [[ $nope == *"no monitor with that name"* ]]; then
	pass "wall: open --output HDT opens HDT, --all both, --output NOPE fails and says why"
else
	fail "wall: open --output HDT opens HDT, --all both, --output NOPE fails and says why" \
		"--output HDT: status $output_status, $hdt_only unfolds, $hdt on HDT" \
		"--all: status $all_status, $both unfolds" "--output NOPE: status $nope_status, $nope"
fi
# With --wait --buttons, another button returns 10 plus its number, and the overview
# stays open: the right button 13, the wheel down 15.
mark=$(log_mark)
"$ROOT/build/ws-direction" overview open --wait --buttons &
waiter=$!
sleep 1
xdotool mousemove 100 1200 click 3
wait "$waiter"
right=$?
"$ROOT/build/ws-direction" overview open --wait --buttons &
waiter=$!
sleep 0.5
xdotool mousemove 100 1200 click 5
wait "$waiter"
wheel=$?
folds=$(tail -n +"$((mark + 1))" "$LOG" | grep -c 'started ws-fold')
unfold close
sleep 1
if ((right == 13 && wheel == 15 && folds == 0)); then
	pass "wall: open --wait --buttons returns 13 for the right button and 15 for the wheel down, and the overview stays"
else
	fail "wall: open --wait --buttons returns 13 for the right button and 15 for the wheel down, and the overview stays" \
		"right button: $right, wheel down: $wheel, folds meanwhile: $folds"
fi
# A click on a window in the overview picks it: i3 shows its workspace and focuses it,
# the overview closes, and "open --wait" returns 0. A click on no window does nothing,
# and "open --wait" returns 1 when close closes the overview.
focused_ws() { i3-msg -s "$SOCK" -t get_workspaces | jq -r '.[] | select(.focused) | .name'; }
# wait_for_exit PID SECONDS: the exit status of PID, or 124 if it still runs by then.
wait_for_exit() {
	local i
	for ((i = 0; i < $2 * 10; i++)); do
		if ! kill -0 "$1" 2>/dev/null; then
			wait "$1"
			return
		fi
		sleep 0.1
	done
	kill "$1"
	wait "$1" 2>/dev/null
	return 124
}
mark=$(log_mark)
"$ROOT/build/ws-direction" overview open --wait &
waiter=$!
sleep 1
faces=$(tail -n +"$((mark + 1))" "$LOG" | grep -m1 -oE 'ws-unfold of window .* face -?[0-9]+ of [0-9]+' |
	sed -E 's/.* of ([0-9]+)$/\1/')
# Left of the column of tiles there is only the wallpaper.
xdotool mousemove 100 1200 click 1
sleep 0.5
if tail -n +"$((mark + 1))" "$LOG" | grep -q 'Unfold: the click at 100,1200 is on no window' &&
	kill -0 "$waiter" 2>/dev/null; then
	pass "wall: a click on no window leaves the overview open"
else
	fail "wall: a click on no window leaves the overview open" "see $LOG"
fi
xdotool mousemove 1600 "$(tile_y "${faces:-1}" 0)" click 1
wait_for_exit "$waiter" 3
status=$?
sleep 1
import -window root "$OUT/shot.png"
folded=$(pixel 1000 1200)
if ((status == 0)) && [[ $(focused_ws) == 1:IDE && $folded == "#FF0000" ]] &&
	tail -n +"$((mark + 1))" "$LOG" | grep -q 'started ws-fold on'; then
	pass "wall: a click on 1:IDE's tile shows 1:IDE full size, and open --wait returns 0"
else
	fail "wall: a click on 1:IDE's tile shows 1:IDE full size, and open --wait returns 0" \
		"open --wait returned $status, focus on $(focused_ws), pixel $folded"
fi
# 1:IDE shows now: a click on its own tile folds at once, without a switch.
mark=$(log_mark)
"$ROOT/build/ws-direction" overview open --wait &
waiter=$!
sleep 1
xdotool mousemove 1600 "$(tile_y "${faces:-1}" 0)" click 1
wait_for_exit "$waiter" 3
status=$?
sleep 1
import -window root "$OUT/shot.png"
folded=$(pixel 1000 1200)
if ((status == 0)) && [[ $(focused_ws) == 1:IDE && $folded == "#FF0000" ]] &&
	tail -n +"$((mark + 1))" "$LOG" | grep -q 'Monitor at .*: started ws-fold on' &&
	! tail -n +"$((mark + 1))" "$LOG" | grep -q 'Switch .* started ws-fold'; then
	pass "wall: a click on the shown workspace's tile folds at once, and open --wait returns 0"
else
	fail "wall: a click on the shown workspace's tile folds at once, and open --wait returns 0" \
		"open --wait returned $status, focus on $(focused_ws), pixel $folded; see $LOG"
fi
"$ROOT/build/ws-direction" overview open --wait &
waiter=$!
sleep 1
unfold close
wait_for_exit "$waiter" 3
status=$?
if ((status == 1)); then
	pass "wall: open --wait returns 1 when close closes the overview"
else
	fail "wall: open --wait returns 1 when close closes the overview" "it returned $status"
fi
i3cmd 'workspace 1:IDE'
sleep "$(pass_time 1 1.1)"
stop_picom

# styles/cube.conf and styles/flip.conf put the workspaces on the sides of a box, or of a
# card, and turn it in perspective. DPT is landscape, so they turn up and down: in 1->2,
# 1 (red) turns up and away, and 2 (blue) comes up from below. A side's edge that is
# further away is shorter. row_share Y COLOR: the share of DPT's row Y that is COLOR.
row_share() { color_part 0 "$1" 3440 1 "$2"; }
# turn_case NAME STYLE FRACTION RED-FAR RED-NEAR [BLUE-NEAR BLUE-FAR]: with STYLE, switch
# DPT 1->2 and take a screenshot at FRACTION of the switch. The rows RED-FAR and RED-NEAR
# of red, and BLUE-FAR and BLUE-NEAR of blue, are at least 40% that color, and the far one
# has at least 5 points less of it (drawn flat, they would have the same). Without the
# blue rows, no blue is drawn: the back of 2's side is turned toward us. HDT keeps its
# color, and above DPT stays dark. Then more switches without screenshots: cube turns
# HDT, which is portrait, so sideways; flip passes 2:Web from 1:IDE to 4:Misc. Last,
# check-frames.py checks every frame of the switches in picom's frame timing log.
turn_case() {
	local name=$1 style=$2 t0 hdt0 hdt above got ok=1 y blue='' frames=$OUT/frames-$2.csv
	rm -f "$frames"
	PICOM_DEBUG="frame_timing=$frames;frame_layout=1" start_picom "$OUT/$style.conf" "picom-$style"
	# picom keeps the image of 2:Web once it showed it.
	i3cmd 'workspace 2:Web'
	sleep "$(pass_time 1 1.1)"
	i3cmd 'workspace 1:IDE'
	sleep "$(pass_time 1 1.1)"
	import -window root "$OUT/shot.png"
	hdt0=$(pixel 3900 1000)
	t0=$(date +%s.%N)
	i3cmd 'workspace 2:Web'
	sleep_until "$t0" "$(pass_time 1 "$3")"
	import -window root "$OUT/shot.png"
	cp "$OUT/shot.png" "$OUT/shot-$style.png"
	hdt=$(pixel 3900 1000)
	above=$(pixel 1000 300)
	got="red rows $4: $(row_share "$4" FF0000)%, $5: $(row_share "$5" FF0000)%"
	(($(row_share "$4" FF0000) >= 40 && $(row_share "$4" FF0000) + 5 <= $(row_share "$5" FF0000))) || ok=0
	if (($# > 5)); then
		got+=", blue rows $7: $(row_share "$7" 0000FF)%, $6: $(row_share "$6" 0000FF)%"
		(($(row_share "$7" 0000FF) >= 40 && $(row_share "$7" 0000FF) + 5 <= $(row_share "$6" 0000FF))) || ok=0
	else
		for y in 700 1200 1700; do blue+=" $(row_share "$y" 0000FF)"; done
		got+=", blue in rows 700 1200 1700:$blue"
		[[ $blue == " 0 0 0" ]] || ok=0
	fi
	[[ $hdt == "$hdt0" && $above != "#FF0000" && $above != "#0000FF" ]] || ok=0
	if ((ok)); then
		pass "$name ($got)"
	else
		fail "$name" "$got; HDT $hdt0 -> $hdt, above DPT $above; see $OUT/shot-$style.png"
	fi
	sleep "$(pass_time 1 1)"
	i3cmd 'workspace 1:IDE'
	sleep "$(pass_time 1 1.1)"
	if [[ $style == cube ]]; then
		for ws in 5:Music 3:Term; do
			i3cmd "workspace $ws"
			sleep "$(pass_time 1 1.1)"
		done
		i3cmd 'focus output DPT'
	else
		for ws in 4:Misc 1:IDE; do
			i3cmd "workspace $ws"
			sleep "$(pass_time 2 1.1)"
		done
	fi
	stop_picom
	check_frames "frames: $style: no jumps or pops, ends at rest, the side in front on its monitor, in perspective, back faces not drawn, one prism" "$frames"
}
# DPT has six workspaces, so the box has six sides.
turn_case "screenshot: cube, DPT 1->2 halfway: 1 tips up and away, 2 comes up from below, in perspective" \
	cube 0.5 560 1120 1280 1840
# On flip's curve, a quarter of the turn is done at 0.35 of the switch.
turn_case "screenshot: flip, DPT 1->2 a quarter in: the card tips over, its back (2) not drawn" \
	flip 0.35 1000 1500

# styles/unfold.conf next to cube.conf: the overview opens the box of workspaces up into a
# flat strip. Unfolded, DPT's workspaces lie in a column, edge to edge, together 92% of
# the monitor high (unfold.conf's var-fill), the shown one bright and the others at 70%.
# Then check-frames.py checks every frame of opening, of a switch while it is open, and
# of closing, also of a switch while it still opens and of closing while it still turns.
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "glx";'
	echo 'vsync = true;'
	echo 'use-damage = true;'
	echo 'unredir-if-possible = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	cat "$ROOT/styles/cube.conf"
	echo '}, {'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	cat "$ROOT/styles/unfold.conf"
	echo '});'
} >"$OUT/unfold.conf"
# strip_y FACES INDEX: the y of the middle of DPT's side INDEX when unfolded.
strip_y() {
	awk -v n="$1" -v i="$2" 'BEGIN { printf "%d\n", 1200 + 0.92 / n * (i - (n - 1) / 2) * 1440 }'
}
# unfold_case NAME MARK COLORS0 COLORS1: sides 0 (1:IDE, red) and 1 (2:Web, blue) of DPT,
# unfolded, have one of COLORS0 and one of COLORS1 (one bright, one dim), and HDT is
# untouched.
unfold_case() {
	local faces y0 y1 c0 c1 hdt
	faces=$(tail -n +"$(($2 + 1))" "$LOG" | grep -m1 -oE 'ws-unfold of window .* face -?[0-9]+ of [0-9]+' |
		sed -E 's/.* of ([0-9]+)$/\1/')
	y0=$(strip_y "${faces:-1}" 0)
	y1=$(strip_y "${faces:-1}" 1)
	import -window root "$OUT/shot.png"
	cp "$OUT/shot.png" "$OUT/shot-unfold-$SHOTS.png"
	SHOTS=$((SHOTS + 1))
	c0=$(pixel 1600 "$y0")
	c1=$(pixel 1600 "$y1")
	hdt=$(pixel 3900 900)
	if [[ -n $faces ]] && is_color "$c0" "$3" && is_color "$c1" "$4" && [[ $hdt == "#00FF00" ]]; then
		pass "$1 ($faces faces; side 0 $c0, side 1 $c1)"
	else
		fail "$1" "$faces faces; side 0 at y $y0: $c0, side 1 at y $y1: $c1, HDT $hdt"
	fi
}
rm -f "$OUT/frames-unfold.csv"
PICOM_DEBUG="frame_timing=$OUT/frames-unfold.csv;frame_layout=1" start_picom "$OUT/unfold.conf" picom-unfold
for ws in 2:Web 4:Misc 7:Extra 5:Music 3:Term 1:IDE; do
	i3cmd "workspace $ws"
	sleep 0.6
done
sleep 1
mark=$(log_mark)
unfold open
sleep 1.5
unfold_case "unfold: DPT opens into a flat strip, 1:IDE bright, 2:Web dimmed, HDT untouched" \
	"$mark" "#FF0000 #FF0000" "#0000B3 #0000B2"
mark=$(log_mark)
i3cmd 'workspace 2:Web'
sleep 1.5
unfold_case "unfold: switching while it is open moves the bright side to 2:Web" "$mark" \
	"#B30000 #B20000" "#0000FF #0000FF"
unfold close
sleep 1.5
import -window root "$OUT/shot.png"
folded=$(pixel 1000 1200)
if [[ $folded == "#0000FF" ]]; then
	pass "unfold: closing folds the box with 2:Web in front, full size"
else
	fail "unfold: closing folds the box with 2:Web in front, full size" "pixel $folded"
fi
# A switch while the box still opens, and closing it while it still turns, for the frame
# check below.
unfold open
sleep 0.15
i3cmd 'workspace 4:Misc'
sleep 0.2
unfold close
sleep 1.5
i3cmd 'workspace 1:IDE'
sleep 1
# The sides are drawn through their corners: a click on 2:Web's side picks it there too.
mark=$(log_mark)
unfold open
sleep 1
faces=$(tail -n +"$((mark + 1))" "$LOG" | grep -m1 -oE 'ws-unfold of window .* face -?[0-9]+ of [0-9]+' |
	sed -E 's/.* of ([0-9]+)$/\1/')
xdotool mousemove 1600 "$(strip_y "${faces:-1}" 1)" click 1
sleep 1.5
import -window root "$OUT/shot.png"
folded=$(pixel 1000 1200)
if [[ $(focused_ws) == 2:Web && $folded == "#0000FF" ]]; then
	pass "unfold: a click on 2:Web's side shows 2:Web full size"
else
	fail "unfold: a click on 2:Web's side shows 2:Web full size" \
		"focus on $(focused_ws), pixel $folded; see $LOG"
fi
i3cmd 'workspace 1:IDE'
sleep 1
stop_picom
check_frames "  (and every frame passes check-frames.py, the box in perspective)" "$OUT/frames-unfold.csv"

# Every style as it ships, at its own length, through switches that interrupt each other
# at 0.3 and 0.7 of a switch, the same way and the other way, on DPT and on HDT, which
# turns sideways; with an overview style next to its switch style (wall to pass-zoom-pan,
# unfold to cube), the overview also opens halfway into a switch. check-frames.py checks
# every frame, with --seams for the styles whose workspaces tile the monitor.
declare -A OVERVIEW_OF=([pass-zoom-pan]=wall [cube]=unfold)
# style_length STYLE: the style's var-length, in seconds.
style_length() { sed -nE 's/.*var-length = ([0-9.]+);.*/\1/p' "$ROOT/styles/$1.conf" | head -n 1; }
style_frames() {
	local style=$1 overview=${OVERVIEW_OF[$1]:-} length wait frames seams=() spec ws t0='' ok
	length=$(style_length "$style")
	[[ -z $overview ]] || length=$(awk -v a="$length" -v b="$(style_length "$overview")" \
		'BEGIN { print (a > b ? a : b) }')
	wait=$(awk -v l="$length" 'BEGIN { print 2 * l }')
	frames=$OUT/frames-style-$style.csv
	{
		echo "plugins = \"$PLUGIN\";"
		echo 'backend = "glx";'
		echo 'vsync = true;'
		echo 'use-damage = true;'
		echo 'unredir-if-possible = false;'
		echo 'rules = ({'
		echo "  match = \"!override_redirect && window_type = 'normal'\";"
		cat "$ROOT/styles/$style.conf"
		if [[ -n $overview ]]; then
			echo '}, {'
			echo "  match = \"!override_redirect && window_type = 'normal'\";"
			cat "$ROOT/styles/$overview.conf"
		fi
		echo '});'
	} >"$OUT/style-frames.conf"
	rm -f "$frames"
	PICOM_DEBUG="frame_timing=$frames;frame_layout=1" start_picom "$OUT/style-frames.conf" \
		"picom-style-$style"
	# picom keeps the images of the windows it showed since it started.
	for ws in 2:Web 4:Misc 7:Extra 1:IDE HDT 5:Music 3:Term DPT; do
		case $ws in
		DPT | HDT) i3cmd "focus output $ws" ;;
		*) i3cmd "workspace $ws" ;;
		esac
		sleep "$wait"
	done
	for spec in 2:Web 1:IDE 7:Extra 4:Misc@0.3 1:IDE@0.7 2:Web 4:Misc@0.3 1:IDE@0.7 \
		HDT 5:Music 3:Term@0.3 5:Music@0.7 DPT ${overview:+2:Web o@0.5 o 1:IDE}; do
		ws=${spec%@*}
		if [[ $ws == DPT || $ws == HDT ]]; then
			i3cmd "focus output $ws"
			continue
		fi
		if [[ $spec == *@* && -n $t0 ]]; then
			sleep_until "$t0" "$(awk -v l="$length" -v f="${spec#*@}" 'BEGIN { print l * f }')"
		elif [[ -n $t0 ]]; then
			sleep_until "$t0" "$wait"
		fi
		t0=$(date +%s.%N)
		if [[ $ws == o ]]; then
			"$ROOT/build/ws-direction" overview toggle
		else
			i3cmd "workspace $ws"
		fi
	done
	sleep "$wait"
	stop_picom
	case $style in
	slide | pass-slide | push | wipe | fold) seams=(--seams) ;;
	esac
	ok="no jumps or pops, no stop at full speed, ends at rest"
	((${#seams[@]})) && ok+=", the workspaces tile the monitor"
	check_frames "frames: $style${overview:+ and $overview}, switches interrupted at 0.3 and 0.7: $ok" \
		"$frames" "${seams[@]}"
}
for style_file in "$ROOT"/styles/*.conf; do
	style=$(basename "$style_file" .conf)
	# The overview styles run next to their switch styles.
	grep -q 'triggers = \["show"' "$style_file" || continue
	style_frames "$style"
done
# pass-zoom-cascade's var-new is 1 for the windows of the new workspace and 0 for the
# others; it was 2^50 for those beyond it, whose wait then outlasted the switch.
odd=$(grep -oE 'var-new=[^,]*' "$OUT/frames-style-pass-zoom-cascade.csv" | sort -u |
	grep -vxE 'var-new=(0|1)')
if [[ -z $odd ]]; then
	pass "pass-zoom-cascade: var-new is 0 or 1"
else
	fail "pass-zoom-cascade: var-new is 0 or 1" "$(tr '\n' ' ' <<<"$odd")"
fi

# An i3 restart puts every window in a new frame, and picom has no image of the new frames
# of hidden workspaces. A picom config change drops all images too. The plugin sees
# hidden windows lose the image picom kept of them, and shows their workspaces once, with
# the screen and the keyboard frozen, then goes back. DPT shows the empty workspace 8 for
# the screenshots, so nothing else is drawn where ws2-a would be.
shown_workspaces() {
	i3-msg -s "$SOCK" -t get_workspaces |
		jq -r '.[] | select(.visible) | "\(.output) \(.name) focused=\(.focused)"' | sort
}
# wave_ws2 NAME DRAWN: show the empty workspace 8 on DPT, start wave on ws2-a, and check
# that it is drawn (DRAWN 1) or not (0).
wave_ws2() {
	local got drawn=0
	i3cmd 'workspace 8'
	sleep "$SETTLE"
	start_wave "$(win_id ws2-a)"
	sleep 0.7
	import -window root "$OUT/shot.png"
	got=$(pixel 1720 1200)
	[[ $got == "#0000FF" ]] && drawn=1
	if [[ $reply == *"uint32 1"* && $drawn == "$2" ]]; then
		pass "$1"
	else
		fail "$1" "pixel $got, reply: ${reply//$'\n'/ }"
	fi
	sleep 2
}
# warm_case NAME MARK BEFORE: after line MARK, the plugin got the images back: it showed
# workspaces with the screen frozen, no animation started meanwhile, and the workspaces
# shown are BEFORE again.
# Every window draws all of itself in time (slow-a too, at 150 ms), and the plugin takes
# well under its limit of 0.5 s per workspace it shows: 0.25 s.
warm_case() {
	local lines shown first last starts late ms n
	wait_until 10 grep -q "Warm: done" <(tail -n +"$(($2 + 1))" "$LOG")
	sleep "$SETTLE"
	lines=$(tail -n +"$(($2 + 1))" "$LOG")
	shown=$(grep -oE 'Warm: will show "[^"]+"' <<<"$lines" | cut -d'"' -f2 | sort | tr '\n' ' ')
	first=$(grep -n -m1 "lost their image" <<<"$lines" | cut -d: -f1)
	last=$(grep -n -m1 "Warm: done" <<<"$lines" | cut -d: -f1)
	starts=$( (sed -n "${first:-1},${last:-1}p" <<<"$lines" | grep -c "Starting animation") || true)
	late=$( (grep -c "did not draw all of itself in time" <<<"$lines") || true)
	ms=$(grep -oE 'done in [0-9]+ ms' <<<"$lines" | head -n 1 | grep -oE '[0-9]+')
	n=$( (grep -c 'Warm: will show' <<<"$lines") || true)
	if [[ -n $first && -n $last && $starts == 0 && $(shown_workspaces) == "$3" &&
		$late == 0 && -n $ms ]] && ((ms < n * 250)); then
		pass "$1 (showed: $shown$(grep -oE 'done in [0-9]+ ms' <<<"$lines"))"
	else
		fail "$1" "lost line ${first:-none}, done line ${last:-none}, $starts animations meanwhile" \
			"$late windows did not draw in time; done in ${ms:-?} ms for $n workspaces" \
			"workspaces before: $(tr '\n' ' ' <<<"$3")" \
			"workspaces after:  $(shown_workspaces | tr '\n' ' ')"
	fi
}
# warm_probe MARK: wait until the plugin froze the screen after line MARK, then try to
# grab the pointer and the keyboard ourselves. Sets PROBE to fakewm's answer, and
# PROBE_LATE to the number of "Warm: done" lines the log had by then.
warm_probe() {
	local deadline=$((SECONDS + 10))
	until sed -n "$(($1 + 1)),\$p" "$LOG" | grep -q "Warm: .* freezing the screen"; do
		((SECONDS < deadline)) || break
		sleep 0.02
	done
	PROBE=$("$OUT/fakewm" grabprobe 2>&1)
	PROBE_LATE=$(sed -n "$(($1 + 1)),\$p" "$LOG" | grep -c "Warm: done")
}
# slow-a, on 7:Extra, paints itself 150 ms after each expose, well within the plugin's
# limit of 0.5 s. The X server paints it red when it maps, which is not its drawing.
run_bg /dev/null "$OUT/slowpaint" slow slow-a 150
I3_XTERMS+=("$LAST_PID")
wait_until 10 has_window slow-a || die "slow-a did not appear"
start_picom "$OUT/wave.conf" picom-warm
# Every workspace with an xterm shows once under this picom, so the plugin sees picom keep
# their images. Then the monitors stay calm for a second.
for ws in 2:Web 4:Misc 7:Extra 5:Music 3:Term 1:IDE; do
	i3cmd "workspace $ws"
	sleep "$SETTLE"
done
sleep 1.5
before=$(shown_workspaces)
# From the restart on, i3 moves the pointer to the monitor that gets the focus, which a
# workspace on the other monitor gets while the plugin shows it.
sed -i 's/^mouse_warping none$/mouse_warping output/' "$OUT/i3.conf"
xdotool mousemove 200 900
mark=$(log_mark)
i3-msg -s "$SOCK" restart >/dev/null 2>&1
warm_probe "$mark"
wait_until 10 i3-msg -s "$SOCK" -t get_version || die "i3 did not come back after a restart"
warm_case "i3 restart: the plugin shows the hidden workspaces once, frozen, and goes back" \
	"$mark" "$before"
if [[ $PROBE =~ ^pointer\ ([0-9-]+)\ keyboard\ ([0-9-]+)$ ]] &&
	((BASH_REMATCH[1] > 0 && BASH_REMATCH[2] > 0 && PROBE_LATE == 0)); then
	pass "  (meanwhile the pointer and the keyboard are frozen: $PROBE)"
else
	fail "  (meanwhile the pointer and the keyboard are frozen)" \
		"fakewm grabprobe: ${PROBE:-nothing}, done lines before it: $PROBE_LATE"
fi
location=$(xdotool getmouselocation | sed -nE 's/^x:([0-9]+) y:([0-9]+) .*/\1,\2/p')
if [[ $location == 200,900 ]]; then
	pass "  (the pointer is back where it was, though i3 moved it)"
else
	fail "  (the pointer is back where it was, though i3 moved it)" "at $location, not 200,900"
fi
wave_ws2 "  (screenshot: a hidden xterm is drawn after the i3 restart)" 1
# picom's image of slow-a is copied when picom binds it (Xvfb's GLX), so a screenshot
# can't show its late paint: the plugin's log says when picom saw it draw all of itself.
slow_ms=$(sed -n "$((mark + 1)),\$p" "$LOG" | grep -m1 "Warm: window $(printf '%#010x' "$(win_id slow-a)") drew all" |
	sed -nE 's/.* in ([0-9]+) ms$/\1/p')
if [[ -n $slow_ms ]] && ((slow_ms >= 150)); then
	pass "  (the plugin waits for a window that paints itself 150 ms after it shows: $slow_ms ms)"
else
	fail "  (the plugin waits for a window that paints itself 150 ms after it shows)" \
		"logged: ${slow_ms:-nothing}"
fi

# DPT shows the empty 8 now. i3 removes an empty workspace once it is hidden, so the plugin
# can't go back to it, and leaves DPT alone until it shows a workspace with windows. Then
# it starts in the pass of that switch.
sleep 1.5
i3-msg -s "$SOCK" restart >/dev/null 2>&1
sleep 1
wait_until 10 i3-msg -s "$SOCK" -t get_version || die "i3 did not come back after a restart"
sleep 2
wave_ws2 "i3 restart on an empty workspace: DPT's hidden xterms stay without images" 0
mark=$(log_mark)
i3cmd 'workspace 1:IDE'
warm_case "  (once DPT shows a workspace with windows, the plugin gets them back)" \
	"$mark" $'DPT 1:IDE focused=true\nHDT 3:Term focused=false'
wave_ws2 "  (screenshot: then the hidden xterm is drawn)" 1

# A config change: picom starts over, and drops every image.
i3cmd 'workspace 1:IDE'
sleep 1.5
before=$(shown_workspaces)
mark=$(log_mark)
: >>"$OUT/wave.conf"
warm_case "picom config change: the plugin gets the images back" "$mark" "$before"
wave_ws2 "  (screenshot: a hidden xterm is drawn after the config change)" 1
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
stop_picom

# When to get images back is the user's choice. A picom that starts has no images of the
# hidden workspaces, and the plugin doesn't show them by itself; "ws-direction warm"
# does, and returns once it is done.
start_picom "$OUT/wave.conf" picom-warm-command
wave_ws2 "a new picom has no images of hidden workspaces, the plugin leaves them" 0
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
mark=$(log_mark)
warm_out=$("$ROOT/build/ws-direction" warm 2>&1)
warm_status=$?
lines=$(tail -n +"$((mark + 1))" "$LOG")
if ((warm_status == 0)) && grep -q "Warm: .* as asked, freezing" <<<"$lines" &&
	grep -q "Warm: done" <<<"$lines" && [[ -z $warm_out ]] &&
	! grep -q "did not draw all of itself in time" <<<"$lines"; then
	pass "ws-direction warm gets the images back, and returns when done ($(grep -oE 'done in [0-9]+ ms' <<<"$lines"))"
else
	fail "ws-direction warm gets the images back, and returns when done" \
		"exit status $warm_status, output: $warm_out, see $LOG"
fi
wave_ws2 "  (screenshot: then a hidden xterm is drawn)" 1
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
mark=$(log_mark)
warm_out=$("$ROOT/build/ws-direction" warm 2>&1)
warm_status=$?
if ((warm_status == 0)) && tail -n +"$((mark + 1))" "$LOG" |
	grep -q "Warm: asked, and every hidden window has an image"; then
	pass "  (asked again, it has nothing to do, and returns at once)"
else
	fail "  (asked again, it has nothing to do, and returns at once)" \
		"exit status $warm_status, output: $warm_out"
fi
"$ROOT/build/ws-direction" overview open || die "ws-direction overview open failed"
sleep "$SETTLE"
warm_out=$("$ROOT/build/ws-direction" warm 2>&1)
warm_status=$?
"$ROOT/build/ws-direction" overview close || die "ws-direction overview close failed"
sleep "$SETTLE"
if ((warm_status == 1)) && [[ $warm_out == *"overview"* ]]; then
	pass "  (not while the overview is open: $warm_out)"
else
	fail "  (not while the overview is open)" "exit status $warm_status, output: $warm_out"
fi

# "ws-direction warm --auto off" sets a root window property, without picom, and then
# the plugin doesn't get images back by itself after a config change; "on" again does.
auto_status=0
"$ROOT/build/ws-direction" warm --auto off || auto_status=$?
mark=$(log_mark)
: >>"$OUT/wave.conf"
sleep 3
if ((auto_status == 0)) && [[ $(xprop -root _WS_DIRECTION_WARM) == *'"off"'* ]] &&
	! tail -n +"$((mark + 1))" "$LOG" | grep -q "Warm: .* freezing"; then
	pass "warm --auto off: after a config change the plugin leaves the images"
else
	fail "warm --auto off: after a config change the plugin leaves the images" \
		"exit status $auto_status, $(xprop -root _WS_DIRECTION_WARM), see $LOG"
fi
wave_ws2 "  (screenshot: a hidden xterm is not drawn)" 0
i3cmd 'workspace 1:IDE'
sleep "$SETTLE"
"$ROOT/build/ws-direction" warm || die "ws-direction warm failed"
"$ROOT/build/ws-direction" warm --auto on || die "ws-direction warm --auto on failed"
sleep "$SETTLE"
before=$(shown_workspaces)
mark=$(log_mark)
: >>"$OUT/wave.conf"
warm_case "  (warm --auto on: the plugin does it by itself again)" "$mark" "$before"
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
PICOM_DEBUG="frame_timing=$OUT/frames-slow.csv;frame_layout=1" start_picom "$OUT/pass-eased.conf" picom-slow
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
check_frames "  (and every frame of these passes check-frames.py)" "$OUT/frames-slow.csv"
# showhide.conf animates a switch with show and hide, which picom starts itself: after the
# pause, with the plugin's variables. (Its frames are not checked here: a show and a hide
# that the window manager sends a frame apart are what the switch styles' ws-pass avoids.
# The styles' own frames are checked above.)
rm -f "$OUT/frames-slow-slide.csv"
PICOM_DEBUG="frame_timing=$OUT/frames-slow-slide.csv;frame_layout=1" start_picom "$CONFIG" picom-slow-slide
mark=$(log_mark)
fakewm grab current 1 sleep 20 map "$FB" unmap "$FA" sleep 80 ungrab
sleep "$SETTLE"
slow_read_case "a slow X server with show and hide: the plugin pauses the screen for its read" \
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

# The plugin declares ws-pass itself, so no config lists it, but showhide.conf has no
# animation for it. The plugin starts it on every switch, picom ignores those starts, and
# the shows and hides run as usual (checked by the cases of phase 1 and 2).
tried=$(grep -cE "windows, started ws-pass on [1-9][0-9]* \(in pass" "$OUT/picom-i3.log")
ignored=$(grep -c "Not starting animation ws-pass .* because it has no animation for it" \
	"$OUT/picom-i3.log")
started=$(grep -c "Starting animation ws-pass" "$OUT/picom-i3.log")
if ((tried > 0 && ignored > 0 && started == 0)); then
	pass "the plugin declares ws-pass, showhide.conf has no animation for it: picom ignores the starts ($tried switches, $ignored windows)"
else
	fail "the plugin declares ws-pass, showhide.conf has no animation for it: picom ignores the starts" \
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
