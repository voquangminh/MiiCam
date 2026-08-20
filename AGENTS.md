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
- Manual workflows (`.github/workflows/build-rtspd.yml`, `build-onvif.yml`, `build-aac-play.yml`) are `workflow_dispatch` only (manual trigger). They reproduce the local-build steps on `ubuntu-latest` and auto-commit the binary (message style: "Automated build: update camera binary rtspd2MP/rtspd/rtspd-v5" or "Automated build: update onvif_server" / "Automated build: update aac_player").
- `build-rtspd.yml` builds rtspd but force-adds all four committed binaries (`rtspd`, `rtspd2MP`, `rtspd-v5`, `rtspd2MP00`).
- `build-aac-play.yml` works around the `aac_player.c` filename bug by compiling `aac_play.c` directly with `$(TOOLCHAIN_PREFIX)-gcc` instead of using `make build/aac_play`.

## Deploying to a camera

- Camera mounts the SD at `/tmp/sd`; binaries live at `/tmp/sd/firmware/bin`. `tools/dev/helpers.sh` (sourced inside the build container) provides `rb <target>` and `upload_rtsp`/`upload_binary` via `scp root@$CAMERA_HOSTNAME`; requires `tools/dev/host.cfg` (gitignored) with the camera hostname.
- `sdcard/config.cfg` is the camera config and is generated (not hand-edited) by `sdcard/firmware/scripts/update/configupdate`; the target device reads `/tmp/sd/config.cfg`.
