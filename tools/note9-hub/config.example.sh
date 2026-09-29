#!/data/data/com.termux/files/usr/bin/bash
## note9-hub configuration — copy to config.sh and edit.
## Run once on the phone:  bash setup.sh  (installs deps, creates config.sh)

CAMERA_RTSP_URL="rtsp://USER:PASS@192.168.1.68:554/live/ch00_0"

## YouTube relay: set empty to DISABLE the youtube pipeline (AI-only server).
YOUTUBE_RTMP_URL="rtmps://a.rtmp.youtube.com/live2/xxxx-xxxx-xxxx"

FFMPEG_BIN="ffmpeg"
FFMPEG_LOGLEVEL="warning"
RTSP_TRANSPORT="tcp"

## AI / motion pipeline.
AI_ENABLED="1"
AI_WIDTH="640"
AI_HEIGHT="360"
AI_FPS="2"
AI_MOTION_THRESHOLD="25"
AI_MIN_AREA="1500"
AI_EVENT_COOLDOWN="10"
AI_SNAPSHOT_DIR="$HOME/.note9-hub/snapshots"
## Optional TFLite object detection. If AI_TFLITE_MODEL is empty the pipeline
## runs motion-only. Termux aarch64 has no tflite-runtime wheel, so install the
## model package from a .deb or skip (motion-only is the working mode).
AI_TFLITE_MODEL=""
AI_LABELS=""
AI_CLASSES="person,car,truck,bus,motorcycle,bicycle"
AI_SCORE_THRESHOLD="0.40"

## `hub.sh serve` binds this port on the snapshots dir — reachable over
## Tailscale at http://<tailscale-ip>:<AI_HTTP_PORT>/
AI_HTTP_PORT="8080"