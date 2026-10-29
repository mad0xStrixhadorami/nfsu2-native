#!/bin/bash
# Record the native game window to a GIF (README media).
# usage: tools/record_gif.sh out.gif <start-delay-s> <duration-s> [width] [fps]   (game env vars come from the caller)
# The game starts with the caller's NFSU2_* environment; capture begins <start-delay> seconds after launch.
set -e
out=$1; delay=$2; dur=$3; width=${4:-512}; fps=${5:-12}
export DISPLAY=${DISPLAY:-:0.0} SDL_AUDIODRIVER=dummy NFSU2_DATA=${NFSU2_DATA:-$HOME/re/nfsu2_game}
tmp=$(mktemp -d)
"$(dirname "$0")/../build/nfsu2" >"$tmp/game.log" 2>&1 &
game=$!
for _ in $(seq 1 80); do xwininfo -name "Need for Speed Underground 2 (native)" >/dev/null 2>&1 && break; sleep 0.25; done # wait for the window
sleep "$delay"
info=$(xwininfo -name "Need for Speed Underground 2 (native)")
X=$(echo "$info" | awk '/Absolute upper-left X/{print $4}'); Y=$(echo "$info" | awk '/Absolute upper-left Y/{print $4}')
WIDTH=$(echo "$info" | awk '/Width:/{print $2}'); HEIGHT=$(echo "$info" | awk '/Height:/{print $2}')
ffmpeg -loglevel error -y -f x11grab -draw_mouse 0 -framerate "$fps" -video_size "${WIDTH}x${HEIGHT}" -i "$DISPLAY+$X,$Y" -t "$dur" "$tmp/cap.mkv"
kill $game 2>/dev/null || true
ffmpeg -loglevel error -y -i "$tmp/cap.mkv" -vf "fps=$fps,scale=$width:-1:flags=lanczos,palettegen=max_colors=48:stats_mode=diff" "$tmp/pal.png"
ffmpeg -loglevel error -y -i "$tmp/cap.mkv" -i "$tmp/pal.png" -lavfi "fps=$fps,scale=$width:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=2:diff_mode=rectangle" "$out"
[ -n "$KEEP_RAW" ] && cp "$tmp/cap.mkv" "${out%.gif}.mkv"
rm -rf "$tmp"
ls -la "$out"
