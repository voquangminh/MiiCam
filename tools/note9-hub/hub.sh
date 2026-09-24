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

case "${1:-}" in
    start)
        command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock
        start_pipe youtube "$HERE/pipelines/youtube.sh"
        if [ "${AI_ENABLED:-1}" = "1" ]; then
            start_pipe ai "$HERE/pipelines/ai.sh"
        else
            echo "ai: disabled in config"
        fi
        ;;
    stop)
        stop_pipe youtube
        stop_pipe ai
        command -v termux-wake-unlock >/dev/null 2>&1 && termux-wake-unlock
        ;;
    restart)
        "$0" stop
        sleep 2
        "$0" start
        ;;
    status)
        status_pipe youtube
        status_pipe ai
        ;;
    logs)
        tail -n "${2:-50}" "$LOG"/*.log
        ;;
    *)
        echo "usage: $0 {start|stop|restart|status|logs [n]}"
        exit 1
        ;;
esac
