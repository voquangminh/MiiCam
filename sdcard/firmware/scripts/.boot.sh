#!/bin/sh
##################################################################################
## purpose: Start enabled services and stop with LED blinking                   ##
## license: GPLv3+, http://www.gnu.org/licenses/gpl-3.0.html                    ##
## author: Jan Sperling , 2017                                                  ##
##################################################################################

SD_MOUNTDIR="/tmp/sd"

if [ -r "${SD_MOUNTDIR}/firmware/scripts/functions.sh" ]
then
    . "${SD_MOUNTDIR}/firmware/scripts/functions.sh"
else
    echo "Unable to load basic functions"
    exit 1
fi

export LOGFILE="${LOGDIR}/ft_boot.log"
export LD_LIBRARY_PATH=/tmp/sd/firmware/lib

(

echo "*** Executing /mnt/data/test/boot.sh... "

##################################################################################
## Self-heal: restore missing bins if firmware/bin was clobbered                ##
##################################################################################

BINS_BACKUP="${SD_MOUNTDIR}/firmware/etc/_bins_all.tar.gz"
BINS_BACKUP2="/mnt/data/_bins_backup/fw_bins_all.tar.gz"

if [ -d "${SD_MOUNTDIR}/firmware/bin" ]
then
    MISSING=""
    for b in rtspd dropbear lighttpd onvif_server tracking camera_adjust codec_ctrl codec_ctl motor_ctrl motor_control chuangmi_ctrl blue_led yellow_led ir_led ir_cut nightmode flipmode mirrormode auto_night_mode take_snapshot take_video rtspd-v5 rtsp_audio_in aac_play arm-php-cgi dropbearkey sftp-server
    do
        if [ ! -f "${SD_MOUNTDIR}/firmware/bin/${b}" ]
        then
            MISSING="${MISSING} ${b}"
        fi
    done

    if [ -n "${MISSING}" ]
    then
        echo "*** firmware/bin incomplete (missing:${MISSING} )... "
        RESTORED=0
        for src in ${BINS_BACKUP} ${BINS_BACKUP2}
        do
            if [ -f "${src}" ]
            then
                echo "*** Restoring firmware/bin from ${src}... "
                tar xzf "${src}" -C "${SD_MOUNTDIR}/firmware/bin" && chmod +x ${SD_MOUNTDIR}/firmware/bin/*
                RESTORED=1
                break
            fi
        done
        if [ "${RESTORED}" -eq 0 ]
        then
            echo "*** WARNING: no usable binary backup found, services may fail!"
        fi
    fi

    ## FT payload files belong in /tmp/sd/ft, keep them out of firmware/bin
    for f in ft_boot.sh prikey.pem rsa_decrypt secret.bin
    do
        [ -f "${SD_MOUNTDIR}/firmware/bin/${f}" ] && rm -f "${SD_MOUNTDIR}/firmware/bin/${f}"
    done
fi

##################################################################################
## Put our bins into PATH                                                       ##
##################################################################################

if [ -d "${SD_MOUNTDIR}/firmware/bin" ] && ! mountpoint -q /tmp/sd/ft
then
    echo "*** Mounting ${SD_MOUNTDIR}/firmware/bin on /tmp/sd/ft... "
    mount --rbind "${SD_MOUNTDIR}/firmware/bin" /tmp/sd/ft
fi


##################################################################################
## Wait for network with booting                                                ##
##################################################################################

if [ "$WAIT_FOR_NETWORK" -eq 1 ]
then
    wait_for_network_until "$PING_RETRIES" "$PING_WAIT" "$PING_IP"
fi

##################################################################################
## Start enabled services                                                       ##
##################################################################################

####################################
## Status LED                     ##
####################################

## Blue on
/tmp/sd/firmware/bin/blue_led -e

## Disable the others (will be changed using restore state if required)
${SD_MOUNTDIR}/firmware/bin/yellow_led -d
${SD_MOUNTDIR}/firmware/bin/ir_led -d
${SD_MOUNTDIR}/firmware/bin/ir_cut -d

####################################
## Restore settings               ##
####################################

if [ "${RESTORE_STATE}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S10restore_state start
fi

####################################
## Telnetd                        ##
####################################

if [ "${ENABLE_TELNETD}" -eq 1 ] || [ "${ENABLE_SSHD}"  -eq 0 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S15telnet start

    if ! grep -q '^telnetd' /tmp/etc/restartd.conf
    then
        echo "telnetd \"/usr/sbin/telnetd\" \"${SD_MOUNTDIR}/firmware/etc/init/S15telnet restart\" \"/bin/echo '*** telnetd was restarted from restartd... '\"" >> /tmp/etc/restartd.conf
    fi
else
    sh ${SD_MOUNTDIR}/firmware/etc/init/S15telnet stop
fi

####################################
## Dropbear SSH                   ##
####################################

if [ "${ENABLE_SSHD}" -eq 1 ] || [ "${ENABLE_TELNETD}" -eq 0 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S20dropbear start

    if ! grep -q '^dropbear' /tmp/etc/restartd.conf
    then
        echo "dropbear \"${SD_MOUNTDIR}/firmware/bin/dropbear\" \"${SD_MOUNTDIR}/firmware/etc/init/S20dropbear restart\" \"/bin/echo '*** Dropbear SSH was restarted from restartd... '\"" >> /tmp/etc/restartd.conf
    fi
fi

####################################
## Crond                          ##
####################################

if [ "${ENABLE_CRON}" -eq 1 ]
then
    ## Load the crontab file by restarting the daemon
    if [ -s "${SD_MOUNTDIR}/firmware/etc/crontab" ]
    then
        sh ${SD_MOUNTDIR}/firmware/etc/init/S25crond restart
    fi

    ## Setup restartd
    if ! grep -q '^crond' /tmp/etc/restartd.conf
    then
        echo "crond \"/usr/sbin/crond\" \"${SD_MOUNTDIR}/firmware/etc/init/S25crond restart\" \"/bin/echo '*** crond was restarted from restartd... '\"" >> /tmp/etc/restartd.conf
    fi
else
    sh ${SD_MOUNTDIR}/firmware/etc/init/S25crond stop
fi

####################################
## NTPd                           ##
####################################

sh ${SD_MOUNTDIR}/firmware/etc/init/S30ntpd start

if ! grep -q '^ntpd' /tmp/etc/restartd.conf
then
    echo "ntpd \"/usr/sbin/ntpd\" \"${SD_MOUNTDIR}/firmware/etc/init/S30ntpd restart\" \"/bin/echo '*** NTPd was restarted from restartd... '\"" >> /tmp/etc/restartd.conf
fi

####################################
## Lighttpd Webserver             ##
####################################

if [ "${ENABLE_HTTPD}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S50lighttpd start
fi

####################################
## FTP server                     ##
####################################

if [ "${ENABLE_FTPD}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S55ftpd start
fi

####################################
## RTSP server                    ##
####################################

if [ "${ENABLE_RTSP}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S60rtsp start
fi

####################################
## ONVIF server                   ##
####################################

if [ "${ENABLE_ONVIF}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S65onvif start

    if ! grep -q '^onvif_server' /tmp/etc/restartd.conf
    then
        echo "onvif_server \"${SD_MOUNTDIR}/firmware/bin/onvif_server\" \"${SD_MOUNTDIR}/firmware/etc/init/S65onvif restart\" \"/bin/echo '*** ONVIF server was restarted from restartd... '\"" >> /tmp/etc/restartd.conf
    fi
fi

####################################
## Auto Night Mode                ##
####################################

if [ "${AUTO_NIGHT_MODE}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S70auto_night_mode start
else
    sh ${SD_MOUNTDIR}/firmware/etc/init/S70auto_night_mode stop

fi

####################################
## MQTT                           ##
####################################

if [ "${ENABLE_MQTT}" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S75mqtt-interval start
    sh ${SD_MOUNTDIR}/firmware/etc/init/S80mqtt-control  start
fi

####################################
## RestartD                       ##
####################################

if [ "$ENABLE_RESTARTD" -eq 1 ]
then
    sh ${SD_MOUNTDIR}/firmware/etc/init/S99restartd restart
else
    sh ${SD_MOUNTDIR}/firmware/etc/init/S99restartd stop
fi

####################################
## Ceiling camera mode            ##
####################################

if [ "$CEILING_MODE" -eq 1 ]
then
    /tmp/sd/firmware/bin/flipmode -e
    /tmp/sd/firmware/bin/mirrormode -e
fi

##################################################################################
## SSH dashboard profile      (persistent: copy from SD to tmpfs /etc)          ##
##################################################################################

cp /tmp/sd/firmware/etc/profile /etc/profile

##################################################################################
## Cleanup                                                                      ##
##################################################################################

if [ -f /mnt/data/test/boot.sh ]
then
    rm /mnt/data/test/boot.sh
fi


) >> "${LOGFILE}" 2>&1

