#!/data/data/com.termux/files/usr/bin/bash
# go2rtc media hub: pulls every camera once and re-serves it locally:
#   RTSP  :8554/<name>, live.mp4, HLS, WebRTC natively (go2rtc does the muxing)
#   API   :1984  (/api/webrtc, /api/mjpeg, /api/streams)
# Config ~/go2rtc/go2rtc.yaml is regenerated from ~/rtsp2rtmp/cameras.conf by
# tools/note9-hub/go2rtc-gen.py (run on the phone).
exec "$HOME/go2rtc/go2rtc" -config "$HOME/go2rtc/go2rtc.yaml"