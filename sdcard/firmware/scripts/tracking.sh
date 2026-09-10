#!/bin/sh

## tracking.sh - Unified tracking control script
## Usage: tracking.sh {on|off|status|blink}
##   on   - Start tracking mode, LED blink
##   off  - Stop tracking mode, LED off
##   status - Show tracking state
##   blink  - Manual LED blink test

SCRIPT="$( basename ${0} )"
SD_MOUNTDIR="/tmp/sd"
STATE_FILE="/dev/shm/rtspd_tracking_state"
PIDFILE="/var/run/tracking.pid"
LED_BLINK_PIDFILE="/var/run/tracking_led.pid"

if [ -r "${SD_MOUNTDIR}/firmware/scripts/functions.sh" ]; then
    . "${SD_MOUNTDIR}/firmware/scripts/functions.sh"
else
    echo "Unable to load basic functions"
    exit 1
fi

led_on()
{
    /tmp/sd/firmware/bin/blue_led -e 2>/dev/null
}

led_off()
{
    /tmp/sd/firmware/bin/blue_led -d 2>/dev/null
}

led_blink_start()
{
    led_blink_stop 2>/dev/null
    (
        while true; do
            led_on
            sleep 1
            led_off
            sleep 1
        done
    ) &
    echo $! > "${LED_BLINK_PIDFILE}"
}

led_blink_stop()
{
    if [ -f "${LED_BLINK_PIDFILE}" ]; then
        local pid=$(cat "${LED_BLINK_PIDFILE}" 2>/dev/null)
        if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null
        fi
        rm -f "${LED_BLINK_PIDFILE}"
    fi
    led_off
}

do_on()
{
    echo "Tracking ON"
    led_blink_start

    if [ "${ENABLE_MQTT}" -eq 1 ]; then
        mqtt_send "${TRACKING_TOPIC:-${MQTT_TOPIC}/tracking}" "$MQTT_ON"
    fi
}

do_off()
{
    echo "Tracking OFF"
    led_blink_stop
    led_off

    if [ "${ENABLE_MQTT}" -eq 1 ]; then
        mqtt_send "${TRACKING_TOPIC:-${MQTT_TOPIC}/tracking}" "$MQTT_OFF"
    fi
}

do_status()
{
    if [ -f "${STATE_FILE}" ]; then
        cat "${STATE_FILE}"
    else
        echo "stopped 0 0"
    fi
}

case "$1" in
    on)
        do_on
        ;;
    off)
        do_off
        ;;
    status)
        do_status
        ;;
    blink)
        led_blink_start
        echo "LED blink started (PID: $(cat ${LED_BLINK_PIDFILE} 2>/dev/null))"
        ;;
    stop_blink)
        led_blink_stop
        echo "LED blink stopped"
        ;;
    *)
        echo "Usage: $0 {on|off|status|blink|stop_blink}"
        exit 1
        ;;
esac

exit 0
