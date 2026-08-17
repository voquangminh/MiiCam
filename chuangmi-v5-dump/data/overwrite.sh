#!/bin/sh

FIRMWARE_NAME="/mnt/data/user_data"
echo "exec overwrite.sh..."
if [ -f "$FIRMWARE_NAME" ]; then 
	echo "${FIRMWARE_NAME} exist,will upgrade user partition!!!"
        mv ${FIRMWARE_NAME} /tmp/
        mkdir /tmp/imiApp
        sync
        tar -xvf /tmp/user_data -C /tmp/imiApp                          
        if [ $? = 0 ];then
	    #rm /mnt/data/* -rf
            mv /tmp/imiApp/os-release /tmp/
            if [ -d /mnt/data/sound ];then
                echo "/mnt/data/sound exit ..."
                rm -rf /tmp/imiApp/sound
  	            cp /tmp/imiApp/* /mnt/data -rf
            else
                echo "/mnt/data/sound not exit !!!"
  	            cp /tmp/imiApp/* /mnt/data -rf
            fi
       	    cp /tmp/os-release /mnt/data -rf
            sync
            reboot
	else
            echo "tar user_data fail"
       fi
else
       echo "$FIRMWARE_NAME not exist"
fi
