#!/usr/bin/env bash
# Records every style with record-demo.sh and makes docs/slide.gif and docs/styles.gif:
# the slide style on its own, and all styles at once in a 3x2 grid, each with its name.
# Usage: make && docs/record-styles.sh WORKDIR
set -eu
WORK=$1
HERE=$(cd "$(dirname "$0")" && pwd)
STYLES=(slide push wipe fold cascade warp)
FONT=$(fc-match -f '%{file}' 'DejaVu Sans Mono:bold')
for s in "${STYLES[@]}"; do
	"$HERE/record-demo.sh" "$WORK/$s" "$s"
done
cp "$WORK/slide/demo.gif" "$HERE/slide.gif"
inputs=() tiles= names=
for i in "${!STYLES[@]}"; do
	inputs+=(-i "$WORK/${STYLES[i]}/raw.mkv")
	tiles+="[$i]scale=376:160:flags=lanczos,drawtext=fontfile=$FONT:text=${STYLES[i]}:x=10:y=10:fontsize=17:fontcolor=white[v$i];"
	names+="[v$i]"
done
ffmpeg -v error -y "${inputs[@]}" -filter_complex "$tiles${names}xstack=inputs=6:layout=0_0|w0_0|w0+w1_0|0_h0|w0_h0|w0+w1_h0:shortest=1:fill=black,fps=25,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
	-loop 0 "$HERE/styles.gif"
echo "wrote $HERE/slide.gif and $HERE/styles.gif"
