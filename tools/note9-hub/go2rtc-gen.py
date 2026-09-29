#!/usr/bin/env python3
"""Generate the Note9 media-hub config from the rtsp2rtmp camera list.

Reads  ~/rtsp2rtmp/cameras.conf   (NAME|RTSP_URL|RTMP_YOUTUBE_KEY)
Writes ~/go2rtc/go2rtc.yaml       (one go2rtc stream per camera, in-place pull)
       ~/rtsp2rtmp/cameras.go2rtc.conf  (same cams, URLs pointed at the local
                                         hub's RTSP server rtsp://127.0.0.1:8554/<name>)

Run on the phone (Termux):  python3 ~/note9-hub/go2rtc-gen.py
Stream names are DROP-IN replacements for the original camera hostnames, so a
consumer only has to swap the enabled CONFIG file. Never hot-swap rtsp2rtmp's
live master from cameras.conf to cameras.go2rtc.conf without first starting the
hub (hub.sh start) - go2rtc must be up or every ffmpeg dies at startup.
"""
import os
import re
import socket


def resolve_host(hostport):
    """Termux Go binaries (go2rtc) read the Android /etc/resolv.conf (::1:53),
    not $PREFIX/etc/resolv.conf, so hostnames usually fail to resolve from
    go2rtc. Pre-resolve every host to its IPv4 so the config only carries IPs.
    Falls back to the hostname if resolution fails (DDNS may be down)."""
    host = hostport
    port = None
    if hostport.startswith("["):          # [v6]:port
        end = hostport.rfind("]")
        host, port = hostport[: end + 1], hostport[end + 1 :]
    elif hostport.count(":") >= 2:        # bare v6
        return hostport
    elif ":" in hostport:                 # host:port or host
        host, port = hostport.rsplit(":", 1)
    try:
        ip = socket.getaddrinfo(host, int(port) if port else None,
                                socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
    except Exception:
        return hostport
    return f"{ip}:{port}" if port else ip


def fix_url(url):
    return re.sub(r"rtsp://([^@/]+)@([^/]+)", lambda m: f"rtsp://{m.group(1)}@{resolve_host(m.group(2))}", url)


def main():
    src = os.path.expanduser("~/rtsp2rtmp/cameras.conf")
    rows, header = [], []
    for line in open(src):
        line = line.rstrip("\n")
        if line.startswith("#"):
            header.append(line)
            continue
        p = [x.strip() for x in line.split("|")]
        if len(p) >= 3 and p[0] and p[1] and p[2]:
            rows.append(p)

    y = ["streams:"]
    for name, url, _key in rows:
        y.append(f"  {name}: {fix_url(url)}")
    open(os.path.expanduser("~/go2rtc/go2rtc.yaml"), "w").write("\n".join(y) + "\n")

    g = header + [f"{n}|rtsp://127.0.0.1:8554/{n}|{k}" for n, _u, k in rows]
    open(os.path.expanduser("~/rtsp2rtmp/cameras.go2rtc.conf"), "w").write("\n".join(g) + "\n")

    print(f"wrote {len(rows)} streams -> ~/go2rtc/go2rtc.yaml")
    print(f"wrote {len(rows)} streams -> ~/rtsp2rtmp/cameras.go2rtc.conf")
    for line in header:
        if line.startswith("#"):
            print("  " + line)


if __name__ == "__main__":
    main()