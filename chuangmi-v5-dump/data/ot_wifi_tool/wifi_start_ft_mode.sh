#!/bin/sh

WPA_SUPPLICANT_CONFIG_FILE="/tmp/wpa_supplicant.conf"

update_wpa_conf()
{
    if [ x"$2" = x ]; then
    cat <<EOF > $WPA_SUPPLICANT_CONFIG_FILE
ctrl_interface=/var/run/wpa_supplicant
update_config=1

network={
        ssid="$1"
        key_mgmt=open
}
EOF
    else
    cat <<EOF > $WPA_SUPPLICANT_CONFIG_FILE
ctrl_interface=/var/run/wpa_supplicant
update_config=1

network={
        ssid="$1"
        psk="$2"
        key_mgmt=WPA-PSK
		proto=WPA WPA2
}
EOF
    fi
}

wifi_sta_mode()
{
    echo "Enabling wifi STA mode"

    get_ssid_passwd
    update_wpa_conf "$ssid" "$passwd"

    #stop uap interfce
    #ifconfig uap0 down
    killall udhcpc wpa_supplicant hostapd dhcpd
	
    ifconfig wlan0 up
    iwconfig wlan0 mode Managed
    mkdir -p /var/run/wpa_supplicant
    #wpa_supplicant -Dnl80211 -iwlan0 -c $WPA_SUPPLICANT_CONFIG_FILE -B
    wpa_supplicant -Dwext -iwlan0 -c$WPA_SUPPLICANT_CONFIG_FILE -d -B 
    sleep 2
    udhcpc -i wlan0

    # check if we've got ip
    echo "get ip addr :"
    ifconfig wlan0 | grep "inet addr" | cut -d ':' -f 2 |cut -d ' ' -f 1
}

get_ssid_passwd()
{
    ssid="miio_default_5G"
    key_mgmt="WPA"
    passwd="0x82562647"
}

start()
{
	wifi_sta_mode
}

start
