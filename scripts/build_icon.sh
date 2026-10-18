#!/bin/sh
# Regenerate the icon PNG and seven-size ICO from the source artwork with FFmpeg.
# Center 832px artwork in a 1024px canvas with a 22%-of-artwork corner radius.
# The source artwork remains unchanged.
set -eu
icon_assets="$(CDPATH= cd -- "$(dirname -- "$0")/../src/assets" && pwd)"
ffmpeg -v error -y -i "$icon_assets/hull-viewer.png" -vf \
"scale=1664:1664:flags=lanczos,format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='if(lte(pow(max(abs(X-831.5)-465.42,0),2)+pow(max(abs(Y-831.5)-465.42,0),2),pow(366.08,2)),255,0)',pad=2048:2048:192:192:color=black@0,scale=1024:1024:flags=lanczos" \
-frames:v 1 -update 1 "$icon_assets/hull-viewer-icon.png"
ffmpeg -v error -y -i "$icon_assets/hull-viewer-icon.png" -filter_complex \
'split=7[a][b][c][d][e][f][g];[a]scale=16:16:flags=lanczos[a16];[b]scale=24:24:flags=lanczos[a24];[c]scale=32:32:flags=lanczos[a32];[d]scale=48:48:flags=lanczos[a48];[e]scale=64:64:flags=lanczos[a64];[f]scale=128:128:flags=lanczos[a128];[g]scale=256:256:flags=lanczos[a256]' \
-map '[a16]' -map '[a24]' -map '[a32]' -map '[a48]' -map '[a64]' -map '[a128]' -map '[a256]' \
-c:v bmp -pix_fmt bgra -frames:v 1 "$icon_assets/hull-viewer.ico"
