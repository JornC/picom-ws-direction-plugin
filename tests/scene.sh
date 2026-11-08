#!/usr/bin/env bash
# Runs a style for a look at it frame by frame, on the harness's two monitors: Xvfb with
# DPT 3440x1440 at +0+480 and HDT 1080x1920 at +3440+0, i3 with two or three tiled
# xterms on each of eight workspaces (1-4, 7 and 8 on DPT, 5 and 6 on HDT), in shades of
# the workspace's own color, and picom with the plugin and STYLE from styles/, slowed
# down. It shows every workspace once, so picom keeps their images, then makes the
# switches given. picom logs every frame: where it draws each window, and the values of
# its script (the frame timing log with the layout records), and the plugin its
# decisions (picom.log). check-frames.py checks the frames of the switches given (with
# --seams for slide, pass-slide, push, wipe and fold, whose workspaces tile the monitor),
# plots them (plot.png) and draws a strip of frames per switch (strip-N-WS.png).
#
# A switch is a workspace number, to go there once the switch before has ended, or
# WS@F, to go there F of LENGTH after the switch before started, which interrupts it;
# o (or o@F) opens or closes the overview of the focused monitor instead.
# The default goes 1->2->1, then 1->4 interrupted at 0.3 by 3 (the other way) and that at
# 0.7 by 1 (the same way), 1->2 interrupted at 0.3 by 3 (the same way) and that at 0.7 by
# 1 (the other way), and on HDT, which turns sideways, 5->6 interrupted at 0.3 and 0.7 by
# switches the other way: 2 1 4 3@0.3 1@0.7 2 3@0.3 1@0.7 6 5@0.3 6@0.7. With OVERVIEW
# it adds 2 o@0.5 o 1: the overview opened halfway into a switch, then closed.
#
# Usage: make && tests/scene.sh OUTDIR STYLE [SWITCH...]   (STYLE: a name from styles/,
# or the path of a style file)
# Environment: PICOM (a picom with PICOM_DEBUG=frame_layout; default: picom on the
# PATH), LENGTH (seconds, put in for var-length; default 1.6; "style" keeps the style's
# own lengths, to check it at the real length), OVERVIEW (an overview
# style for a second rule, a name from styles/ like wall or unfold, or the path of a
# style file; default none), BACKEND
# (default glx), PARTS (the directory to take the styles' parts from; default
# styles/parts), SCENE_DISPLAY (default :93), RECORD (1 to also record the screen with
# ffmpeg: video-N-WS.png is a strip of the recording per switch, and scene.gif all of
# it). It needs Xvfb, xrandr, xprop, i3, xterm and Python 3 with matplotlib.
# Xvfb's glx has no vsync and copies window contents once, so the recording shows the
# shapes and the timing, not how smooth it is on a real screen.
set -u
OUT=$1 STYLE=$2
shift 2
SWITCHES=("$@")
if ((!${#SWITCHES[@]})); then
	SWITCHES=(2 1 4 3@0.3 1@0.7 2 3@0.3 1@0.7 6 5@0.3 6@0.7)
	[[ -z ${OVERVIEW:-} ]] || SWITCHES+=(2 o@0.5 o 1)
fi
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PICOM=${PICOM:-$(command -v picom)}
PLUGIN=$ROOT/build/ws-direction.so
STYLE_FILE=$ROOT/styles/$STYLE.conf
[[ $STYLE != */* ]] || STYLE_FILE=$STYLE
PARTS=${PARTS:-$ROOT/styles/parts}
LENGTH=${LENGTH:-1.6}
# The length to put in for var-length, and the one to wait by.
if [[ $LENGTH == style ]]; then
	put_length=(-e "")
	LENGTH=$(sed -nE 's/.*var-length = ([0-9.]+);.*/\1/p' "$STYLE_FILE" | head -n 1)
else
	put_length=(-e "s/var-length = [0-9.]+;/var-length = $LENGTH;/")
fi
SEAMS=()
case $(basename "$STYLE_FILE" .conf) in
slide | pass-slide | push | wipe | fold) SEAMS=(--seams) ;;
esac
DISP=${SCENE_DISPLAY:-:93}
[[ $DISP != ":0" ]] || { echo "refusing to run on :0"; exit 2; }
[[ -f $PLUGIN ]] || { echo "run make first"; exit 2; }
[[ -f $STYLE_FILE ]] || { echo "no such style: $STYLE_FILE"; exit 2; }
[[ -n $PICOM && -x $PICOM ]] || { echo "picom not found, set PICOM"; exit 2; }
tools=(Xvfb xrandr xprop i3 i3-msg xterm python3)
[[ ${RECORD:-} != 1 ]] || tools+=(ffmpeg)
for tool in "${tools[@]}"; do
	command -v "$tool" >/dev/null || { echo "missing tool: $tool"; exit 2; }
done
python3 -c 'import matplotlib' 2>/dev/null || { echo "missing Python module: matplotlib"; exit 2; }
[[ ! -e /tmp/.X${DISP#:}-lock ]] || { echo "display $DISP in use"; exit 2; }
mkdir -p "$OUT/ws-direction/parts" "$XDG_RUNTIME_DIR/scene${DISP#:}"
# Only what an earlier run wrote: OUTDIR may hold other files.
rm -f "$OUT"/{xvfb,i3,picom,ffmpeg}.log "$OUT"/{i3,picom}.conf "$OUT"/picom.out \
	"$OUT"/plot.png "$OUT"/strip-*.png "$OUT"/video-*.png "$OUT"/scene.gif \
	"$OUT"/frames.csv "$OUT"/frames.csv.old "$OUT"/raw.mkv "$OUT"/from
# The styles include their parts relative to picom.conf's directory first.
cp "$PARTS"/*.conf "$OUT/ws-direction/parts/"
SOCK=$XDG_RUNTIME_DIR/scene${DISP#:}/ipc
MONITORS=(--monitor "0,480,3440,1440" --monitor "3440,0,1080,1920")
export DISPLAY=$DISP

PIDS=()
# The EXIT trap runs it; shellcheck 0.11 misses that when the script ends with exit.
# shellcheck disable=SC2329
cleanup() {
	local i
	for ((i = ${#PIDS[@]} - 1; i >= 0; i--)); do kill "${PIDS[i]}" 2>/dev/null; done
	wait 2>/dev/null
}
trap cleanup EXIT
bg() { local log=$1; shift; "$@" >"$log" 2>&1 & PIDS+=("$!"); LAST=$!; }
i3cmd() { i3-msg -s "$SOCK" "workspace $1" >/dev/null; }
monotonic() { python3 -c 'import time; print(time.monotonic())'; }

bg "$OUT/xvfb.log" Xvfb "$DISP" -screen 0 4520x1920x24 +extension GLX +extension RANDR -nolisten tcp -noreset
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done
xrandr --setmonitor DPT 3440/800x1440/337+0+480 screen >/dev/null
xrandr --setmonitor HDT 1080/300x1920/530+3440+0 none >/dev/null

{
	echo "ipc-socket $SOCK"
	echo 'font pango:monospace 8'
	echo 'focus_follows_mouse no'
	for ws in 1 2 3 4 7 8; do echo "workspace $ws output DPT"; done
	for ws in 5 6; do echo "workspace $ws output HDT"; done
	for ws in 1 2 3 4 5 6 7 8; do echo "assign [instance=\"^w${ws}[abc]\$\"] workspace $ws"; done
} >"$OUT/i3.conf"
# i3 puts its own socket in XDG_RUNTIME_DIR, whose path must stay short.
XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR/scene${DISP#:} bg "$OUT/i3.log" i3 -c "$OUT/i3.conf"
for _ in $(seq 50); do i3-msg -s "$SOCK" -t get_version >/dev/null 2>&1 && break; sleep 0.1; done
# Each workspace has two or three windows side by side, in shades of its color.
colors=(ff0000 0000ff 00ff00 ffff00 ff00ff 00ffff ffffff 808080)
shades=(a:ff b:b0 c:70)
for ws in 1 2 3 4 5 6 7 8; do
	for shade in "${shades[@]:0:$((2 + ws % 2))}"; do
		name=w$ws${shade%:*}
		color=$(sed -E "s/ff/${shade#*:}/g" <<<"${colors[ws - 1]}")
		bg /dev/null xterm -name "$name" -T "$name" -bg "#$color" -fg black -fa mono -fs 14 \
			-e sh -c "for i in \$(seq 200); do echo \"$name line \$i ####################\"; done; sleep 100000"
		sleep 0.1
	done
done
sleep 1.5

{
	echo "plugins = \"$PLUGIN\";"
	echo "backend = \"${BACKEND:-glx}\";"
	echo 'vsync = true;'
	echo 'unredir-if-possible = false;'
	echo 'rules = ({'
	echo "  match = \"!override_redirect && window_type = 'normal'\";"
	sed -E "${put_length[@]}" "$STYLE_FILE"
	if [[ -n ${OVERVIEW:-} ]]; then
		echo '}, {'
		echo "  match = \"!override_redirect && window_type = 'normal'\";"
		overview=$ROOT/styles/$OVERVIEW.conf
		[[ $OVERVIEW != */* ]] || overview=$OVERVIEW
		sed -E "${put_length[@]}" "$overview"
	fi
	echo '});'
} >"$OUT/picom.conf"
PICOM_DEBUG="frame_timing=$OUT/frames.csv;frame_layout=1" WS_DIRECTION_LOG=$OUT/picom.log \
	bg "$OUT/picom.out" "$PICOM" --config "$OUT/picom.conf" --log-level debug --log-file "$OUT/picom.log"
PICOM_PID=$LAST
for _ in $(seq 100); do grep -q "Screen redirected" "$OUT/picom.log" 2>/dev/null && break; sleep 0.1; done
sleep 1
for ws in 2 3 4 7 8 6 5 1; do
	i3cmd "$ws"
	sleep 0.4
done
sleep "$(awk -v l="$LENGTH" 'BEGIN { print l * 1.6 }')"

if [[ ${RECORD:-} == 1 ]]; then
	bg "$OUT/ffmpeg.log" ffmpeg -y -f x11grab -draw_mouse 0 -framerate 15 -video_size 4520x1920 \
		-i "$DISP.0+0,0" -vf scale=1130:480 -c:v libx264rgb -preset ultrafast -qp 0 "$OUT/raw.mkv"
	FF=$LAST
	sleep 1
fi

FROM=$(monotonic)
# For check-frames.py --from, to check the log again.
echo "$FROM" >"$OUT/from"
STARTS=() REAL=()
for spec in "${SWITCHES[@]}"; do
	ws=${spec%@*}
	if [[ $spec == *@* ]]; then
		sleep "$(awk -v l="$LENGTH" -v f="${spec#*@}" 'BEGIN { print l * f }')"
	elif ((${#STARTS[@]})); then
		sleep "$(awk -v l="$LENGTH" 'BEGIN { print l * 1.5 }')"
	fi
	STARTS+=("$(monotonic)")
	REAL+=("$(date +%s.%N)")
	if [[ $ws == o ]]; then
		"$ROOT/build/ws-direction" overview toggle
	else
		i3cmd "$ws"
	fi
done
sleep "$(awk -v l="$LENGTH" 'BEGIN { print l * 1.6 }')"

if [[ -n ${FF:-} ]]; then
	kill -INT "$FF"
	wait "$FF" 2>/dev/null
fi
kill -0 "$PICOM_PID" 2>/dev/null || { echo "picom stopped, see $OUT/picom.log"; exit 1; }
kill -INT "$PICOM_PID"
wait "$PICOM_PID" 2>/dev/null

"$HERE/check-frames.py" "$OUT/frames.csv" "${MONITORS[@]}" "${SEAMS[@]}" --from "$FROM" \
	--plot "$OUT/plot.png"
rc=$?
for i in "${!SWITCHES[@]}"; do
	"$HERE/check-frames.py" "$OUT/frames.csv" "${MONITORS[@]}" --from "${STARTS[i]}" \
		--strip "$OUT/strip-$((i + 1))-${SWITCHES[i]%@*}.png" --strip-length "$LENGTH" >/dev/null
done
if [[ -n ${FF:-} ]]; then
	# x11grab stamps frames with the wall clock; the input's start is the first one.
	start=$(sed -nE 's/.*Duration: N\/A, start: ([0-9.]+),.*/\1/p' "$OUT/ffmpeg.log" | head -n 1)
	for i in "${!SWITCHES[@]}"; do
		ffmpeg -v error -y -ss "$(awk -v a="${REAL[i]}" -v b="$start" 'BEGIN { print a - b }')" \
			-t "$LENGTH" -i "$OUT/raw.mkv" -vf "fps=8/$LENGTH,tile=4x2:padding=6" -frames:v 1 \
			"$OUT/video-$((i + 1))-${SWITCHES[i]%@*}.png"
	done
	ffmpeg -v error -y -i "$OUT/raw.mkv" -vf \
		"fps=15,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
		-loop 0 "$OUT/scene.gif"
fi
grep -E '(ERROR|WARN)' "$OUT/picom.log" | sed -E 's/^\[[^]]*\] //' | sort | uniq -c | head -n 10
ls "$OUT"/*.png "$OUT"/*.gif 2>/dev/null
exit "$rc"
