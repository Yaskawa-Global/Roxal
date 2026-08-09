#!/bin/sh
# Encode a doom.rox --capture dump into a constant-frame-rate MP4.
#
#   ./build-rel/roxal examples/doom/doom.rox --capture frames.raw
#   examples/doom/encode_capture.sh frames.raw doom.mp4
#
# Capture mode advances exactly one 35 Hz tic per frame and appends the frame
# as raw 320x200 rgb24, so how fast the renderer actually ran never reaches the
# video -- every frame is evenly spaced, and a slow machine only makes the play
# session take longer. This scales the dump up with nearest-neighbour (the
# FrameView presents unsmoothed, so the result matches what the window showed)
# and encodes it at a constant frame rate.
#
# The default 30 fps is one frame per captured tic, played 6/7 as fast: motion
# runs 17% slow, but 30 is a rate every player and transcoder handles natively.
# 35 fps is exact speed, at the risk that something re-encoding the file to 30
# drops one frame in seven and puts judder back in. Neither ever drops or
# duplicates a frame at capture time -- the choice is only speed vs. rate.
#
# Options:
#   -s N     integer upscale factor (default 4, i.e. 1280x960)
#   -f FPS   frame rate (default 30; -f 35 plays at true speed)
#   -a       add a silent audio track -- some players and embedders (Teams
#            previews, PowerPoint) expect one. Captures are always silent:
#            the game's sound runs in real time, the capture does not.
set -e

W=320
H=200
OH=240          # 320x200 is stored with non-square pixels; doom.qml presents it
                # at 4:3, so each scale step is 320x240, not 320x200
TICRATE=35
FPS=30
SCALE=4
AUDIO=0
RAW=
OUT=

while [ $# -gt 0 ]; do
    case "$1" in
        -s) SCALE=$2; shift 2 ;;
        -f) FPS=$2; shift 2 ;;
        -a) AUDIO=1; shift ;;
        -h|--help) awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)  if [ -z "$RAW" ]; then RAW=$1; elif [ -z "$OUT" ]; then OUT=$1; else
                echo "unexpected argument: $1" >&2; exit 2
            fi
            shift ;;
    esac
done

if [ -z "$RAW" ]; then
    echo "usage: $0 [-s SCALE] [-f FPS] [-a] <frames.raw> [out.mp4]" >&2
    exit 2
fi
[ -f "$RAW" ] || { echo "no such capture: $RAW" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }
[ -n "$OUT" ] || OUT=$(basename "${RAW%.*}").mp4

FRAMESZ=$((W * H * 3))
BYTES=$(wc -c < "$RAW")
FRAMES=$((BYTES / FRAMESZ))
[ "$FRAMES" -gt 0 ] || { echo "capture is empty" >&2; exit 1; }
if [ $((BYTES % FRAMESZ)) -ne 0 ]; then
    echo "warning: $RAW ends mid-frame ($((BYTES % FRAMESZ)) trailing bytes) -- encoding $FRAMES whole frames" >&2
fi

DS=$((FRAMES * 10 / FPS))               # duration in deciseconds
SPEED=$((FPS * 100 / TICRATE))          # hundredths of true speed
printf '%d frames -> %dx%d @ %s fps, %d.%ds at %d.%02dx speed -> %s\n' \
    "$FRAMES" "$((W * SCALE))" "$((OH * SCALE))" "$FPS" \
    "$((DS / 10))" "$((DS % 10))" "$((SPEED / 100))" "$((SPEED % 100))" "$OUT"

if [ "$AUDIO" -eq 1 ]; then
    exec ffmpeg -y -hide_banner -loglevel warning \
        -f rawvideo -pixel_format rgb24 -video_size ${W}x${H} -framerate "$FPS" -i "$RAW" \
        -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
        -vf "scale=$((W * SCALE)):$((OH * SCALE)):flags=neighbor" \
        -frames:v "$FRAMES" -c:v libx264 -crf 18 -preset medium -pix_fmt yuv420p \
        -c:a aac -b:a 96k -shortest -movflags +faststart "$OUT"
fi

exec ffmpeg -y -hide_banner -loglevel warning \
    -f rawvideo -pixel_format rgb24 -video_size ${W}x${H} -framerate "$FPS" -i "$RAW" \
    -vf "scale=$((W * SCALE)):$((OH * SCALE)):flags=neighbor" \
    -frames:v "$FRAMES" -c:v libx264 -crf 18 -preset medium -pix_fmt yuv420p \
    -movflags +faststart "$OUT"
