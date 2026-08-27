# AGENTS.md

Alternate firmware for the Xiaomi Chuangmi 720p IP camera (Grain Media GM8136S SoC). Everything cross-compiles to `arm-unknown-linux-uclibcgnueabi` (ARMv5TE, uclibc, gcc 4.4.0) and is bundled onto an SD card (`sdcard/`) flashed into the camera. There is no test suite; verification = cross-compile + run on a real camera.

## Build system

- Canonical full build is Docker-based: `./manage.sh --shell` (interactive build env), `--build` (runs `make images clean`), `--build-docker`, `--all`. Toolchain is expected at `/usr/src/arm-linux-3.3/toolchain_gnueabi-4.4.0_ARMv5TE/usr/bin` inside the container.
- Top-level `Makefile` builds everything (`all`), packs the SD image (`images` → `MiiCam.zip`/`MiiCam.tgz`), and `clean` wipes `src/`, `prefix/`, `build/`, `tools/bin`, `tools/lib/*.so`.
- Third-party source tarballs are downloaded into `src/` (gitignored) per `tools/dev/sources.json` by `tools/dev/download-sources.py`; each `tools/make/*.mk` downloads/extracts/builds one package into `prefix/`.
- To build only a specific target: `make build/<target>` (e.g. `build/rtspd`, `build/onvif_server`).

## Active dev targets

- `tools/rtsp_server/rtspd.c` — the actively developed RTSP daemon (recent work: AAC audio). Build → `tools/bin/rtspd`. Links against `librtsp.a`, `librtsp_glibc.a`, `log/log.c`, and `libgm.so`. `tools/rtsp_server/` also holds older variants (`rtspd-chuangmi-v5.c`, `rtspd-claude.c`, `rtspd2MP.c`, `rtspd2MP00.c`, `osd.c`) that are NOT the active build.
- `tools/onvif_server/*.c` — ONVIF server → `tools/bin/onvif_server`. `tools/rtsp_server/aac_play.c` → `tools/bin/aac_play`. Note: the Makefile references `aac_player.c` but the actual file is `aac_play.c`; this will cause a build failure for `make build/aac_play` until fixed.
- `tools/lib/*.c` — shared `libchuangmi_*.so` libs (ircut, led, pwm, isp328, utils); `tools/utils/*.c` — one-off camera utilities. Both build against GM libs in `tools/gm_lib/` (headers in `inc/`, shared libs in `lib/`).
- `tools/gm8136-rtsp-audio/` is a standalone parallel subtree (own `Dockerfile`/`Makefile`/`build.sh`, vendored SDK under its `sdk/`). It is NOT wired into the top-level Makefile and its `gm_lib/` is untracked locally. Don't assume edits there affect `tools/bin/rtspd`.

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
- `codespace` branch has additional CI workflows not on `master`: `build-libs.yml` (auto-triggers on push for `tools/lib/**`, `tools/utils/**`, `Makefile`), `build-onvif.yml`, `build-aac-play.yml`, `opencode.yml` (responds to `/oc` or `/opencode` in PR comments).
- `build-rtspd.yml` (on `master`) is `workflow_dispatch` only and force-adds all four committed binaries (`rtspd`, `rtspd2MP`, `rtspd-v5`, `rtspd2MP00`).
- `build-aac-play.yml` works around the `aac_player.c` filename bug by compiling `aac_play.c` directly with `$(TOOLCHAIN_PREFIX)-gcc` instead of using `make build/aac_play`.

## Deploying to a camera

- Camera mounts the SD at `/tmp/sd`; binaries live at `/tmp/sd/firmware/bin`. `tools/dev/helpers.sh` (sourced inside the build container) provides `rb <target>` and `upload_rtsp`/`upload_binary` via `scp root@$CAMERA_HOSTNAME`; requires `tools/dev/host.cfg` (gitignored) with the camera hostname.
- `tools/deploy_camera.js` — Node.js script that SSHs to the camera and uploads binaries + libs. Uses `ssh2` package. Hardcoded to deploy `chuangmi_ctrl`, `codec_ctrl`, `motor_ctrl`, `camera_adjust`, `rtspd` and `libchuangmi_*.so` libs.
- `sdcard/config.cfg` is the camera config and is generated (not hand-edited) by `sdcard/firmware/scripts/update/configupdate`; the target device reads `/tmp/sd/config.cfg`.
