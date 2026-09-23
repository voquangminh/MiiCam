#!/bin/sh
## Flush tmpfs logs (/var/log, RAM) to the SD card as gzip, then truncate.
## Runs nightly from crontab. busybox-safe (no logrotate required).
## Daemons hold append-mode fds, so we copy-then-truncate instead of
## renaming: the fd keeps writing to the same inode and continues normally.

LOGDIR="/var/log"
ARCHIVE="/tmp/sd/log/archive"
TODAY=$(date +%Y%m%d)

mkdir -p "$ARCHIVE"
[ -d "$ARCHIVE" ] || exit 0

for f in syslog rtspd.log lighttpd.log php_errors.log webapp.log mqtt.log; do
    [ -f "$LOGDIR/$f" ] || continue
    [ -s "$LOGDIR/$f" ] || continue

    cat "$LOGDIR/$f" > "$ARCHIVE/${TODAY}_${f}"
    gzip -f "$ARCHIVE/${TODAY}_${f}"

    : > "$LOGDIR/$f"
done

## Prune archives older than 14 days
find "$ARCHIVE" -type f -name "*.gz" -mtime +14 -exec rm -f {} \; 2>/dev/null

exit 0