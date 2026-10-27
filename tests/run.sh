#!/usr/bin/env bash
# Test harness for the ws-direction picom plugin, and so for the picom hook it builds on
# (animation variables, plugin API 0.2).
#
# Phase 1: a nested Xvfb with two monitors (DPT 3440x1440 at +0+480, HDT 1080x1920 at
# +3440+0), stock i3, xterms on several workspaces, and picom with the plugin,
# styles/slide.conf and debug logging. It runs i3 commands through i3-msg and checks
# the workspace-direction that the plugin logs for every window that shows, opens or
# hides. It also takes screenshots in the middle of slowed-down slides, to check the
# slide style slides the right way and stays on its monitor. Then it runs picom with
# tests/picom-anim.conf, whose scripts don't read the variables, and checks the plugin
# is loaded but never asked (the hook calls a provider only for scripts that read its
# variable), and with a config without "open" animations to check the startup seed.
#
# Phase 2: i3 is replaced by fakewm.c, a tiny stand-in window manager. It can spread one
# workspace switch over several picom frames, and set up cases i3 won't produce on demand
# (no _NET_DESKTOP_VIEWPORT, sticky windows, a window on the other monitor showing during
# a switch).
#
# Prints PASS/FAIL per case and exits 1 if any case failed. Everything runs on
# $WSDIR_DISPLAY (default :91), never on the real display, and only processes started
# here are stopped.
#
# Run it alone, not next to builds: the fakewm case with 40 ms gaps can run past the
# plugin's 100 ms switch lifetime on a loaded machine or under a sanitizer build. Rerun
# it alone before treating that failure as a bug.
#
# Usage: make && tests/run.sh
# Environment: PICOM (picom binary, default: picom on the PATH; it must have the
# animation variable hook), I3 (default /usr/bin/i3), WSDIR_DISPLAY (default
# :91), WSDIR_WORK (directory for the i3 socket and logs, default
# $XDG_RUNTIME_DIR/wsdir-<display number>), WSDIR_SETTLE (seconds to wait after each
# step, default 0.7), WSDIR_PLUGIN (the plugin, default build/ws-direction.so).

set -u

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
for tool in "$I3" Xvfb xrandr i3-msg xterm xdotool xprop import magick cc pkg-config; do
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
	run_bg "$OUT/$2.out" "$PICOM" --config "$1" --log-level debug --log-file "$LOG"
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
		sed -nE 's/.*\] (open|show|hide) of window 0x[0-9a-f]+ \((.*)\): workspace-direction = (-?[0-9]+) \(.*/\2 \1 \3/p' |
		sed -E 's/ open / show /' | sort -u
}

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
		fail "$name" "expected: $(echo ${want:-nothing} | tr '\n' ' ')" \
			"got:      $(echo ${GOT:-nothing} | tr '\n' ' ')"
	fi
}

# i3_case NAME I3-COMMAND EXPECTATION...
i3_case() {
	local name=$1 cmd=$2 mark
	shift 2
	mark=$(log_mark)
	i3cmd "$cmd"
	sleep "$SETTLE"
	GOT=$(directions_since "$mark")
	expect "$name" "$@"
}

# fake_case NAME "FAKEWM STEPS" EXPECTATION...
fake_case() {
	local name=$1 steps=$2 mark
	shift 2
	mark=$(log_mark)
	# shellcheck disable=SC2086
	fakewm $steps
	sleep "$SETTLE"
	GOT=$(directions_since "$mark")
	expect "$name" "$@"
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

cc -O2 -Wall -o "$OUT/fakewm" "$HERE/fakewm.c" $(pkg-config --cflags --libs xcb) ||
	die "cannot build fakewm"

if "$PICOM" --config "$CONFIG" --diagnostics >"$OUT/diagnostics.out" 2>&1 &&
	grep -q "Config file used: $CONFIG" "$OUT/diagnostics.out" &&
	! grep -qiE "error.*(config|script|parse|line)" "$OUT/diagnostics.out"; then
	pass "slide style loads (picom --diagnostics)"
else
	fail "slide style loads (picom --diagnostics)" "see $OUT/diagnostics.out"
fi
if grep -q "picom is too old" "$OUT/diagnostics.out"; then
	die "this picom has no animation variable hook (plugin API 0.2), see $OUT/diagnostics.out"
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

start_picom "$CONFIG" picom-i3

i3_case "HDT 5->3 while the focus is on DPT, first switch after picom started" \
	'workspace 3:Term' "ws5-a hide -1" "ws3-a show -1"
i3_case "focus moves to the other monitor: no animation" \
	'focus output DPT' none
i3_case "DPT 1->2" 'workspace 2:Web' "ws1-a hide 1" "ws1-b hide 1" "ws2-a show 1"
i3_case "DPT 2->1" 'workspace 1:IDE' "ws2-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "DPT 1->4 (no wrap-around guessing)" 'workspace 4:Misc' \
	"ws1-a hide 1" "ws1-b hide 1" "ws4-a show 1"
i3_case "DPT 4->1" 'workspace 1:IDE' "ws4-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "HDT 3->5" 'workspace 5:Music' "ws3-a hide 1" "ws5-a show 1"
i3_case "HDT 5->3" 'workspace 3:Term' "ws5-a hide -1" "ws3-a show -1"
i3_case "focus moves to the other monitor by switching to its visible workspace" \
	'workspace 1:IDE' none
i3_case "DPT 1->7" 'workspace 7:Extra' "ws1-a hide 1" "ws1-b hide 1" "ws7-a show 1"
i3_case "HDT 3->5 while the focus is on DPT showing 7" \
	'workspace 5:Music' "ws3-a hide 1" "ws5-a show 1"
i3_case "DPT 7->1 while the focus is on HDT" \
	'workspace 1:IDE' "ws7-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "HDT 5->3 while the focus is on DPT showing 1" \
	'workspace 3:Term' "ws5-a hide -1" "ws3-a show -1"
i3_case "focus back to DPT: no animation" 'focus output DPT' none
i3_case "DPT 1->8, an empty workspace: the outgoing windows get the sign" \
	'workspace 8' "ws1-a hide 1" "ws1-b hide 1"
i3_case "DPT 8->1, the empty workspace is removed" \
	'workspace 1:IDE' "ws1-a show -1" "ws1-b show -1"
i3_case "DPT 1->6, an empty workspace" 'workspace 6' "ws1-a hide 1" "ws1-b hide 1"
before=$(current_desktop)
i3_case "DPT 6->7: 6 is removed, so only _NET_DESKTOP_NAMES changes" \
	'workspace 7:Extra' "ws7-a show 1"
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
i3_case "DPT 4->1" 'workspace 1:IDE' "ws4-a hide -1" "ws1-a show -1" "ws1-b show -1"
i3_case "move a window to another workspace: 0" \
	'[title="^ws1-b$"] move container to workspace 4:Misc' "ws1-b hide 0"
i3_case "move it back to the visible workspace: 0" \
	'[title="^ws1-b$"] move container to workspace 1:IDE' "ws1-b show 0"
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

i3_case "DPT 1->Notes: names without a number sort after numbered ones" \
	'workspace Notes' "ws1-a hide 1" "ws1-b hide 1" "notes-a show 1"
i3_case "DPT Notes->Alpha: Alpha comes later in _NET_DESKTOP_NAMES" \
	'workspace Alpha' "notes-a hide 1" "alpha-a show 1"
i3_case "DPT Alpha->Notes" 'workspace Notes' "alpha-a hide -1" "notes-a show -1"
i3_case "DPT Notes->7" 'workspace 7:Extra' "notes-a hide -1" "ws7-a show -1"
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
		fail "$name" "got: $(echo $GOT)"
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
sed -E -e 's/cubic-bezier\([^)]*\)/linear/' -e 's/"[0-9.]+ \* var-slide/"6 * var-slide/' \
	-e 's/\+ [0-9.]+ \* \(1 - var-slide\)/+ 3 * (1 - var-slide)/' \
	"$CONFIG" >"$OUT/slow.conf"
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
decisions=$(grep -c ": workspace-direction = " "$LOG")
if grep -qF "$PLUGIN" "/proc/$PICOM_PID/maps" &&
	grep -q 'Workspace ".*" is shown' "$LOG" &&
	! grep -qE "ws-direction:|Cannot add animation variable" "$OUT/picom-plain.out" &&
	((starts > 0 && decisions == 0)); then
	pass "config without workspace-direction: animations run, plugin is never asked ($starts starts, $decisions decisions)"
else
	fail "config without workspace-direction: animations run, plugin is never asked" \
		"$starts starts, $decisions decisions; see $LOG and $OUT/picom-plain.out"
fi
stop_picom

# Without an "open" animation picom doesn't look at the windows already on screen when
# it starts, so here only the seed (from mapped windows) can know that HDT shows 3.
# DPT shows 7 and has the focus: 7->5 would be -1, but on HDT it is 3->5, +1.
sed 's/triggers = \["show", "open"\];/triggers = ["show"];/' "$CONFIG" >"$OUT/no-open.conf"
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
# Desktops 1:A and 2:B are on DPT, 3:C and 4:D on HDT. fs is on all desktops.
for spec in fa:+100+600 fb:+100+1000 fb2:+800+1000 fc:+3600+100 fc2:+3600+500 \
	fd:+3600+900 fs:+1500+600; do
	start_xterm fake "${spec%%:*}" -geometry "40x10${spec#*:}"
done
FA=$(win_id fa) FB=$(win_id fb) FB2=$(win_id fb2) FC=$(win_id fc) FC2=$(win_id fc2)
FD=$(win_id fd) FS=$(win_id fs)
# DPT shows 1:A (fa, and the sticky fs), HDT shows 3:C (fc).
fakewm names 1:A,2:B,3:C,4:D viewport 0,480,0,480,3440,0,3440,0 current 0 \
	desktop "$FA" 0 desktop "$FB" 1 desktop "$FB2" 1 desktop "$FC" 2 desktop "$FC2" 2 \
	desktop "$FD" 3 desktop "$FS" sticky unmap "$FB" unmap "$FB2" unmap "$FC2" unmap "$FD"

start_picom "$CONFIG" picom-fake
mark=$(log_mark)
fake_case "DPT 1:A->2:B as root change, map, unmap 40 ms apart; an unrelated window shows 300 ms later" \
	"current 1 sleep 40 map $FB sleep 40 unmap $FA sleep 300 map $FB2" \
	"fb show 1" "fa hide 1" "fb2 show 0"
t_switch=$(log_time_ms "$mark" 'Workspace switch on monitor')
# The decision lines, not picom's own lines about these windows.
t_show=$(log_time_ms "$mark" 'of window 0x[0-9a-f]+ \(fb\): workspace-direction = ')
t_hide=$(log_time_ms "$mark" 'of window 0x[0-9a-f]+ \(fa\): workspace-direction = ')
if [[ -n $t_switch && -n $t_show && -n $t_hide ]] && ((t_hide - t_show >= 20)); then
	pass "  (those steps were handled in different frames: +$((t_show - t_switch)) ms, +$((t_hide - t_show)) ms)"
else
	fail "  (those steps should be handled in different frames)" \
		"switch $t_switch, show $t_show, hide $t_hide (ms)"
fi
fake_case "DPT 2:B->1:A while a window on HDT (no switch there) shows: it gets 0" \
	"current 0 sleep 30 map $FA sleep 30 unmap $FB unmap $FB2 map $FC2" \
	"fa show -1" "fb hide -1" "fb2 hide -1" "fc2 show 0"
fake_case "a sticky window that hides during a switch gets 0" \
	"current 1 map $FB unmap $FA unmap $FS" "fb show 1" "fa hide 1" "fs hide 0"
fake_case "DPT 2:B->1:A with the map 40 ms before the root change, and the unmap 40 ms after" \
	"map $FA sleep 40 current 0 sleep 40 unmap $FB" "fa show -1" "fb hide -1"
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

# Every script in the other configs reads the variables, so picom must have asked
# the plugin at every animation start, and never otherwise. The plugin works out
# all five variables at the first call for a window in a frame: one decision line
# per start.
counts=
bad=0
for log in "$OUT"/picom-*.log; do
	[[ $log != "$OUT/picom-plain.log" ]] || continue
	starts=$(grep -c "Starting animation" "$log")
	decisions=$(grep -c ": workspace-direction = " "$log")
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
