#!/data/data/com.termux/files/usr/bin/bash
set -u

HERE="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$HERE/config.sh"

export AI_WIDTH AI_HEIGHT AI_FPS AI_MOTION_THRESHOLD AI_MIN_AREA
export AI_EVENT_COOLDOWN AI_SNAPSHOT_DIR AI_TFLITE_MODEL AI_LABELS
export AI_CLASSES AI_SCORE_THRESHOLD

mkdir -p "$AI_SNAPSHOT_DIR"
trap 'pkill -TERM -P $$ 2>/dev/null; exit 0' TERM INT

retry=3
while true; do
    echo "[ai] start $(date '+%F %T')"
    "$FFMPEG_BIN" -hide_banner -loglevel "$FFMPEG_LOGLEVEL" \
        -rtsp_transport "$RTSP_TRANSPORT" \
        -i "$CAMERA_RTSP_URL" \
        -vf "fps=$AI_FPS,scale=$AI_WIDTH:$AI_HEIGHT" \
        -f rawvideo -pix_fmt rgb24 -an - | python3 "$HERE/detect.py"
    echo "[ai] pipeline exit, retry in ${retry}s"
    sleep "$retry"
done
