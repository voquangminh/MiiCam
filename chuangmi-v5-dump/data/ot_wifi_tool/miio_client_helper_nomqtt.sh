#!/bin/sh

WIFI_START_SCRIPT="/mnt/data/ot_wifi_tool/wifi_start.sh"
MIIO_RECV_LINE="/mnt/data/ot_wifi_tool/miio_recv_line"
MIIO_SEND_LINE="/mnt/data/ot_wifi_tool/miio_send_line"
WIFI_MAX_RETRY=1
WIFI_RETRY_INTERVAL=3
WIFI_SSID=

GLIBC_TIMEZONE_DIR="/usr/share/zoneinfo"
UCLIBC_TIMEZONE_DIR="/usr/share/zoneinfo/uclibc"
YOUR_LINK_TIMEZONE_FILE="/mnt/data/TZ"
YOUR_TIMEZONE_DIR=$UCLIBC_TIMEZONE_DIR
LINK_HOSTS_FILE="/mnt/data/hosts"
PRC_LINK_HOSTS_FILE="/mnt/data/hosts.prc"
GLOBAL_LINK_HOSTS_FILE="/mnt/data/hosts.global"
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

send_helper_ready() {
    ready_msg="{\"method\":\"_internal.helper_ready\"}"
    echo $ready_msg
    $MIIO_SEND_LINE "$ready_msg"
}

req_wifi_conf_status() {
    wificonf_dir=$1
    wificonf_dir=${wificonf_dir##*params\":\"}
    wificonf_dir=${wificonf_dir%%\"*}

    wifi_ready=`/usr/sbin/nvram get miio_ssid`
    if [ x"$wifi_ready" = x ]; then
	wifi_ready="no"
    else
	wifi_ready="yes"
    fi

    is_ft_p2p=`/bin/cat  /tmp/ft_mode`

    REQ_WIFI_CONF_STATUS_RESPONSE=""
    if [ $wifi_ready = "yes" ] || [ $is_ft_p2p = "3" ]; then
	REQ_WIFI_CONF_STATUS_RESPONSE="{\"method\":\"_internal.res_wifi_conf_status\",\"params\":1}"

	WIFI_SSID=`/usr/sbin/nvram get miio_ssid`
	echo "WIFI_SSID: $WIFI_SSID"
    else
	REQ_WIFI_CONF_STATUS_RESPONSE="{\"method\":\"_internal.res_wifi_conf_status\",\"params\":0}"
    fi
}

request_dinfo() {
    dinfo_did=`/usr/sbin/nvram factory get did`
    dinfo_key=`/usr/sbin/nvram factory get key`
    dinfo_vendor=`/usr/sbin/nvram factory get vendor`
    dinfo_mac=`/usr/sbin/nvram factory get mac`
    dinfo_model=`/usr/sbin/nvram factory get model`
    RESPONSE_DINFO="{\"method\":\"_internal.response_dinfo\",\"params\":{"
    if [ x$dinfo_did != x ]; then
	RESPONSE_DINFO="$RESPONSE_DINFO\"did\":$dinfo_did"
    fi
    if [ x$dinfo_key != x ]; then
	RESPONSE_DINFO="$RESPONSE_DINFO,\"key\":\"$dinfo_key\""
    fi
    if [ x$dinfo_vendor != x ]; then
	RESPONSE_DINFO="$RESPONSE_DINFO,\"vendor\":\"$dinfo_vendor\""
    fi
    if [ x$dinfo_mac != x ]; then
	RESPONSE_DINFO="$RESPONSE_DINFO,\"mac\":\"$dinfo_mac\""
    fi
    if [ x$dinfo_model != x ]; then
	RESPONSE_DINFO="$RESPONSE_DINFO,\"model\":\"$dinfo_model\""
    fi
    RESPONSE_DINFO="$RESPONSE_DINFO}}"
}

request_dtoken() {
    dtoken_string=$1
    dtoken_dir=${dtoken_string##*dir\":\"}
    dtoken_dir=${dtoken_dir%%\"*}
    dtoken_token=${dtoken_string##*ntoken\":\"}
    dtoken_token=${dtoken_token%%\"*}

    wifi_ready=`/usr/sbin/nvram get miio_ssid`
    if [ x"$wifi_ready" = x ]; then
	wifi_ready="no"
    else
	wifi_ready="yes"
    fi

    if [ $wifi_ready = "no" ]; then
	/usr/sbin/nvram unset miio_token
	/usr/sbin/nvram commit
    fi

    miio_token=`/usr/sbin/nvram get miio_token`
    if [ x$miio_token = x ]; then
	/usr/sbin/nvram set miio_token=$dtoken_token
	/usr/sbin/nvram commit
	miio_token=`/usr/sbin/nvram get miio_token`
    fi

    miio_country=`/usr/sbin/nvram get miio_country`
    if [ -f $LINK_HOSTS_FILE ]; then
	unlink $LINK_HOSTS_FILE
    fi
    if [ x$miio_country = x ]; then
	ln -sf $PRC_LINK_HOSTS_FILE $LINK_HOSTS_FILE
    else
	ln -sf $GLOBAL_LINK_HOSTS_FILE $LINK_HOSTS_FILE
    fi
    RESPONSE_DCOUNTRY="{\"method\":\"_internal.response_dcountry\",\"params\":\"${miio_country}\"}"
    RESPONSE_DTOKEN="{\"method\":\"_internal.response_dtoken\",\"params\":\"${miio_token}\"}"
}

save_wifi_conf() {
    miio_ssid=$1
    miio_passwd=$2
    miio_uid=$3
    miio_country=$4
    if [ -f $LINK_HOSTS_FILE ]; then
	unlink $LINK_HOSTS_FILE
    fi
    if [ x$miio_country = x ]; then
	ln -sf $PRC_LINK_HOSTS_FILE $LINK_HOSTS_FILE
    else
	ln -sf $GLOBAL_LINK_HOSTS_FILE $LINK_HOSTS_FILE
    fi
    /usr/sbin/nvram set miio_ssid="$miio_ssid" > /dev/null 2>&1
    /usr/sbin/nvram set miio_passwd="$miio_passwd" > /dev/null 2>&1
    /usr/sbin/nvram set miio_uid=$miio_uid > /dev/null 2>&1
    /usr/sbin/nvram set miio_country="$miio_country" > /dev/null 2>&1
    if [ x"$miio_passwd" = x ]; then
	/usr/sbin/nvram set miio_key_mgmt="NONE" > /dev/null 2>&1
    else
	/usr/sbin/nvram set miio_key_mgmt="WPA" > /dev/null 2>&1
    fi
    /usr/sbin/nvram commit > /dev/null 2>&1
}

clear_wifi_conf() {
    /usr/sbin/nvram unset miio_ssid > /dev/null 2>&1
    /usr/sbin/nvram unset miio_passwd > /dev/null 2>&1
    /usr/sbin/nvram unset miio_uid > /dev/null 2>&1
    /usr/sbin/nvram unset miio_country > /dev/null 2>&1
    /usr/sbin/nvram commit > /dev/null 2>&1
}
save_tz_conf() {
	new_tz=$YOUR_TIMEZONE_DIR/$1
	echo $new_tz
	if [ -f $new_tz ]; then
		/usr/sbin/nvram set timezone="$1" > /dev/null 2>&1
		/usr/sbin/nvram commit > /dev/null 2>&1
		unlink $YOUR_LINK_TIMEZONE_FILE
		ln -sf  $new_tz $YOUR_LINK_TIMEZONE_FILE
		echo "timezone set success:$new_tz"
	else
		echo "timezone is not exist:$new_tz"
	fi
}

sanity_check() {
    if [ ! -e $WIFI_START_SCRIPT ]; then
	echo "Can't find wifi_start.sh: $WIFI_START_SCRIPT"
	echo 'Please change $WIFI_START_SCRIPT'
	exit 1
    fi
}

main() {
    while true; do
	BUF=`$MIIO_RECV_LINE`
	if [ $? -ne 0 ]; then
	    sleep 1;
	    continue
	fi
	if contains "$BUF" "_internal.info"; then
	    STRING=`wpa_cli status`

	    ifname=${STRING#*\'}
	    ifname=${ifname%%\'*}
	    echo "ifname: $ifname"

	    if [ "x$WIFI_SSID" != "x" ]; then
		ssid="$WIFI_SSID"
	    else
		ssid=`/usr/sbin/nvram get miio_ssid`
		WIFI_SSID="$ssid"
	    fi
	    # handle special char, e.g.: '"', '\'
	    # Here we're using sed, we might switch to jshon
	    ssid=$(echo $ssid | sed -e 's/^"/\\"/' | sed -e 's/\([^\]\)"/\1\\"/g' | sed -e 's/\([^\]\)"/\1\\"/g' | sed -e 's/\([^\]\)\(\\[^"\\\/bfnrtu]\)/\1\\\2/g' | sed -e 's/\([^\]\)\\$/\1\\\\/')
	    echo "ssid: $ssid"

	    bssid=${STRING##*bssid=}
	    bssid=`echo ${bssid} | cut -d ' ' -f 1 | tr '[:lower:]' '[:upper:]'`
	    echo "bssid: $bssid"

	    rssi=`iwconfig wlan0 |grep 'Signal'`
	    rssi=${rssi#*level=}
	    rssi=`echo $rssi | cut -d ' ' -f 1`
	    if [ "x$rssi" = "x" ]; then
		rssi=0
	    fi

	    ip=${STRING##*ip_address=}
	    ip=`echo ${ip} | cut -d ' ' -f 1`
	    echo "ip: $ip"

	    STRING=`ifconfig ${ifname}`

	    netmask=${STRING##*Mask:}
	    netmask=`echo ${netmask} | cut -d ' ' -f 1`
	    echo "netmask: $netmask"

	    gw=`route -n|grep 'UG'|tr -s ' ' | cut -f 2 -d ' '`
	    echo "gw: $gw"

	    # get vendor and then version
	    vendor=`/usr/sbin/nvram get vendor` | tr '[:lower:]' '[:upper:]'
	    sw_version=`grep "${vendor}_VERSION" /etc/os-release | cut -f 2 -d '='`
	    if [ -z $sw_version ]; then
		sw_version="unknown"
	    fi

	    msg="{\"method\":\"_internal.info\",\"partner_id\":\"\",\"params\":{\
\"hw_ver\":\"Linux\",\"fw_ver\":\"$sw_version\",\
\"ap\":{\
 \"ssid\":\"$ssid\",\"bssid\":\"$bssid\",\"rssi\":\"$rssi\"\
},\
\"netif\":{\
 \"localIp\":\"$ip\",\"mask\":\"$netmask\",\"gw\":\"$gw\"\
}}}"

	    echo $msg
	    $MIIO_SEND_LINE "$msg"
	elif contains "$BUF" "_internal.req_wifi_conf_status"; then
	    echo "Got _internal.req_wifi_conf_status"
	    req_wifi_conf_status "$BUF"
	    echo $REQ_WIFI_CONF_STATUS_RESPONSE
	    $MIIO_SEND_LINE "$REQ_WIFI_CONF_STATUS_RESPONSE"
	elif contains "$BUF" "_internal.wifi_start"; then
	    wificonf_dir2=${BUF##*\"datadir\":\"}
	    wificonf_dir2=${wificonf_dir2%%\"*}
	    miio_ssid=${BUF##*\"ssid\":\"}
	    miio_ssid=${miio_ssid%%\",\"passwd\":\"*}
	    miio_passwd=${BUF##*\",\"passwd\":\"}
	    miio_passwd=${miio_passwd%%\",\"uid\":\"*}
	    miio_uid=${BUF##*\",\"uid\":\"}
	    miio_uid=${miio_uid%%\"*}
	    miio_country=${BUF##*\",\"country_domain\":\"}
	    miio_country=${miio_country%%\",\"tz\":\"*}
	    tz=${BUF##*\",\"tz\":\"}
	    tz=${tz%%\"*}

	    save_wifi_conf "$miio_ssid" "$miio_passwd" "$miio_uid" "$miio_country"


	    CMD=$WIFI_START_SCRIPT
	    RETRY=1
	    WIFI_SUCC=1
	    until [ $RETRY -gt $WIFI_MAX_RETRY ]
	    do
		WIFI_SUCC=1
		echo "Retry $RETRY: CMD=${CMD}"
		${CMD} && break
		WIFI_SUCC=0

		if [ $WIFI_MAX_RETRY -eq 1 ]; then
		   break
		fi
		let RETRY=$RETRY+1
		sleep $WIFI_RETRY_INTERVAL
	    done

	    if [ $WIFI_SUCC -eq 1 ]; then
		msg="{\"method\":\"_internal.wifi_connected\"}"
		echo $msg
		$MIIO_SEND_LINE "$msg"
	    else
		clear_wifi_conf
		CMD=$WIFI_START_SCRIPT
		echo "Back to AP mode, CMD=${CMD}"
		${CMD}
		msg="{\"method\":\"_internal.wifi_ap_mode\",\"params\":null}";
		echo $msg
		$MIIO_SEND_LINE "$msg"
	    fi
	elif contains "$BUF" "_internal.request_dinfo"; then
	    echo "Got _internal.request_dinfo"
	    request_dinfo "$BUF"
	    echo $RESPONSE_DINFO
	    $MIIO_SEND_LINE "$RESPONSE_DINFO"
	elif contains "$BUF" "_internal.request_dtoken"; then
	    echo "Got _internal.request_dtoken"
	    request_dtoken "$BUF"
	    echo $RESPONSE_DCOUNTRY
	    $MIIO_SEND_LINE "$RESPONSE_DCOUNTRY"
	    # echo $RESPONSE_DTOKEN
	    $MIIO_SEND_LINE "$RESPONSE_DTOKEN"
	else
	    echo "Unknown cmd: $BUF"
	fi
    done
}

sanity_check
send_helper_ready
main
