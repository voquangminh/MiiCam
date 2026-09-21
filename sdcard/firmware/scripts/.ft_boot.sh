#!/bin/sh

##################################################################################
## purpose: Initialize the Chuangmi 720P hack                                   ##
## license: GPLv3+, http://www.gnu.org/licenses/gpl-3.0.html                    ##
## author: Jan Sperling , 2017                                                  ##
##################################################################################

if [ ! -d "${LOGDIR}" ]
then
    mkdir -p "${LOGDIR}"
fi

##################################################################################
## Config                                                                       ##
##################################################################################

export LD_LIBRARY_PATH=/tmp/sd/firmware/lib
export SD_MOUNTDIR=/tmp/sd

if [ -f "${SD_MOUNTDIR}/config.cfg" ]
then
    . "${SD_MOUNTDIR}/config.cfg"
else
    echo "Config not found! Running normal boot!" | tee -a "${LOGFILE}"
    echo 0 > /tmp/ft_mode
    do_vg_boot
    exit
fi

if [ "${DISABLE_HACK}" -eq 1 ]
then
    echo "Hack disabled, proceed with default start" | tee -a "${LOGFILE}"
    echo 0 > /tmp/ft_mode
    do_vg_boot
    exit
fi

(
cat << EOF
################################
## Running Chuangmi 720P hack ##
################################

Chuangmi 720P configuration:

  HOSTNAME       = ${CAMERA_HOSTNAME}
  TIMEZONE       = ${TIMEZONE}

################################
EOF

##################################################################################
## Syslog                                                                       ##
##################################################################################

echo "*** Enabling logging"

if [ "$ENABLE_LOGGING" -eq 1 ]
then
    sh "${SD_MOUNTDIR}/firmware/etc/init/S01logging" restart
else
    sh "${SD_MOUNTDIR}/firmware/etc/init/S01logging" stop
fi

##################################################################################
## Make /etc writeable                                                          ##
##################################################################################

echo "*** Making /etc writable"

if ! [ -d /tmp/etc ]
then
    cp -r /etc /tmp/
    cp -r ${SD_MOUNTDIR}/firmware/etc/* /tmp/etc
fi

if ! mountpoint -q /etc
then
    mount --rbind /tmp/etc /etc
fi

##################################################################################
## Make /root writable                                                          ##
##################################################################################

echo "*** Mounting /root from sd card"

if ! [ -d /tmp/root ]
then
    cp -r ${SD_MOUNTDIR}/firmware/root /tmp/root
    chown -R root:root /tmp/root
    chmod -R 0750 /tmp/root
fi

if ! mountpoint -q /root
then
    mount --rbind /tmp/root /root
fi


##################################################################################
## WIFI                                                                         ##
##################################################################################

echo "*** Setting up WIFI configuration"

if [ -s "${SD_MOUNTDIR}/firmware/scripts/configure_wifi" ]
then
    echo "*** Configuring WIFI... "
    sh "${SD_MOUNTDIR}/firmware/scripts/configure_wifi"
fi


##################################################################################
## Mount GMLIB configuration                                                    ##
##################################################################################

echo "*** Setting up our own gmlib config"

if [ -f /tmp/sd/firmware/etc/gmlib.cfg ]
then
    mount --rbind /tmp/sd/firmware/etc/gmlib.cfg /gm/config/gmlib.cfg
fi

##################################################################################
## Mount VG boot configuration                                                  ##
## The stock /mnt/data/vg_boot.sh insmods the OV9732 sensor with fps=15, which  ##
## caps capture at 15 until the sensor accepts `w sen_fps 30` (only after warm- ##
## up). Ship the 30 fps insmod from SD and bind it over the flash copy so the   ##
## sensor comes up at 30 from the very first frame.                             ##
##################################################################################

echo "*** Setting up our own vg_boot config"

if [ -f /tmp/sd/firmware/etc/vg_boot.sh ]
then
    mount --bind /tmp/sd/firmware/etc/vg_boot.sh /gm/config/vg_boot.sh
fi


##################################################################################
## Set root Password                                                            ##
##################################################################################

if [ -n "${ROOT_PASSWORD}" ]
then
    echo "*** Setting root password... "
    echo "root:${ROOT_PASSWORD}" | chpasswd
else
    echo "WARN: root password must be set for SSH and or Telnet"
fi


##################################################################################
## Set time zone                                                                ##
##################################################################################

if [ -n "${TIMEZONE}" ]
then
    echo "*** Configure time zone... "

    if [ -f "/usr/share/zoneinfo/uclibc/$TIMEZONE" ]
    then
        cp -f /usr/share/zoneinfo/uclibc/$TIMEZONE /etc/localtime
    fi

    rm /tmp/etc/TZ
    echo "${TIMEZONE}" > /tmp/etc/TZ
    export TZ="${TIMEZONE}"
fi


##################################################################################
## Set hostname and format /etc/hosts                                           ##
##################################################################################

if [ -n "${CAMERA_HOSTNAME}" ]
then
    echo "*** Setting hostname... "
    echo "${CAMERA_HOSTNAME}" > /etc/hostname
    hostname "${CAMERA_HOSTNAME}"

    echo "*** Configuring new /etc/hosts file... "
    echo -e "127.0.0.1 \tlocalhost\n127.0.1.1 \t$CAMERA_HOSTNAME\n\n" > /etc/hosts

    if [ -f "${SD_MOUNTDIR}/firmware/etc/hosts" ]
    then
        echo "*** Appending ${SD_MOUNTDIR}/firmware/etc/hosts to /etc/hosts"
        cat ${SD_MOUNTDIR}/firmware/etc/hosts >> /etc/hosts
    fi
fi

##################################################################################
## Prepare restartd.conf                                                        ##
##################################################################################

if ! mount | grep -q /mnt/data/imi/imi_init/S99restartd
then
    echo "*** Replacing restartd startup script and config with our own version"
    mount --bind /etc/init/S99restartd /mnt/data/imi/imi_init/S99restartd
fi

if [ ! -f /tmp/etc/restartd.conf.org ] && mountpoint -q /etc
then
    cp /mnt/data/restartd/restartd.conf /tmp/etc/restartd.conf.org
    cp /mnt/data/restartd/restartd.conf /tmp/etc/restartd.conf
fi

# Watch rtspd too, so a boot-time crash (audio/video graph race) self-heals.
if [ -f /tmp/etc/restartd.conf ] && ! grep -q '"\([^"]*\)rtspd"' /tmp/etc/restartd.conf
then
    echo "rtspd \"/tmp/sd/firmware/bin/rtspd\" \"/tmp/sd/firmware/etc/init/S60rtsp restart\" \"/bin/echo '*** rtspd was restarted from restartd... '\"" >> /tmp/etc/restartd.conf
fi


##################################################################################
## Disable Cloud Services and OTA                                               ##
##################################################################################

if [ "${DISABLE_CLOUD}" -eq 1 ]
then
    sh "${SD_MOUNTDIR}/firmware/etc/init/S35disable_cloud" start
    sh "${SD_MOUNTDIR}/firmware/etc/init/S40disable_ota" start

elif [ "${DISABLE_OTA}" -eq 1 ]
then
    sh "${SD_MOUNTDIR}/firmware/etc/init/S40disable_ota" start
else
    sh "${SD_MOUNTDIR}/firmware/etc/init/S40disable_ota" stop
fi


##################################################################################
## Start enabled Services                                                       ##
##################################################################################

if ! [ -f /mnt/data/test/boot.sh ]
then
    ln -s -f ${SD_MOUNTDIR}/firmware/scripts/.boot.sh /mnt/data/test/boot.sh
fi

) >> "${LOGFILE}" 2>&1
