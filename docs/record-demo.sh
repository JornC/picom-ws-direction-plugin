#!/usr/bin/env bash
# Records a demo GIF on its own Xvfb: two monitors at 1/4 of a 3440x1440 + 1080x1920
# layout (860x360 landscape at +0+120, 270x480 portrait at +860+0), stock i3 with a bar,
# colored windows, and picom with the plugin and a style from styles/ in a rule for normal
# windows. Two changes for the demo only: windows are cropped below the 20 px bar, and
# picom uses the xrender backend, because glx on Xvfb doesn't redraw the bar.
# Usage: make && docs/record-demo.sh OUTDIR [STYLE]   (slide, push, wipe, fold, cascade or
# warp; default slide)
# Environment: PICOM (default: picom on the PATH), DEMO_DISPLAY (default :205),
# DEMO_LENGTH (seconds per switch instead of the style's var-length; it also makes the
# demo two switches, forward and back), DEMO_USE_DAMAGE (set to false for
# use-damage = false; default: picom's default).
set -u
OUT=$1
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PICOM=${PICOM:-$(command -v picom)}
PLUGIN=$ROOT/build/ws-direction.so
STYLE_FILE=$ROOT/styles/${2:-slide}.conf
LENGTH=${DEMO_LENGTH:-}
DISP=${DEMO_DISPLAY:-:205}
SOCK=${XDG_RUNTIME_DIR:-/tmp}/ws-direction-demo.sock
W=1130 H=480
[[ $DISP != ":0" ]] || { echo "refusing to run on :0"; exit 2; }
[[ -f $PLUGIN ]] || { echo "run make first"; exit 2; }
[[ -f $STYLE_FILE ]] || { echo "no such style: $STYLE_FILE"; exit 2; }
[[ ! -e /tmp/.X${DISP#:}-lock ]] || { echo "display $DISP in use"; exit 2; }
mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/raw.mkv "$SOCK"
export DISPLAY=$DISP

PIDS=()
cleanup() {
	local i
	for ((i = ${#PIDS[@]} - 1; i >= 0; i--)); do kill "${PIDS[i]}" 2>/dev/null; done
	sleep 0.5
	for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
	wait 2>/dev/null
}
trap cleanup EXIT
bg() { local log=$1; shift; "$@" >"$log" 2>&1 & PIDS+=("$!"); LAST=$!; }
i3cmd() { i3-msg -s "$SOCK" "$@" >/dev/null; }

bg "$OUT/xvfb.log" Xvfb "$DISP" -screen 0 ${W}x${H}x24 -br +extension GLX +extension RANDR -nolisten tcp -noreset
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done
xrandr --setmonitor DPT 860/200x360/84+0+120 screen >/dev/null
xrandr --setmonitor HDT 270/75x480/133+860+0 none >/dev/null

cat >"$OUT/i3.conf" <<EOF
ipc-socket $SOCK
font pango:DejaVu Sans 8
focus_follows_mouse no
mouse_warping none
focus_on_window_activation none
default_border pixel 0
gaps inner 6
workspace "1:IDE" output DPT
workspace "2:Web" output DPT
workspace "3:Chat" output DPT
workspace "4:Term" output HDT
workspace "5:Music" output HDT
workspace "6:Notes" output HDT
assign [instance="^w1"] workspace "1:IDE"
assign [instance="^w2"] workspace "2:Web"
assign [instance="^w3"] workspace "3:Chat"
assign [instance="^w4"] workspace "4:Term"
assign [instance="^w5"] workspace "5:Music"
assign [instance="^w6"] workspace "6:Notes"
bar {
	position top
	font pango:DejaVu Sans 8
	workspace_min_width 28
	colors {
		background #15171c
		focused_workspace #3d7bd9 #3d7bd9 #ffffff
		active_workspace #2b2f38 #2b2f38 #ffffff
		inactive_workspace #15171c #15171c #8a909c
	}
}
EOF
bg "$OUT/i3.log" i3 -c "$OUT/i3.conf"
for _ in $(seq 50); do i3-msg -s "$SOCK" -t get_version >/dev/null 2>&1 && break; sleep 0.1; done

# win INSTANCE BG LABEL FILE LINE: a terminal with a heading and text from this repo, so
# that moving and scaling show. It redraws when i3 resizes it, so the heading stays on top.
win() {
	bg "$OUT/xterm-$1.log" env L="$3" F="$4" N="$5" xterm -name "$1" -bg "$2" -fg '#eef1f6' \
		-fa 'DejaVu Sans Mono' -fs 6 +sb -b 8 -bw 0 -e bash -c '
			last=
			while :; do
				size=$(stty size)
				if [[ $size != "$last" ]]; then
					last=$size
					clear
					printf "\033[?25l\033[1m%s\033[0m\n\n" "$L"
					tail -n +"$N" "$F" | expand -t 4 | cut -c1-$((${size#* } - 1)) |
						head -n $((${size% *} - 3))
				fi
				sleep 0.2
			done'
	sleep 0.3
}
# 1:IDE: three columns.
win w1a '#2f5d9e' '1:IDE  editor' "$ROOT/ws_direction.c" 200
win w1b '#29528c' '1:IDE  build' "$ROOT/tests/run.sh" 250
win w1c '#22467a' '1:IDE  notes' "$ROOT/README.md" 40
# 2:Web: a 2x2 grid.
win w2a '#2f8a6f' '2:Web  docs' "$ROOT/README.md" 1
win w2b '#287a62' '2:Web  issues' "$ROOT/styles/fold.conf" 1
i3cmd '[instance="^w2a$"] focus'
i3cmd 'split v'
win w2c '#226a55' '2:Web  search' "$ROOT/LICENSE" 1
i3cmd '[instance="^w2b$"] focus'
i3cmd 'split v'
win w2d '#1c5b49' '2:Web  mail' "$ROOT/docs/record-demo.sh" 1
# 3:Chat: one wide window, two stacked on the right.
win w3a '#8a4fa8' '3:Chat  general' "$ROOT/ws_direction.c" 40
win w3b '#7a4595' '3:Chat  people' "$ROOT/Makefile" 1
i3cmd '[instance="^w3b$"] focus'
i3cmd 'split v'
win w3c '#6b3c83' '3:Chat  files' "$ROOT/styles/slide.conf" 1
# The portrait monitor stacks windows by itself.
win w4a '#b5652b' '4:Term  shell' "$ROOT/tests/run.sh" 40
win w4b '#9e5824' '4:Term  logs' "$ROOT/tests/fakewm.c" 1
win w5a '#a83e5a' '5:Music  queue' "$ROOT/README.md" 80
win w5b '#94364f' '5:Music  player' "$ROOT/styles/warp.conf" 1
win w5c '#802e44' '5:Music  lyrics' "$ROOT/LICENSE" 60
win w6a '#6f7a2a' '6:Notes' "$ROOT/README.md" 120
sleep 1
# Focus every window once, so no workspace stays marked urgent.
for w in w2a w2b w2c w2d w3a w3b w3c w5a w5b w5c w6a w4a w4b w1a w1b w1c; do
	i3cmd "[instance=\"^$w\$\"] focus"
	sleep 0.15
done
i3cmd 'workspace 4:Term'
i3cmd 'workspace 1:IDE'
sleep 0.5

SET_LENGTH=()
[[ -z $LENGTH ]] || SET_LENGTH=(-e "s/var-length = [0-9.]*;/var-length = $LENGTH;/")
# The i3 bar is 20 px high; crop below it, so windows don't cover it.
{
	echo "plugins = \"$PLUGIN\";"
	echo 'backend = "xrender";'
	echo 'unredir-if-possible = false;'
	[[ ${DEMO_USE_DAMAGE:-} != false ]] || echo 'use-damage = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	sed -e 's/crop-y = "plugin-workspace-monitor-y";/crop-y = "plugin-workspace-monitor-y + 20";/' \
		-e 's/crop-height = "plugin-workspace-monitor-height";/crop-height = "plugin-workspace-monitor-height - 20";/' \
		"${SET_LENGTH[@]}" "$STYLE_FILE"
	echo '});'
} >"$OUT/picom.conf"
bg "$OUT/picom.out" "$PICOM" --config "$OUT/picom.conf" --log-file "$OUT/picom.log"
PICOM_PID=$LAST
sleep 1.5

bg "$OUT/ffmpeg.log" ffmpeg -y -f x11grab -draw_mouse 0 -framerate 50 -video_size ${W}x${H} \
	-i "$DISP.0+0,0" -c:v libx264rgb -preset ultrafast -qp 0 "$OUT/raw.mkv"
FF=$LAST
sleep 0.5

# Both sequences end on the workspaces they start on (1:IDE and 4:Term), so the GIF loops
# without a jump.
if [[ -n $LENGTH ]]; then
	STEPS=(2:Web 1:IDE)
	PAUSE=$(awk -v l="$LENGTH" 'BEGIN { print l + 0.8 }')
else
	STEPS=(2:Web 3:Chat 1:IDE 5:Music 6:Notes 4:Term)
	PAUSE=1.1
fi
for ws in "${STEPS[@]}"; do
	i3cmd "workspace $ws"
	sleep "$PAUSE"
done
# Back to the focus of the start; 1:IDE is already shown, so nothing animates.
i3cmd 'workspace 1:IDE'
sleep 0.4

kill -INT "$FF"
wait "$FF" 2>/dev/null
kill -0 "$PICOM_PID" 2>/dev/null || { echo "picom stopped, see $OUT/picom.log"; exit 1; }
ffmpeg -v error -y -i "$OUT/raw.mkv" -vf \
	"fps=25,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
	-loop 0 "$OUT/demo.gif"
echo "wrote $OUT/demo.gif"
