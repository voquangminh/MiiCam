#!/bin/sh
/mnt/data/imi/imi_init/S50loader backup

cp -f /bin/busybox /tmp/busybox
cp -f /mnt/data/miio_ota/ld-uClibc.so.0 /tmp
cp -f /lib/libuClibc-0.9.33.2.so /tmp/libc.so.0

BIN_BUSYBOX_MD5=`md5sum /bin/busybox | awk {'print $1'}`
TMP_BUSYBOX_MD5=`md5sum /tmp/busybox | awk {'print $1'}`

LIB_LDLIBC_MD5=`md5sum /mnt/data/miio_ota/ld-uClibc.so.0 | awk {'print $1'}`
TMP_LDLIBC_MD5=`md5sum /tmp/ld-uClibc.so.0 | awk {'print $1'}`

LIB_LIBC_MD5=`md5sum /lib/libc.so.0 | awk {'print $1'}`
TMP_LIBC_MD5=`md5sum /tmp/libc.so.0 | awk {'print $1'}`

if [ $BIN_BUSYBOX_MD5 == $TMP_BUSYBOX_MD5 ] && 
   [ $LIB_LIBC_MD5 == $TMP_LIBC_MD5 ] && 
   [ $LIB_LDLIBC_MD5 == $TMP_LDLIBC_MD5 ];then
    echo "OTA: before we start, free out more memory..." | /tmp/busybox logger -t miio_ota
    /mnt/data/imi/imi_init/S99restartd stop
    /mnt/data/imi/imi_init/S95miio_smb stop
    /mnt/data/imi/imi_init/S60miio_avstreamer stop
    /mnt/data/imi/imi_init/S93miot_devicekit stop
    #/mnt/data/imi/imi_init/S50mosquitto stop

    /tmp/busybox killall udhcpc
    /tmp/busybox killall dbus-daemon
    /tmp/busybox killall crond
    /tmp/busybox killall telnetd
    /tmp/busybox killall miio_client_helper_nomqtt.sh
    #/mnt/data/imi/imi_init/S01logging stop
    #/tmp/busybox killall logger    
    #rm /var/log/* -rf
    rm /tmp/alarm* -rf
for service in /tmp/firmware/etc/init.d/*; do $service stop; done
    exit 0
else
    echo "OTA: cope busybox or uClibc error." | /tmp/busybox logger -t miio_ota
    echo "OTA: {busybox:{bin:$BIN_BUSYBOX_MD5,tmp:$TMP_BUSYBOX_MD5},ld-uClibc:{lib:$LIB_LDLIBC_MD5,tmp:$TMP_LDLIBC_MD5},libc{lib:$LIB_LIBC_MD5, tmp:$LIB_LIBC_MD5}}" | /tmp/busybox logger -t miio_ota
    exit 1
fi

