#!/data/data/com.termux/files/usr/bin/bash
set -u

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

if [ ! -f "$HERE/config.sh" ]; then
    echo "config.sh not found. Copy config.example.sh -> config.sh and edit it."
    exit 1
fi
. "$HERE/config.sh"

RUN="$HOME/.note9-hub/run"
LOG="$HOME/.note9-hub/log"
mkdir -p "$RUN" "$LOG"

start_pipe() {
    name="$1"; shift
    pidfile="$RUN/$name.pid"
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name already running (pid $(cat "$pidfile"))"
        return 0
    fi
    nohup bash "$@" >>"$LOG/$name.log" 2>&1 &
    echo $! >"$pidfile"
    echo "$name started (pid $!, log $LOG/$name.log)"
}

stop_pipe() {
    name="$1"
    pidfile="$RUN/$name.pid"
    [ -f "$pidfile" ] || { echo "$name not running"; return 0; }
    pid="$(cat "$pidfile")"
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null
        i=0
        while [ $i -lt 10 ] && kill -0 "$pid" 2>/dev/null; do sleep 1; i=$((i+1)); done
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
        echo "$name stopped"
    else
        echo "$name was not running (stale pidfile)"
    fi
    rm -f "$pidfile"
}

status_pipe() {
    name="$1"
    pidfile="$RUN/$name.pid"
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name: running (pid $(cat "$pidfile"))"
    else
        echo "$name: stopped"
    fi
}

SNAP_DIR="${AI_SNAPSHOT_DIR:-$HOME/.note9-hub/snapshots}"
HTTP_PORT="${AI_HTTP_PORT:-8080}"

case "${1:-}" in
    start)
        mkdir -p "$SNAP_DIR"
        command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock
        if [ -x "$HOME/go2rtc/go2rtc" ] && [ -f "$HOME/go2rtc/go2rtc.yaml" ]; then
            start_pipe go2rtc "$HERE/pipelines/go2rtc.sh"
        else
            echo "go2rtc: disabled (~/go2rtc/go2rtc binary/config missing)"
        fi
        if [ -n "${YOUTUBE_RTMP_URL:-}" ]; then
            start_pipe youtube "$HERE/pipelines/youtube.sh"
        else
            echo "youtube: disabled (YOUTUBE_RTMP_URL empty in config)"
        fi
        if [ "${AI_ENABLED:-1}" = "1" ]; then
            start_pipe ai "$HERE/pipelines/ai.sh"
        else
            echo "ai: disabled in config"
        fi
        ;;
    stop)
        stop_pipe go2rtc
        stop_pipe youtube
        stop_pipe ai
        stop_pipe http
        command -v termux-wake-unlock >/dev/null 2>&1 && termux-wake-unlock
        ;;
    restart)
        "$0" stop
        sleep 2
        "$0" start
        ;;
    status)
        status_pipe go2rtc
        status_pipe youtube
        status_pipe ai
        status_pipe http
        ;;
    logs)
        tail -n "${2:-50}" "$LOG"/*.log
        ;;
    serve)
        ## Snapshot viewer: serves AI_SNAPSHOT_DIR over HTTP (e.g. over
        ## Tailscale: http://<tailscale-ip>:$(AI_HTTP_PORT)/). Pure python, no
        ## extra deps.
        if [ -f "$RUN/http.pid" ] && kill -0 "$(cat "$RUN/http.pid")" 2>/dev/null; then
            echo "http already serving :$HTTP_PORT (pid $(cat "$RUN/http.pid"))"
            exit 0
        fi
        mkdir -p "$SNAP_DIR"
        nohup python3 -m http.server "$HTTP_PORT" --directory "$SNAP_DIR" >>"$LOG/http.log" 2>&1 &
        echo $! >"$RUN/http.pid"
        echo "snapshots http server on :$HTTP_PORT (pid $!, dir $SNAP_DIR)"
        ;;
    *)
        echo "usage: $0 {start|stop|restart|status|logs [n]|serve}"
        exit 1
        ;;
esac
