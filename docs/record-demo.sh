#!/usr/bin/env bash
# Records docs/demo.gif on its own Xvfb: two monitors at 1/4 of a 3440x1440 + 1080x1920
# layout (860x360 landscape at +0+120, 270x480 portrait at +860+0), stock i3 with a bar,
# colored windows, and picom with the plugin and examples/picom-wsdir.conf (3440 scaled
# to 860). Two changes for the demo only: the slides are cropped below the 20 px bar, and
# picom uses the xrender backend, because glx on Xvfb doesn't redraw the bar.
# Usage: make && docs/record-demo.sh OUTDIR
# Environment: PICOM (default: picom on the PATH), DEMO_DISPLAY (default :205).
set -u
OUT=$1
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PICOM=${PICOM:-$(command -v picom)}
PLUGIN=$ROOT/build/ws-direction.so
EXAMPLE=$ROOT/examples/picom-wsdir.conf
DISP=${DEMO_DISPLAY:-:205}
SOCK=${XDG_RUNTIME_DIR:-/tmp}/ws-direction-demo.sock
W=1130 H=480
[[ $DISP != ":0" ]] || { echo "refusing to run on :0"; exit 2; }
[[ -f $PLUGIN ]] || { echo "run make first"; exit 2; }
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

# win INSTANCE BG LABEL
win() {
	bg "$OUT/xterm-$1.log" xterm -name "$1" -bg "$2" -fg '#f4f6fa' -fa 'DejaVu Sans Mono:bold' -fs 15 \
		+sb -b 14 -bw 0 -e sh -c "printf '\033[?25l\n%s' '$3'; exec sleep 100000"
	sleep 0.3
}
win w1a '#2f5d9e' '1:IDE  editor'
win w1b '#24497d' '1:IDE  build'
win w2a '#2f8a6f' '2:Web  browser'
win w2b '#246b56' '2:Web  docs'
win w3a '#8a4fa8' '3:Chat'
win w4a '#b5652b' '4:Term'
win w5a '#a83e5a' '5:Music'
win w6a '#6f7a2a' '6:Notes'
sleep 1
# Focus every window once, so no workspace stays marked urgent.
for ws in 2:Web 3:Chat 5:Music 6:Notes 4:Term 1:IDE; do
	i3cmd "workspace $ws"
	i3cmd 'focus left'
	sleep 0.2
	i3cmd 'focus right'
	sleep 0.2
done
sleep 0.5

# The i3 bar is 20 px high; crop below it, so slides don't cover it.
sed -e 's/3440/860/g' -e 's/crop-y = "workspace-monitor-y";/crop-y = "workspace-monitor-y + 20";/' \
	-e 's/crop-height = "workspace-monitor-height";/crop-height = "workspace-monitor-height - 20";/' \
	-e 's/^backend = "glx";/backend = "xrender";/' "$EXAMPLE" | { echo "plugins = \"$PLUGIN\";"; cat; } >"$OUT/picom.conf"
bg "$OUT/picom.out" "$PICOM" --config "$OUT/picom.conf" --log-file "$OUT/picom.log"
PICOM_PID=$LAST
sleep 1.5

bg "$OUT/ffmpeg.log" ffmpeg -y -f x11grab -draw_mouse 0 -framerate 50 -video_size ${W}x${H} \
	-i "$DISP.0+0,0" -c:v libx264rgb -preset ultrafast -qp 0 "$OUT/raw.mkv"
FF=$LAST
sleep 1.2

step() { i3cmd "workspace $1"; sleep 1.1; }
step 2:Web
step 3:Chat
step 1:IDE
step 5:Music
step 6:Notes
step 4:Term
step 2:Web
sleep 0.4

kill -INT "$FF"
wait "$FF" 2>/dev/null
kill -0 "$PICOM_PID" 2>/dev/null || { echo "picom stopped, see $OUT/picom.log"; exit 1; }
ffmpeg -v error -y -i "$OUT/raw.mkv" -vf \
	"fps=50,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
	-loop 0 "$OUT/demo.gif"
echo "wrote $OUT/demo.gif"
