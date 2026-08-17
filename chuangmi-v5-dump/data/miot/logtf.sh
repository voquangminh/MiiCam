#!/bin/sh

BASELOG=/var/log/messages
MIIO_CLIENT_LOG=/var/log/miio_client.log
TARGETDIR=/mnt/media
FOUND=no

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

cp -af $BASELOG $DEST/`basename $BASELOG`
cp -af $MIIO_CLIENT_LOG $DEST/`basename $MIIO_CLIENT_LOG`
sync
