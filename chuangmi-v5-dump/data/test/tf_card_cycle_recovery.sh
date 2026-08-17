#!/bin/sh

if [ -f /mnt/media/mmcblk0p1/ota_loop ];then
version=`cat /etc/os-release | grep XIAOMI_VERSION= | cut -c16-31`
if [ -f /mnt/media/mmcblk0p1/tf_recovery.img.bak ];then
	cd /mnt/media/mmcblk0p1/
	mv tf_recovery.img.bak debug_${version}_tf_recovery.img
fi
	echo $version
	cd /mnt/media/mmcblk0p1/
	tf_version=`find -name "*tf_recovery*" | cut -c9-24`
	echo $tf_version
if [ $version = $tf_version ];then
	echo "=="
	else
	echo "!="
	cd /mnt/media/mmcblk0p1/
	mv *tf_recover* tf_recovery.img
	echo "mv to tf_recovery.img"
	while true
	do
        ret=`wpa_cli status | grep COMPLETED`
        if [ $? == "0" ];then
         echo "000"
         sleep 10
         reboot
        else
         echo "111"
        fi
	sleep 10
	done
#	reboot
	fi
fi
