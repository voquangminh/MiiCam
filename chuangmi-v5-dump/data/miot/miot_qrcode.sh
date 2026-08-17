#!/bin/sh

MIOT_QRCODE="/mnt/data/miot/miot_qrcode"
RECV_LINE="/mnt/data/miot/recv_line"
MIIO_SEND_LINE="/mnt/data/ot_wifi_tool/miio_send_line"

QRCODE_FILE="/tmp/qrcode.b39e528e"
SOUND_FIFO="/tmp/sound_fifo"

LETSQUIT="no"

# contains(string, substring)
#
# Returns 0 if the specified string contains the specified substring,
# otherwise returns 1.
contains() {
    string="$1"
    substring="$2"
    if test "${string#*$substring}" != "$string"
    then
        return 0    # $substring is in $string
    else
        return 1    # $substring is not in $string
    fi
}

sanity_check() {
    wifi_ready=`/usr/sbin/nvram get miio_ssid`
    if [ x"$wifi_ready" = x ]; then
	wifi_ready="no"
    else
	wifi_ready="yes"
    fi

    is_ft_p2p=`/bin/cat  /tmp/ft_mode`

    if [ $wifi_ready = "yes" ] || [ $is_ft_p2p = "3" ]; then
	echo "WiFi sta mode, no need to do QRscan"
	LETSQUIT="yes"
    fi
}

main() {
    while true; do
	BUF=`$RECV_LINE`
	if [ $? -ne 0 ]; then
	    sleep 0.1;
	    continue
	fi
	if contains "$BUF" "wifi_ap_mode"; then
	    while true; do
		if [ -p "$SOUND_FIFO" ]; then
		    echo "entering WiFi AP mode"
		    #echo "/mnt/data/sound/5GHz_wifi_supported.aac" > "$SOUND_FIFO"
		    echo "/mnt/data/sound/qrcode_supported.aac" > "$SOUND_FIFO"
		    break
		else
		    echo "WARNING: no $SOUND_FIFO"
		    sleep 1
		fi
	    done

	    RETRY=1
	    TRYNUM=1
	    echo "/mnt/data/sound/waiting_connection.aac" > "$SOUND_FIFO"
	    while true; do
		if [ ! -f $QRCODE_FILE ]; then
		    if [ $((RETRY%200)) -eq 0 ]; then
			sanity_check
			if [ x"$LETSQUIT" = xyes ]; then
			    break
			fi

			if [ $TRYNUM -eq 60 ]; then
				echo "/mnt/data/sound/waiting_connection.aac" > "$SOUND_FIFO"
			fi

			let TRYNUM=$TRYNUM+1
			echo "QRcode scan Retry $RETRY"
		    fi

            if [ $((RETRY%24000)) -eq 0 ]; then
                /usr/bin/killall wpa_supplicant hostapd udhcpd
                /sbin/ifconfig wlan0 down
            fi

		    let RETRY=$RETRY+1
		    sleep 0.1
		    continue
		fi

		break
	    done

	    if [ x"$LETSQUIT" = xyes ]; then
		# continue, so that we can monitor "wifi_connected" and "cloud_connected"
		continue
	    fi

	    STRING=`cat $QRCODE_FILE`
	    #BINDKEY=$(echo $STRING | awk -F'\a' '{print $1}')
	    BINDKEY=$(echo  $STRING | awk 'BEGIN{FS="\a"}{print $1}')
	    #SSID=$(echo $STRING | awk -F'\a' '{print $2}')
	    SSID=$(echo $STRING | awk 'BEGIN{FS="\a"}{print $2}')
	    #PASSWD=$(echo $STRING | awk -F'\a' '{print $3}')
	    PASSWD=$(echo $STRING | awk 'BEGIN{FS="\a"}{print $3}')
	    COUNTRY_DOMAIN=$(echo $STRING | awk 'BEGIN{FS="\a"}{print $4}')
	    TIMEZONE=$(echo $STRING | awk 'BEGIN{FS="\a"}{print $5}')
	    echo "bind key: $BINDKEY"
	    echo "ssid: $SSID"
	    echo "passwd: $PASSWD"
	    echo "country_domain: $COUNTRY_DOMAIN"
	    echo "time zone:$TIMEZONE"
	    rm -rf   /mnt/data/TZ
	    ln -s /usr/share/zoneinfo/uclibc/$TIMEZONE  /mnt/data/TZ
	    # sanity check
	    if [ x"$BINDKEY" = x ]; then
		echo "bind key null, quit"
		exit 0
	    fi

	    # handle special char, e.g.: '"', '\'
	    # Here we're using sed, we might switch to jshon
	    SSID=$(echo $SSID | sed -e 's/^"/\\"/' | sed -e 's/\([^\]\)"/\1\\"/g' | sed -e 's/\([^\]\)"/\1\\"/g' | sed -e 's/\([^\]\)\(\\[^"\\\/bfnrtu]\)/\1\\\2/g' | sed -e 's/\([^\]\)\\$/\1\\\\/')
	    PASSWD=$(echo $PASSWD | sed -e 's/^"/\\"/' | sed -e 's/\([^\]\)"/\1\\"/g' | sed -e 's/\([^\]\)"/\1\\"/g' | sed -e 's/\([^\]\)\(\\[^"\\\/bfnrtu]\)/\1\\\2/g' | sed -e 's/\([^\]\)\\$/\1\\\\/')

	    echo "/mnt/data/sound/qrcode_success.aac" > "$SOUND_FIFO"
	    if [ "x$COUNTRY_DOMAIN" = "xNULL" ]; then
	    	msg="{\"id\":95279527,\"method\":\"local.ble.config_router\",\"params\":{\"bind_key\":\"$BINDKEY\",\"ssid\":\"$SSID\",\"passwd\":\"$PASSWD\"}}"
	    else
	    	msg="{\"id\":95279527,\"method\":\"local.ble.config_router\",\"params\":{\"bind_key\":\"$BINDKEY\",\"ssid\":\"$SSID\",\"passwd\":\"$PASSWD\",\"country_domain\":\"$COUNTRY_DOMAIN\"}}"
	    fi
	    echo $msg
	    $MIIO_SEND_LINE "$msg"

	    echo "/mnt/data/sound/connecting_pease_wait.aac" > "$SOUND_FIFO"
	elif contains "$BUF" "wifi_connected"; then
	    echo "/mnt/data/sound/wifi_connected.aac" > "$SOUND_FIFO"

	    is_ft_p2p=`/bin/cat  /tmp/ft_mode`
	    if [ $is_ft_p2p = "3" ]; then
		exit 0
	    fi
	elif contains "$BUF" "cloud_connected"; then
	    echo "/mnt/data/sound/binding_success.aac" > "$SOUND_FIFO"
	    exit 0
	fi
    done
}

sanity_check
if [ x"$LETSQUIT" = xyes ]; then
    exit 0
fi
main
