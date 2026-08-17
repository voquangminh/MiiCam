export PATH="/usr/bin:/usr/sbin:/bin:/sbin:/mnt/data/bin"

# ==================== CHUANGMI-V5 HARDWARE DYNAMIC DASHBOARD ====================
CAM_NAME=$(hostname)

# 1. Tu dong quet thong tin SoC, CPU va RAM
SOC_MODEL=$(awk -F': ' '/Hardware/ {print $2; exit}' /proc/cpuinfo)
if [ -z "$SOC_MODEL" ]; then SOC_MODEL="Unknown SoC"; fi
CPU_MODEL=$(awk -F': ' '/Processor/ {print $2; exit}' /proc/cpuinfo)
RAM_INFO=$(free -m | awk '/Mem:/ {print $2"MB total, "$4"MB free"}')
DISK_INFO=$(df -h | awk '/\/tmp\/sd$/ {print $2" total, "$4" free ("$5" used)"}')
if [ -z "$DISK_INFO" ]; then DISK_INFO="No SD Card mounted"; fi

#RAM_TOTAL=$(free | grep "Mem:" | sed 's/  */ /g' | cut -d' ' -f2)
#RAM_FREE=$(free | grep "Mem:" | sed 's/  */ /g' | cut -d' ' -f4)
#RAM_INFO="$((RAM_TOTAL / 1024))MB total, $((RAM_FREE / 1024))MB free"

# 2. Tu dong do tim ma chip WiFi thuc te dang chay tu lsmod va iwconfig
WIFI_SSID=$(iwconfig wlan0 2>/dev/null | awk -F'"' '/ESSID/ {print $2}')
if [ -z "$WIFI_SSID" ]; then WIFI_SSID="Disconnected"; fi
if lsmod | grep -q "8189"; then
    WIFI_CHIP="Realtek RTL8189FTV/FS"
elif lsmod | grep -q "mt76"; then
    WIFI_CHIP="Mediatek MT7601U"
else
    WIFI_CHIP="Generic WiFi Device"
fi

# 3. Tu dong do tim ma chip Audio Codec tu lsmod
if lsmod | grep -q "adda3"; then
    AUDIO_CHIP="Grain-Media ADDA308"
else
    AUDIO_CHIP="Built-in Kernel Audio"
fi

# 4. T DO TIM MAT CAM BIEN ANH (SENSOR) TU LSMOD DRIVER GOC
if lsmod | grep -q "ov9732"; then
    SENSOR_CODE="OmniVision OV9732 (720p)"
elif lsmod | grep -q "imx"; then
    SENSOR_CODE="SONY IMX Series"
elif lsmod | grep -q "ar0130"; then
    SENSOR_CODE="Aptina AR0130 Sensor"
else
    # Phuong an du phong neu ko co trong lsmod thi tim trong dmesg
    SENSOR_CODE=$(dmesg 2>/dev/null | grep -iE "sensor|imx|ov9|ar0" | grep -oE "imx[0-9]+|ov[0-9]+|ar[0-9]+" | head -n 1 | tr 'a-z' 'A-Z')
    if [ -z "$SENSOR_CODE" ]; then SENSOR_CODE="Generic CMOS Sensor"; fi
fi

DATE_NOW=$(date +"%d/%m/%Y")
UPTIME_CAM=$(uptime | sed 's/.*up \([^,]*\),.*/\1/' | sed 's/^[ \t]*//')
LOAD_AVG=$(uptime | sed 's/.*load average://' | sed 's/^[ \t]*//')

# T? d?ng tr?ch xu?t s? lu?ng phi?n dang nh?p Telnet/SSH v?t ly th?i gian th?c t? l?nh uptime/who
USER_COUNT=$(uptime | sed 's/.*, \([0-9]*\) user.*/\1/' | tr -d ' ')
[ -z "$USER_COUNT" ] && USER_COUNT=$(who 2>/dev/null | wc -l | tr -d ' ')
[ -z "$USER_COUNT" ] && USER_COUNT="1"
USER_RES="${USER_COUNT} active"

# Kh?i t?o ma tr?n 13 d?ng h? th?ng b?n ph?i, t? d?ng ch?n ma m?u xanh Cyan cho nhan ti?u d? tru?c d?u :
cat << EOT > /tmp/sys_info.txt
$(printf "\033[1;36m%-14s\033[0m: %s" "Firmware Hack" "chuangmi-v5-build")
$(printf "\033[1;36m%-14s\033[0m: %s" "Camera name" "$CAM_NAME")
$(printf "\033[1;36m%-14s\033[0m: %s" "SoC Plattform" "$SOC_MODEL")
$(printf "\033[1;36m%-14s\033[0m: %s" "Core CPU" "$CPU_MODEL")
$(printf "\033[1;36m%-14s\033[0m: %s" "Sensor Code" "$SENSOR_CODE")
$(printf "\033[1;36m%-14s\033[0m: %s" "Memory RAM" "$RAM_INFO")
$(printf "\033[1;36m%-14s\033[0m: %s" "Storage SD" "$DISK_INFO")
$(printf "\033[1;36m%-14s\033[0m: %s" "WiFi Network" "$WIFI_SSID")
$(printf "\033[1;36m%-14s\033[0m: %s" "WiFi Chipset" "$WIFI_CHIP")
$(printf "\033[1;36m%-14s\033[0m: %s" "Audio Codec" "$AUDIO_CHIP")
$(printf "\033[1;36m%-14s\033[0m: %s" "Current Date" "$DATE_NOW")
$(printf "\033[1;36m%-14s\033[0m: %s" "System Uptime" "$UPTIME_CAM")
$(printf "\033[1;36m%-14s\033[0m: %s" "Load Average" "$LOAD_AVG")
EOT

echo "==================== HARDWARE CONTROLLER STATUS ===================="
echo -e "  \033[1;34m[STREAM STATUS]\033[0m                           \033[1;30m|\033[0m \033[1;34m[SYSTEM OVERVIEW]\033[0m"

idx=1

# D?nh v? ch?nh x?c du?ng d?n c?ng command c?a Grain-Media theo logic chu?n c?a b?n
ISP_PATH="/proc/isp328/command"
if [ ! -f "$ISP_PATH" ]; then
    ISP_PATH="/proc/isp/command"
fi

# 2. VONG L?P TIEM L?NH VA D?C GIA TR? TH?C T? QUA C?NG COMMAND C?A B?N
for param in brightness contrast hue saturation denoise sharpness dr_mode daynight ae_en awb_en af_en sen_size users; do
    VAL=""
    
    if [ "$param" = "users" ]; then
        VAL="$USER_RES"
    elif [ -f "$ISP_PATH" ]; then
        # Th?c hi?n ghi l?nh d?c r v?o c?ng command c?a Grain-Media
        echo "r $param" > "$ISP_PATH"
        # H?ng gi? tr? tr? v? v? b?c t?ch l?m s?ch kho?ng tr?ng
        RAW_RES=$(cat "$ISP_PATH" 2>/dev/null)
        VAL=$(echo "$RAW_RES" | tr -d ' ' | tr -d '\r\n')
    fi
    
    if [ -z "$VAL" ]; then
        VAL="N/A"
    fi

    # C?t tr?i r?ng d?ng 38 ky t? d? v?ch d?c trung t?m th?ng d?ng t?p nhu k? ch?
    LEFT_TXT=$(printf "  \033[1;32m%-12s\033[0m: %-20s" "$param" "$RES")
    RIGHT_TXT=$(sed -n "${idx}p" /tmp/sys_info.txt 2>/dev/null)

    if [ ! -z "$RIGHT_TXT" ]; then
        echo -e "${LEFT_TXT} \033[1;30m|\033[0m ${RIGHT_TXT}"
    else
        echo -e "${LEFT_TXT}"
    fi
    idx=$((idx+1))
done
echo "===================================================================="
rm -f /tmp/sys_info.txt
