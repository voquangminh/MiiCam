# AGENTS.md

Alternate firmware for the Xiaomi Chuangmi 720p IP camera (Grain Media GM8136S SoC). Everything cross-compiles to `arm-unknown-linux-uclibcgnueabi` (ARMv5TE, uclibc, gcc 4.4.0) and is bundled onto an SD card (`sdcard/`) flashed into the camera. There is no test suite; verification = cross-compile + run on a real camera.

## Build system

- Canonical full build is Docker-based: `./manage.sh --shell` (interactive build env), `--build` (runs `make images clean`), `--build-docker`, `--all`. Toolchain is expected at `/usr/src/arm-linux-3.3/toolchain_gnueabi-4.4.0_ARMv5TE/usr/bin` inside the container.
- Top-level `Makefile` builds everything (`all`), packs the SD image (`images` → `MiiCam.zip`/`MiiCam.tgz`), and `clean` wipes `src/`, `prefix/`, `build/`, `tools/bin`, `tools/lib/*.so`.
- Third-party source tarballs are downloaded into `src/` (gitignored) per `tools/dev/sources.json` by `tools/dev/download-sources.py`; each `tools/make/*.mk` downloads/extracts/builds one package into `prefix/`.
- To build only a specific target: `make build/<target>` (e.g. `build/rtspd`, `build/onvif_server`).

## Active dev targets

- `tools/rtsp_server/rtspd.c` — the actively developed RTSP daemon. Build → `tools/bin/rtspd`. Links against `librtsp.a`, `librtsp_glibc.a`, `log/log.c`, and `libgm.so`. Recent work (via `codec_ctrl`): live bitrate/mode/fps/gop changes that apply on a ctrl-triggered auto-restart (pending-override file, async-signal-safe child/handoff), plus VUI/ROI/color defaults. The ctrl interface (`/tmp/rtspd.ctrl`, polled by `rtspd_ctrl_thread`) now also accepts `resolution WxH`, `bitrate_max`, `h264profile`, `h264level`, `vui_cs`, `vui_fr`, `flip`, `rotation`, `crop`, and bare `restart` (to apply already-staged args) — all staged in `/tmp/rtspd_pending_args` and applied on restart. Capture setup also supports crop (`-c WxH+X+Y`) and prescale reduce (`-p WxH`) via `cliArgs`. `tools/rtsp_server/` also holds variants: `rtspd-chuangmi-v5.c` (built as `tools/bin/rtspd-v5`, still kept in sync — changes often land in both `rtspd.c` and `rtspd-chuangmi-v5.c`), plus older `rtspd-claude.c`, `rtspd2MP.c`, `osd.c` (committed binaries but not the active feature target).
- `tools/utils/codec_ctrl.c` — CLI front-end to the rtspd ctrl file. Commands: `status [-j|-k]`, `keyframe`, `bitrate`, `mode`, `fps`, `gop`, plus `resolution WxH`, `bitrate_max`, `flip`, `rotation`, `crop`, `h264profile`, `h264level`, `vui_cs`, `vui_fr`, `zoom`. `status -j` parses `/proc/videograph/gmlib_setting` into JSON for the web UI.
- `tools/onvif_server/*.c` — ONVIF server → `tools/bin/onvif_server`. `tools/rtsp_server/aac_play.c` → `tools/bin/aac_play`. Note: the Makefile references `aac_player.c` but the actual file is `aac_play.c`; this will cause a build failure for `make build/aac_play` until fixed.
- `tools/rtsp_server/rtsp_audio_in.c` → `tools/bin/rtsp_audio_in` — **downlink / two-way audio** receiver (client → camera speaker). The RTSP network layer used by rtspd (`librtsp.a`) is a prebuilt static lib with NO inbound audio callback, so the downlink receiver is a **separate process** (not inside rtspd). It reuses the exact `aac_play.c` render binding (`GM_FILE_OBJECT`→`GM_AUDIO_RENDER_OBJECT`, same attr offsets/ABI 52) and feeds `gm_send_multi_bitstreams`. Usage: `rtsp_audio_in [-p recvport=5004] [-c {aac|ulaw|alaw|pcm}] [-r rate=16000] [-g ch=1] [-v]`. AAC path reads RFC 3640 MPEG4-GENERIC AUs (rtspd AUSize convention) OR raw ADTS and rebuilds ADTS-LC mono @ cfg rate; G.711 → PCM / raw PCM switch the render `encode_type` between GM_AAC=2 and GM_PCM=1 (PCM/8k/32k acceptance on the ADDA308 is UNPROVEN — vendor path is AAC-LC 16 kHz mono; test on-camera). Build: `make build/rtsp_audio_in` (wired into `all` and CI `build-rtspd.yml`, force-added binary). Two-way audio needs BOTH the existing rtspd uplink (mic→client ) AND this downlink process running alongside.
- On-camera two-way test: `rtsp_audio_in -p 5004 -c aac -r 16000` on the camera, then push RTP to `<camera>:5004` (e.g. `ffmpeg -re -i in.aac -c:a aac -ar 16000 -ac 1 -f rtp 'rtp://<camera>:5004'`, or a G.711 sender for `-c ulaw/alaw`).
- `tools/lib/*.c` — shared `libchuangmi_*.so` libs (ircut, led, pwm, isp328, utils); `tools/utils/*.c` — one-off camera utilities. Both build against GM libs in `tools/gm_lib/` (headers in `inc/`, shared libs in `lib/`).
- `tools/gm8136-rtsp-audio/` is a standalone parallel subtree (own `Dockerfile`/`Makefile`/`build.sh`, vendored SDK under its `sdk/`). It is NOT wired into the top-level Makefile and its `gm_lib/` is untracked locally. Don't assume edits there affect `tools/bin/rtspd`.

## Web UI / profiles

- `sdcard/firmware/www/` is the web frontend, served by lighttpd (doc-root `.../www/public`, PHP-CGI). lighttpd rewrites any `/api/*` to `/api/index.php`, so API routes live in `www/public/api/index.php` (a hand-rolled PHP router). The repo previously referenced a `website` target with no rule (a latent `make all` failure) — that rule now exists and just asserts the www files are present.
- Web UI reads/controls the camera by shelling out to `/tmp/sd/firmware/bin/{codec_ctrl,motor_ctrl,camera_adjust}` from PHP (`exec`). The `status` endpoint parses `codec_ctrl status -j`; motor control calls `motor_ctrl <cmd>`; profiles are JSON persisted at `/tmp/sd/firmware/etc/profiles.json` (ships in the SD image, survives reboot).
- A profile is a set of codec knobs (resolution/fps/bitrate/mode/gop/h264profile/h264level/flip/rotation) with `name`/`description`. "Apply profile" writes each knob into `/tmp/rtspd_pending_args` directly from PHP (mirroring rtspd's merge semantics), records `active_profile=<index>` in the same file (rtspd ignores that key; the web layer reads it back), then sends the `restart` command to `/tmp/rtspd.ctrl` so all changes apply in one restart.
- Note the sensor is 720p-native (MAX_FPS 15, clamp in code); only downscale resolutions (320x180…1280x720) are listed — upscaling above native may fail `gm_bind()`.

## Motor control

- `tools/utils/motor_ctrl.c` — CLI tool for PTZ motor control (renamed from `motor_control`). Build → `tools/bin/motor_ctrl`. Commands: `left`, `right`, `up`, `down`, `home`, `goto <x> <y>`, `pos`, `status`, `preset`, `calibrate`, `sync`.
- `tools/onvif_server/onvif_server.c` has inline motor control (does NOT use `motor_driver.c`).
- `tools/onvif_server/motor_driver.c` + `motor_driver.h` — standalone motor library compiled into onvif_server but **never called** (dead code). `onvif_server.c` has its own inline motor ioctl code.
- **Hardware quirk**: `H_COORD_GET`/`V_COORD_GET` ioctls always return 0 on this SoC. Both `motor_ctrl.c` and `onvif_server.c` track position via software deltas (`cur_x += dx`) instead of reading hardware. The `sync` command in motor_ctrl can re-read from hardware if needed.
- Direction mapping (same in all three files): horizontal `dx>0→dir=0` (right), `dx<0→dir=1` (left); vertical `dy>0→dir=1` (up), `dy<0→dir=0` (down).

## Building locally (what CI does — fastest path for a single binary)

1. Extract `sdk/toolchain_gnueabi-4.4.0_ARMv5TE.tgz` and `sdk/gm_lib_2015-01-09-IPCAM.tgz`, then `chmod -R +x` the SDK.
2. The 32-bit gcc 4.4.0 host binaries need 32-bit libs on a modern 64-bit host: `sudo apt-get install libc6:i386 libstdc++6:i386 lib32z1 libmpfr6:i386 libgmp10:i386`, plus symlinks:
   ```
   sudo ln -sf /usr/lib/i386-linux-gnu/libmpfr.so.6 /usr/lib/i386-linux-gnu/libmpfr.so.4
   sudo ln -sf /usr/lib/i386-linux-gnu/libgmp.so.10 /usr/lib/i386-linux-gnu/libgmp.so.3
   ```
3. `export PATH=<toolchain>/usr/bin:$PATH`, then `make build/<target>`.

WSL quirk: the 32-bit toolchain cannot read sources on Windows drvfs (`/mnt/c`) and fails with "Value too large for defined data type". The busybox build already works around this by staging in `$(HOME)/.miicam-build` (override with `MIICAM_NATIVEDIR`); expect the same error for other packages if you build the full image directly in WSL instead of in the container.

## Git / CI quirks

- `tools/bin/*`, `tools/lib/*.so`, and `sdcard/firmware/bin/*` are gitignored, yet some binaries are committed. CI force-adds them (`git add -f tools/bin/rtspd`, etc.). If you edit C code and rebuild, commit the binary with `git add -f tools/bin/<name>`, not plain `git add`.
- `codespace` is the active working branch (this checkout is on `codespace`; `master` is not checked out locally). It holds all CI workflows, vs `master` which has fewer. Workflows present on `codespace`: `build-rtspd.yml` (auto-triggers on push to `tools/rtsp_server/**`/`Makefile` AND `workflow_dispatch`, force-adds `rtspd2MP`/`rtspd`/`rtspd-v5`/`rtspd2MP00`), `build-libs.yml` (auto-triggers on push for `tools/lib/**`, `tools/utils/**`, `Makefile`), `build-onvif.yml`, `build-aac-play.yml`, `opencode.yml` (responds to `/oc` or `/opencode` in PR comments).
- `build-aac-play.yml` works around the `aac_player.c` filename bug by compiling `aac_play.c` directly with `$(TOOLCHAIN_PREFIX)-gcc` instead of using `make build/aac_play`.

## Deploying to a camera

- Camera mounts the SD at `/tmp/sd`; binaries live at `/tmp/sd/firmware/bin`. `tools/dev/helpers.sh` (sourced inside the build container) provides `rb <target>` and `upload_rtsp`/`upload_binary` via `scp root@$CAMERA_HOSTNAME`; requires `tools/dev/host.cfg` (gitignored) with the camera hostname.
- `tools/deploy_camera.js` — Node.js script that SSHs to the camera and uploads binaries + libs. Uses `ssh2` package. Hardcoded to deploy `chuangmi_ctrl`, `codec_ctrl`, `motor_ctrl`, `camera_adjust`, `rtspd` and `libchuangmi_*.so` libs.
- `sdcard/config.cfg` is the camera config and is generated (not hand-edited) by `sdcard/firmware/scripts/update/configupdate`; the target device reads `/tmp/sd/config.cfg`.
