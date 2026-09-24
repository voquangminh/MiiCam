#!/data/data/com.termux/files/usr/bin/bash
set -u

HERE="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$HERE/config.sh"

trap 'pkill -TERM -P $$ 2>/dev/null; exit 0' TERM INT

retry=3
while true; do
    echo "[youtube] start $(date '+%F %T')"
    "$FFMPEG_BIN" -hide_banner -loglevel "$FFMPEG_LOGLEVEL" \
        -rtsp_transport "$RTSP_TRANSPORT" \
        -i "$CAMERA_RTSP_URL" \
        -map 0:v -c copy -an -f flv "$YOUTUBE_RTMP_URL"
    rc=$?
    echo "[youtube] ffmpeg exit=$rc, retry in ${retry}s"
    sleep "$retry"
done
