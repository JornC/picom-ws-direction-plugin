#!/usr/bin/env bash
# Records every style with record-demo.sh and makes the GIFs in docs/: slide.gif, the
# slide style on its own; styles.gif, all styles at once in a grid of COLS columns, each
# with its name; and styles-slow.gif, the same grid slowed down to 1.2 s per switch, going
# to the next workspace and back.
# Usage: make && docs/record-styles.sh WORKDIR
set -eu
WORK=$1
HERE=$(cd "$(dirname "$0")" && pwd)
STYLES=(slide push wipe fold cascade warp zoom-pan)
COLS=4
FONT=$(fc-match -f '%{file}' 'DejaVu Sans Mono:bold')

# grid DIR GIF: the recordings in DIR/<style>/raw.mkv, side by side, as GIF.
grid() {
	local inputs=() tiles= names= layout= i
	for i in "${!STYLES[@]}"; do
		inputs+=(-i "$1/${STYLES[i]}/raw.mkv")
		tiles+="[$i]scale=376:160:flags=lanczos,drawtext=fontfile=$FONT:text=${STYLES[i]}:x=10:y=10:fontsize=17:fontcolor=white[v$i];"
		names+="[v$i]"
		layout+="${layout:+|}$((i % COLS * 376))_$((i / COLS * 160))"
	done
	ffmpeg -v error -y "${inputs[@]}" -filter_complex "$tiles${names}xstack=inputs=${#STYLES[@]}:layout=$layout:shortest=1:fill=black,fps=25,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
		-loop 0 "$2"
}

for s in "${STYLES[@]}"; do
	# A bug in picom's xrender backend makes cascade flicker for a frame with use-damage on;
	# see the README.
	damage=
	[[ $s != cascade ]] || damage=false
	DEMO_USE_DAMAGE=$damage "$HERE/record-demo.sh" "$WORK/normal/$s" "$s"
	DEMO_USE_DAMAGE=$damage DEMO_LENGTH=1.2 "$HERE/record-demo.sh" "$WORK/slow/$s" "$s"
done
cp "$WORK/normal/slide/demo.gif" "$HERE/slide.gif"
grid "$WORK/normal" "$HERE/styles.gif"
grid "$WORK/slow" "$HERE/styles-slow.gif"
echo "wrote slide.gif, styles.gif and styles-slow.gif in $HERE"
