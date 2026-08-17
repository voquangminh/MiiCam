#!/bin/sh
/tmp/S50loader restore
export LD_LIBRARY_PATH=/tmp
BASELOG=/mnt/data/imi/imi_log/
MIIO_CLIENT_LOG=/var/log/miio_client.log
TARGETDIR=/mnt/media
FOUND=no

/tmp/ld-uClibc.so.0 /tmp/busybox echo "OTA: copy log to TF."  >> /mnt/data/imi/imi_log/ota_1.log
/tmp/ld-uClibc.so.0 /tmp/busybox echo "OTA: about to reboot." >> /mnt/data/imi/imi_log/ota_1.log

#/tmp/ld-uClibc.so.0 /tmp/busybox sh /mnt/data/miot/logtf.sh

for d in ${TARGETDIR}/*; do
    if [ -d "$d" ]; then
	if [ ! -d "$d/log" ]; then
	    mkdir -p "$d/log"
	fi
	DEST=$d/log
	FOUND=true
	break
    fi
done

if [ x$FOUND = xno ]; then
    exit
fi

/tmp/ld-uClibc.so.0 /tmp/busybox cp -af $BASELOG $DEST/
/tmp/ld-uClibc.so.0 /tmp/busybox cp -af $MIIO_CLIENT_LOG $DEST/

/tmp/ld-uClibc.so.0 /tmp/busybox sync

