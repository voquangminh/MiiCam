#!/bin/sh
## timelapse.sh - periodic snapshot capture for time-lapse videos.
## Usage: timelapse.sh {start|stop|status}
## Config (config.cfg):
##   TIMELAPSE_INTERVAL  seconds between frames (default 10)
##   TIMELAPSE_DURATION  minutes to run (0 = unlimited, default 0)
## Snapshots are captured via the standard rtspd trigger (take_snapshot,
## /dev/shm/rtspd_snapshot) and copied into /tmp/sd/timelapse/YYYY-MM-DD/.

SCRIPT="$( basename ${0} )"
SD_MOUNTDIR="/tmp/sd"
PIDFILE="/var/run/timelapse.pid"
DIRNAME="date +%Y-%m-%d"

if [ -r "${SD_MOUNTDIR}/firmware/scripts/functions.sh" ]; then
    . "${SD_MOUNTDIR}/firmware/scripts/functions.sh"
else
    echo "Unable to load basic functions"
    exit 1
fi

[ -z "$TIMELAPSE_INTERVAL" ] && TIMELAPSE_INTERVAL=10
[ -z "$TIMELAPSE_DURATION" ] && TIMELAPSE_DURATION=0

capture_once()
{
    ## Blocking: returns only after rtspd produced a fresh JPEG.
    local lastpath
    lastpath=$( /tmp/sd/firmware/bin/take_snapshot 2>/dev/null | last_f | tail -n 1 )
    [ -n "$lastpath" ] && [ -f "$lastpath" ] && echo "$lastpath"
}

do_start()
{
    if [ -f "${PIDFILE}" ] && kill -0 "$( cat "${PIDFILE}" 2>/dev/null )" 2>/dev/null; then
        echo "Timelapse already running (PID $( cat "${PIDFILE}" ))"
        exit 0
    fi

    ## Re-exec self in "run" mode under nohup so the loop survives the init
    ## script / shell exit that launched us. Explicit /bin/sh: the script is
    ## not marked executable on the (vfat) SD card.
    nohup /bin/sh "$0" run >/dev/null 2>&1 &
    echo $! > "${PIDFILE}"
    echo "Timelapse started (PID $( cat "${PIDFILE}" ), every ${TIMELAPSE_INTERVAL}s)"
}

do_run()
{
    counter=0
    last_prefix=''
    started=$( date +%s )

    while :; do
        [ "$TIMELAPSE_DURATION" -gt 0 ] && [ $(( $( date +%s ) - started )) -ge $(( TIMELAPSE_DURATION * 60 )) ] && break

        SAVE_DIR="${SD_MOUNTDIR}/timelapse/$( ${DIRNAME} )"
        [ -d "$SAVE_DIR" ] || mkdir -p "$SAVE_DIR"

        srcpath=$( capture_once )
        if [ -n "$srcpath" ] && [ -f "$srcpath" ]; then
            prefix="$( date +%Y-%m-%d_%H-%M-%S )"
            if [ "$prefix" = "$last_prefix" ]; then
                counter=$(( counter + 1 ))
            else
                counter=1
                last_prefix="$prefix"
            fi
            filename="${prefix}_$( printf '%03d' "$counter" ).jpg"
            cp "$srcpath" "${SAVE_DIR}/${filename}"
        fi

        sleep "$TIMELAPSE_INTERVAL"
    done

    rm -f "${PIDFILE}"
}

do_stop()
{
    if [ -f "${PIDFILE}" ]; then
        kill "$( cat "${PIDFILE}" 2>/dev/null )" 2>/dev/null
        rm -f "${PIDFILE}"
    fi
    echo "Timelapse stopped"
}

do_status()
{
    if [ -f "${PIDFILE}" ] && kill -0 "$( cat "${PIDFILE}" 2>/dev/null )" 2>/dev/null; then
        echo "running $( cat "${PIDFILE}" )"
    else
        echo "stopped"
    fi
}

case "$1" in
    start)
        do_start
        ;;
    run)
        do_run
        ;;
    stop)
        do_stop
        ;;
    restart)
        do_stop
        do_start
        ;;
    status)
        do_status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac

exit 0