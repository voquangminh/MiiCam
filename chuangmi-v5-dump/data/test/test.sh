#!/bin/sh
echo "start test.sh ..."
#/mnt/data/test/ircut2day &
echo ctrl_interface=/var/run/wpa_supplicant > /tmp/wpa.conf
echo update_config=1 >> /tmp/wpa.conf
echo network={ >> /tmp/wpa.conf
#echo ssid=\"test\" >> /tmp/wpa.conf
#echo psk=\"12345678\" >> /tmp/wpa.conf
cat /mnt/media/mmcblk0p1/manufacture/manufacture.txt >> /tmp/wpa.conf
echo key_mgmt=WPA-PSK >> /tmp/wpa.conf
echo proto=WPA WPA2 >> /tmp/wpa.conf
echo } >> /tmp/wpa.conf
