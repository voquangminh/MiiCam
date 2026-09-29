#!/data/data/com.termux/files/usr/bin/bash
## One-shot Termux setup for note9-hub. Run on the phone:
##   pkg update && bash setup.sh
set -e
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

echo "==> Installing Termux packages (ffmpeg, python, numpy, pillow, api)..."
pkg install -y ffmpeg python python-numpy python-pillow termux-api

echo "==> Making scripts executable..."
chmod +x "$HERE/hub.sh" "$HERE/detect.py" "$HERE/pipelines/ai.sh" "$HERE/pipelines/youtube.sh" "$HERE/pipelines/go2rtc.sh" "$HERE/go2rtc-gen.py"

if [ ! -x "$HOME/go2rtc/go2rtc" ]; then
    echo "==> Installing go2rtc (media hub, linux/arm64)..."
    mkdir -p "$HOME/go2rtc"
    curl -sSL -o "$HOME/go2rtc/go2rtc" https://github.com/AlexxIT/go2rtc/releases/latest/download/go2rtc_linux_arm64
    chmod +x "$HOME/go2rtc/go2rtc"
fi

if [ ! -f "$HERE/config.sh" ]; then
    cp "$HERE/config.example.sh" "$HERE/config.sh"
    echo "==> Created $HERE/config.sh — EDIT IT (CAMERA_RTSP_URL at minimum)."
else
    echo "==> config.sh already present, leaving it untouched."
fi

mkdir -p "$HOME/.note9-hub/run" "$HOME/.note9-hub/log"
echo "==> Done."
echo "    1. edit $HERE/config.sh"
echo "    2. bash $HERE/hub.sh start         (go2rtc media hub + RTSP->YouTube relay + AI pipeline)"
echo "    3. bash $HERE/hub.sh serve         (HTTP snapshot viewer, port AI_HTTP_PORT)"
echo "    4. bash $HERE/hub.sh status|logs   (supervise)"
echo "    5. python3 $HERE/go2rtc-gen.py     (rebuild ~/go2rtc/go2rtc.yaml + cameras.go2rtc.conf from ~/rtsp2rtmp/cameras.conf)"