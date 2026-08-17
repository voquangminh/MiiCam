CONFIG_PARTITION=$1
echo "vg_boot.sh, CONFIG_PARTITION: ${CONFIG_PARTITION}"
#### rm /tmp/busybox if it exist
rm /tmp/busybox


##########################################
# get product hid: mark hardware version when sensor,lens,ircut changed.
# IPC003_1=1 :( 8135s+sc1145 )
# IPC003_2=2 :( 8135s+ov9732 )
##########################################

#if [  "$product_hid" == "0002"  ] ; then
#	echo "sensor type is ov9732"
#        video_frontend=ov9732
#elif [ "$product_hid" == "2" ] ; then
#	echo "sensor type is ov9732"
#	video_frontend=ov9732
#else
#	echo "don't support this HID, default ov9732"
#	video_frontend=ov9732
#fi
video_frontend=ov9732
video_system=PAL

# Support video_front_end: ov2710, ov2715, ov9712, ov9715, ov9714, ov5653
# Support video_front_end: mt9m034, ar0130, ar0140, ar0330, ar0331
# Support video_front_end: imx222, imx238, imx236, imx238
# Support video_front_end: ov2710, ov2718

#####################################
#			reset key
#####################################
devmem 0x90c0005c 32 0x55004000
echo 39 > /sys/class/gpio/export
echo in > /sys/class/gpio/gpio39/direction

chipver=`head -1 /proc/pmu/chipver`
chipid=`echo $chipver | cut -c 1-4`

if [ "$chipid" != "8136" ] && [ "$chipid" != "8135" ]; then
    echo "Error! Not support chip version $chipver."
    exit
fi

if [ "$video_system" != "NTSC" ] && [ "$video_system" != "PAL" ]; then
    echo "Invalid argument for NTSC/PAL."
    exit
fi

if [ "$3" != "" ] ; then
    video_frontend=$3
fi

if [ "$video_frontend" != "ov2715" ] && [ "$video_frontend" != "ov2710" ] &&
   [ "$video_frontend" != "ov9710" ] && [ "$video_frontend" != "ov9712" ] &&
   [ "$video_frontend" != "ov9715" ] && [ "$video_frontend" != "ov9714" ] &&
   [ "$video_frontend" != "ar0130" ] && [ "$video_frontend" != "mt9m034" ] &&
   [ "$video_frontend" != "ar0140" ] && [ "$video_frontend" != "ar0141" ] &&
   [ "$video_frontend" != "ar0330" ] && [ "$video_frontend" != "ar0331" ] &&
   [ "$video_frontend" != "imx222" ] && [ "$video_frontend" != "imx124" ] &&
   [ "$video_frontend" != "imx238" ] && [ "$video_frontend" != "sc1145" ] &&
   [ "$video_frontend" != "ov9732" ] ; then
     echo "Invalid argument for video frontend: $video_frontend"
     exit
fi

insmod /lib/modules/3.3.0/frammap.ko
cat /proc/frammap/ddr_info

insmod /lib/modules/3.3.0/log.ko mode=0 log_ksize=4
insmod /lib/modules/3.3.0/ms.ko
insmod /lib/modules/3.3.0/em.ko
insmod /lib/modules/3.3.0/think2d.ko
insmod /lib/modules/3.3.0/flcd200-common.ko
insmod /lib/modules/3.3.0/flcd200-pip.ko output_type=0 fb0_fb1_share=1    # CVBS display
insmod /lib/modules/3.3.0/sar_adc.ko
# insmod /lib/modules/3.3.0/ftpwmtmr010.ko
insmod /lib/modules/3.3.0/fe_common.ko
insmod /lib/modules/3.3.0/adda308.ko input_mode=0 single_end=1 output_mode=1
insmod /lib/modules/3.3.0/ft3dnr200.ko src_yc_swap=1 dst_yc_swap=1 ref_yc_swap=1 config_path=${CONFIG_PARTITION}/


case "$video_frontend" in
    "sc1145")
        codec_max_width=1280
        codec_max_height=720
        if [ "$video_system" == "NTSC" ]  ; then
            insmod /lib/modules/3.3.0/fisp328.ko cfg_path=${CONFIG_PARTITION}/isp328_sc1145.cfg
            insmod /lib/modules/3.3.0/fisp_algorithm.ko exp_us=1 pwr_freq=0
            insmod /lib/modules/3.3.0/fisp_sc1145.ko sensor_w=1280 sensor_h=720 fps=15
        elif [ "$video_system" == "PAL" ] ; then
		insmod /lib/modules/3.3.0/fisp328.ko cfg_path=${CONFIG_PARTITION}/isp328_sc1145.cfg
		insmod /lib/modules/3.3.0/fisp_algorithm.ko exp_us=1 pwr_freq=1
		insmod /lib/modules/3.3.0/fisp_sc1145.ko sensor_w=1280 sensor_h=720 fps=15
        fi
        ;;
    "ov9732")
        codec_max_width=1280
        codec_max_height=720
        if [ "$video_system" == "NTSC" ]  ; then
            insmod /lib/modules/3.3.0/fisp328.ko cfg_path=${CONFIG_PARTITION}/isp328_ov9732.cfg
            insmod /lib/modules/3.3.0/fisp_algorithm.ko exp_us=1 pwr_freq=0
            insmod /lib/modules/3.3.0/fisp_ov9732.ko sensor_w=1280 sensor_h=720 fps=15
        elif [ "$video_system" == "PAL" ] ; then
		insmod /lib/modules/3.3.0/fisp328.ko cfg_path=${CONFIG_PARTITION}/isp328_ov9732.cfg
		insmod /lib/modules/3.3.0/fisp_algorithm.ko exp_us=1 pwr_freq=1
		insmod /lib/modules/3.3.0/fisp_ov9732.ko sensor_w=1280 sensor_h=720 fps=15
        fi
        ;;
    *)
        echo "Invalid argument for video frontend: $video_frontend"
        exit
        ;;
esac

insmod /lib/modules/3.3.0/vcap300_common.ko
insmod /lib/modules/3.3.0/vcap0.ko vi_mode=0,1 ext_irq_src=1
insmod /lib/modules/3.3.0/vcap300_isp.ko ch_id=0 range=1
insmod /lib/modules/3.3.0/fmcp_drv.ko mp4_tight_buf=1 config_path="${CONFIG_PARTITION}/"
insmod /lib/modules/3.3.0/favc_enc.ko h264e_max_b_frame=0 h264e_one_ref_buf=1 h264e_tight_buf=1 h264e_max_chn=6 h264e_max_width=$codec_max_width h264e_max_height=$codec_max_height h264e_slice_offset=1 config_path=${CONFIG_PARTITION}/
insmod /lib/modules/3.3.0/favc_rc.ko
insmod /lib/modules/3.3.0/decoder.ko
insmod /lib/modules/3.3.0/fmjpeg_drv.ko
#insmod /lib/modules/3.3.0/fmpeg4_drv.ko mp4_max_width=$codec_max_width mp4_max_height=$codec_max_height
#insmod /lib/modules/3.3.0/mp4e_rc.ko

# Encode 4CH + Cascade YUV 1CH
insmod /lib/modules/3.3.0/sw_osg.ko
insmod /lib/modules/3.3.0/fscaler300.ko max_vch_num=3 max_minors=3 temp_width=0 temp_height=0
#insmod /lib/modules/3.3.0/ftdi220.ko
insmod /lib/modules/3.3.0/osd_dispatch.ko
insmod /lib/modules/3.3.0/codec.ko
insmod /lib/modules/3.3.0/audio_drv.ko audio_ssp_num=0,1 audio_ssp_chan=1,1 bit_clock=400000,400000 sample_rate=8000,8000 audio_out_enable=1,0 audio_nr_enable=1,0
insmod /lib/modules/3.3.0/gs.ko reserved_ch_cnt=1 alloc_unit_size=65536 flow_mode=1
insmod /lib/modules/3.3.0/loop_comm.ko
insmod /lib/modules/3.3.0/vpd_slave.ko vpslv_dbglevel=0 ddr0_sz=0 ddr1_sz=0 config_path="${CONFIG_PARTITION}/" usr_func=0 usr_param=0 datain_minors=4 dataout_minors=8
insmod /lib/modules/3.3.0/vpd_master.ko vpd_dbglevel=0 gmlib_dbglevel=0

echo /mnt/nfs > /proc/videograph/dumplog   #configure log path
#mdev -s
#cat /proc/modules
echo 0 > /proc/frammap/free_pages   #should not free DDR1 for performance issue
echo 1 > /proc/vcap300/vcap0/dbg_mode  #need debug mode to detect capture overflow
echo 0 > /proc/videograph/em/dbglevel
echo 0 > /proc/videograph/gs/dbglevel
echo 0 > /proc/videograph/ms/dbglevel
echo 0 > /proc/videograph/datain/dbglevel
echo 0 > /proc/videograph/dataout/dbglevel
echo 0 > /proc/videograph/vpd/dbglevel
echo 0 > /proc/videograph/gmlib/dbglevel

#echo =========================================================================
#if [ -e ${CONFIG_PARTITION}/gmlib.cfg ] ; then
#grep ";" ${CONFIG_PARTITION}/gmlib.cfg |sed -n '1,1p'
#else
#grep ";" ${CONFIG_PARTITION}/spec.cfg |sed -n '2,6p'
#fi

#rootfs_date=`ls /|grep 00_2`
#mtd_date=`ls ${CONFIG_PARTITION}|grep 00_2`
#echo =========================================================================
#echo "  Video Front End: $video_frontend"
#echo "  Chip Version: $chipver"
#echo "  RootFS Version: $rootfs_date"
#echo "  MTD Version: $mtd_date"
#echo =========================================================================

devmem 0x9a1000a0 32 0x87878587
devmem 0x9a100034 32 0x061f0606
devmem 0x9a1000c4 32 0x08000f08
devmem 0x9a1000c8 32 0x061f0606
devmem 0x9a100030 32 0xDF000f04

#for led
devmem 0x91000008 32 0x0002c020

#SPEAKER
echo 17 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio17/direction
echo 0 > /sys/class/gpio/gpio17/value
# set volume down
echo -7 > /proc/adda308/LDAV
echo 1 > /proc/adda308/ALC_mode
echo -78 > /proc/adda308/ALCNGTH
echo 18 > /proc/adda308/ALCMAX
#imi ko
insmod /lib/modules/3.3.0/ftpwmtmr010.ko
insmod /lib/modules/3.3.0/key.ko

#IPC006 
#insmod /mnt/data/imi/imi_modules/motor.ko

#ir_led
devmem 0x90c00064 32 0x40005028
#
# Jieve
#
echo 3 >/proc/isp328/ae/hi_light_supp
echo 1 > /proc/isp328/awb/sta_mode
#echo 1000 1000 1000 > /proc/isp328/awb/target_rg

#h264
echo DefaultCfg 2 > /proc/videograph/h264e/param
echo DeltaQPWeight 4 > /proc/videograph/h264e/param

#thdnr
echo 0 85 > /proc/thdnr200/sharp_strength
echo 1 80 20 > /proc/thdnr200/hpf_gain_ratio

#vcap
echo 0 > /proc/vcap300/input_module/isp/data_range

#telnetd
#devmem 0x96105440 32 0x01500000
#devmem 0x96105438 32 0x01500000
#echo 1 0x50 > /proc/3dnr/dma/param
#echo w ae_en 0 > /proc/isp320/command
#echo w sen_exp 133 > /proc/isp320/command
#echo w fps 15 > /proc/isp320/command

# force max CPU performance
#echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

echo 1 > /sys/class/timed_output/ir-filter/direction
#/usr/sbin/ir_sample 200 5 2>&1 > /dev/null &

#for display cvbs
echo 18 > /proc/flcd200/pip/fb0_input
echo 1 > /proc/flcd200/tv_bot_shift

export LD_LIBRARY_PATH=/mnt/data/imi/lib
export LD_LIBRARY_PATH=/mnt/media/mmcblk0p1/openssl/lib:$LD_LIBRARY_PATH

### rtc sync to system
hwclock --hctosys

/mnt/data/test/tf_card_cycle_recovery.sh &
/mnt/data/test/test.sh

#manufacture
MANUFA="/mnt/media/mmcblk0p1/manufacture"
MANUFA_BIN="/mnt/media/mmcblk0p1/manufacture.bin"
MANUFA_DAT="/mnt/media/mmcblk0p1/manufacture.dat"

#write_num
WRITE_NUM="/mnt/media/mmcblk0p1/write_num"
WRITE_NUM_BIN="/mnt/media/mmcblk0p1/write_num.bin"
WRITE_NUM_DAT="/mnt/media/mmcblk0p1/write_num.dat"

#test_usb_socket
TEST_USB_SOCKET="/mnt/media/mmcblk0p1/test_usb_socket"
TEST_USB_SOCKET_BIN="/mnt/media/mmcblk0p1/test_usb_socket.bin"
TEST_USB_SOCKET_DAT="/mnt/media/mmcblk0p1/test_usb_socket.dat"

#test focus
TEST_FOCUS="/mnt/media/mmcblk0p1/test_focusing/"
TEST_FOCUS_BIN="/mnt/media/mmcblk0p1/test_focusing.bin"
TEST_FOCUS_DAT="/mnt/media/mmcblk0p1/test_focusing.dat"
SDCARD_DIR="/mnt/media/mmcblk0p1"

#openssl
OPENSSL="/mnt/data/imi/openssl"

#for manufature
if [ -f "$MANUFA_DAT" ] && [ ! -f "$WRITE_NUM_DAT" ] && [ ! -f "$TEST_FOCUS_DAT" ] && [ ! -f "$TEST_USB_SOCKET_DAT" ];
then
	rm -rf "$MANUFA_BIN"
        rm -rf "$MANUFA"
        echo " manufacture.dat"
	if [ ! -f "$OPENSSL" ];
	then
	     unzip /mnt/data/imi/openssl.zip -d /mnt/data/imi/
	fi
        ./mnt/data/imi/openssl smime -decrypt -in "$MANUFA_DAT" -binary -inform DEM -inkey /mnt/data/imi/private-key.pem -out "$MANUFA_BIN"
	if [ -f "$MANUFA_BIN" ];
	then
		tar -xf "$MANUFA_BIN" -C /tmp/
		cd /tmp/manufacture
		./test_drv &
	fi
#for write_num
elif [ ! -f "$MANUFA_DAT" ] && [ -f "$WRITE_NUM_DAT" ] && [ ! -f "$TEST_FOCUS_DAT" ] && [ ! -f "$TEST_USB_SOCKET_DAT" ];
then
	rm -rf "$WRITE_NUM_BIN"
        rm -rf "$WRITE_NUM"
        echo " write.dat"
	if [ ! -f "$OPENSSL" ];
	then
	     unzip /mnt/data/imi/openssl.zip -d /mnt/data/imi/
	fi
        ./mnt/data/imi/openssl smime -decrypt -in "$WRITE_NUM_DAT" -binary -inform DEM -inkey /mnt/data/imi/private-key.pem -out "$WRITE_NUM_BIN"
        if [ -f "$WRITE_NUM_BIN" ];
        then
		tar -xf "$WRITE_NUM_BIN" -C /tmp/
		cd /tmp/write_num
		./write_num &
	fi
#for test_focusing
elif [ ! -f "$MANUFA_DAT" ] && [ ! -f "$WRITE_NUM_DAT" ] && [ -f "$TEST_FOCUS_DAT" ] && [ ! -f "$TEST_USB_SOCKET_DAT" ];
then
        rm -rf "$TEST_FOCUS_BIN"
        rm -rf "$TEST_FOCUS"
        echo " test_focusing.dat"
	if [ ! -f "$OPENSSL" ];
	then
	     unzip /mnt/data/imi/openssl.zip -d /mnt/data/imi/
	fi
        ./mnt/data/imi/openssl smime -decrypt -in "$TEST_FOCUS_DAT" -binary -inform DEM -inkey /mnt/data/imi/private-key.pem -out "$TEST_FOCUS_BIN"
        if [ -f "$TEST_FOCUS_BIN" ];
        then
        	echo " test_focusing.bin"
        	tar -xf "$TEST_FOCUS_BIN" -C /tmp/

		cd /tmp/test_focusing
		insmod udc-core.ko
		insmod g_GM_udc.ko
		insmod g_ether.ko
		/mnt/data/miio_ota/pre-ota.sh
		sleep 1
		ipaddr=`cat /tmp/test_focusing/ipaddr`
		ifconfig usb0 ${ipaddr}
		./test_drv &
		echo 1 > /sys/class/gpio/gpio17/value
		./audio_playback kaishiceshi.wav
		sleep 3
		#echo 0 > /sys/class/gpio/gpio14/value
		#echo 1 > /sys/class/gpio/gpio15/value
		./rtspd  >/dev/null 2>&1
	fi
elif [ ! -f "$MANUFA_DAT" ] && [ ! -f "$WRITE_NUM_DAT" ] && [ ! -f "$TEST_FOCUS_DAT" ] && [ -f "$TEST_USB_SOCKET_DAT" ];
then
	rm -rf "$TEST_USB_SOCKET_BIN"
        rm -rf "$TEST_USB_SOCKET"
        echo " test_ubs_socket.dat"
	if [ ! -f "$OPENSSL" ];
	then
	     unzip /mnt/data/imi/openssl.zip -d /mnt/data/imi/
	fi
        ./mnt/data/imi/openssl smime -decrypt -in "$TEST_USB_SOCKET_DAT" -binary -inform DEM -inkey /mnt/data/imi/private-key.pem -out "$TEST_USB_SOCKET_BIN"
        if [ -f "$TEST_USB_SOCKET_BIN" ];
        then
		tar -xf "$TEST_USB_SOCKET_BIN" -C /tmp/
		cd /tmp/test_usb_socket
		insmod udc-core.ko
		insmod g_GM_udc.ko
		insmod g_ether.ko
		sleep 1
		ipaddr=`cat /tmp/test_usb_socket/ipaddr`
		ifconfig usb0 ${ipaddr}
		./test_usb_socket &
	fi
else
	if [ -f "$MANUFA_DAT" ] ;
	then
		cd /mnt/data/imi
		./audio_playback wufayunxing.wav
	elif [ -f "$WRITE_NUM_DAT" ] ;
	then
		cd /mnt/data/imi
		./audio_playback wufayunxing.wav
	elif [ -f "$TEST_FOCUS_DAT" ] ;
	then
		cd /mnt/data/imi
		./audio_playback wufayunxing.wav
	else
		echo "Normal Running !!"
	fi
fi

