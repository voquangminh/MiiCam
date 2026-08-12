/*
 * onvif_server_full.c - Chuangmi/GM8136 ONVIF server
 * Direct /dev/motor ioctl + ISP328 imaging + GPIO LED/IR-cut.
 * No mijiactrl, ledctl, system(), popen(), or shell dependency.
 *
 * Build:
 *   arm-linux-gcc -std=gnu99 -Os -Wall -Wextra -pthread \
 *       onvif_server_full.c -o onvif_server
 *
 * Endpoints:
 *   UDP 239.255.255.250:3702                 WS-Discovery
 *   http://CAMERA:8899/onvif/device_service  Device
 *   http://CAMERA:8899/onvif/media_service   Media
 *   http://CAMERA:8899/onvif/ptz_service     PTZ
 *   http://CAMERA:8899/onvif/imaging_service Imaging
 *   http://CAMERA:8899/onvif/deviceio_service custom LED operations
 *
 * This is an embedded interoperability implementation, not a claim of ONVIF
 * conformance. Run the official Device Test Tool before making such a claim.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#define HTTP_PORT 8899
#define RTSP_PORT 554
#define WSD_PORT 3702
#define WSD_GROUP "239.255.255.250"
#define MOTOR_DEVICE "/dev/motor"
#define ISP_COMMAND "/proc/isp328/command"
#define IRCUT_STATE "/var/run/ircut"
#define MAX_REQUEST 65536
#define MAX_RESPONSE 65536
#define X_MAX 31
#define Y_MAX 15
#define PRESET_MAX 16
#define STATE_FILE "/tmp/onvif_ptz.state"
/* Digital zoom state shared with the RTSP server (rtspd2MP.c):
 * line format: "<zoom> <pan> <tilt>"  (all normalized 0.0..1.0) */
#define ZOOM_STATE_FILE "/dev/shm/rtspd_zoom"

/* Recovered exactly from vendor motor.ko::motor_ioctl(). */
#define MOTOR_MAGIC 'M'
#define H_DIR_SET   _IOW(MOTOR_MAGIC,  3, int)
#define H_DIST_SET  _IOW(MOTOR_MAGIC,  4, int)
#define H_COORD_GET _IOW(MOTOR_MAGIC,  5, int)
#define H_COORD_SET _IOW(MOTOR_MAGIC,  6, int)
#define V_DIR_SET   _IOW(MOTOR_MAGIC, 23, int)
#define V_DIST_SET  _IOW(MOTOR_MAGIC, 24, int)
#define V_COORD_GET _IOW(MOTOR_MAGIC, 25, int)
#define V_COORD_SET _IOW(MOTOR_MAGIC, 26, int)
/* PWM controller */
#define PWM_DEVICE "/dev/ftpwmtmr010"
#define PWM_IOCTL_01 0x40047001UL
#define PWM_IOCTL_02 0x40047002UL
#define PWM_IOCTL_05 0x40307005UL
#define PWM_IOCTL_06 0x40307006UL
#define PWM_IOCTL_07 0x40307007UL
#define PWM_IOCTL_09 0x40307009UL
#define PWM_IOCTL_0E 0x4004700eUL
/* LED controller */
#define BLUE_LED_BRIGHTNESS  "/sys/class/leds/BLUE/brightness"
#define BLUE_LED_TRIGGER     "/sys/class/leds/BLUE/trigger"
#define BLUE_LED_DELAY_ON	 "/sys/class/leds/BLUE/delay_on"
#define BLUE_LED_DELAY_OFF   "/sys/class/leds/BLUE/delay_off"
#define RED_LED_BRIGHTNESS   "/sys/class/leds/RED/brightness"
#define RED_LED_TRIGGER		 "/sys/class/leds/RED/trigger"
#define RED_LED_DELAY_ON	 "/sys/class/leds/RED/delay_on"
#define RED_LED_DELAY_OFF    "/sys/class/leds/RED/delay_off"

enum {
    STATUS_LED_BLUE = 0,
    STATUS_LED_RED  = 1
};

enum {
    STATUS_LED_SOLID = 0,
    STATUS_LED_OFF   = 1,
    STATUS_LED_BLINK = 2
};

typedef struct {
    const char *brightness;
    const char *trigger;
    const char *delay_on;
    const char *delay_off;
} status_led_paths_t;

static const status_led_paths_t status_led_paths[] = {
    {
        BLUE_LED_BRIGHTNESS,
        BLUE_LED_TRIGGER,
        BLUE_LED_DELAY_ON,
        BLUE_LED_DELAY_OFF
    },
    {
        RED_LED_BRIGHTNESS,
        RED_LED_TRIGGER,
        RED_LED_DELAY_ON,
        RED_LED_DELAY_OFF
    }
};

static volatile sig_atomic_t running = 1;
static char local_ip[64] = "127.0.0.1";
static char public_host[128] = "";
static int public_http_port = 0;
static int public_rtsp_port = 0;
static char endpoint_uuid[96] = "urn:uuid:81360000-0000-4000-8000-000000000001";
static int pwm_fd = -1;
static int motor_fd = -1;
static pthread_mutex_t motor_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t isp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gpio_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint32_t value[12];
} pwm_config_t;

typedef char pwm_config_size_must_be_48[
    sizeof(pwm_config_t) == 48 ? 1 : -1
];

typedef struct { int used, x, y; float z; char token[32], name[64]; } preset_t;
typedef struct {
    int x, y, home_x, home_y;
    int moving, dx, dy, worker_active;
    float zoom, home_zoom, zvel;
    pthread_t worker;
    preset_t presets[PRESET_MAX];
} ptz_state_t;
static ptz_state_t ptz = { .x=15, .y=7, .home_x=15, .home_y=7, .home_zoom=0.0f };

static const char *onvif_host(void)
{
    return public_host[0] ? public_host : local_ip;
}

static int onvif_http_port(void)
{
    return public_http_port > 0 ? public_http_port : HTTP_PORT;
}

static int onvif_rtsp_port(void)
{
    return public_rtsp_port > 0 ? public_rtsp_port : RTSP_PORT;
}

typedef struct {
    int brightness, contrast, hue, saturation, denoise, sharpness;
    int drc_strength, dr_mode, daynight;
    int ae_en, awb_en, af_en;
    int sensor_exposure, sensor_gain, sensor_fps;
    int mirror, flip, ircut;
} image_state_t;

static int clampi(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static float clampf(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
static float x_to_pan(int x){return ((float)x*2.0f/X_MAX)-1.0f;}
static float y_to_tilt(int y){return ((float)y*2.0f/Y_MAX)-1.0f;}
static int pan_to_x(float p){p=clampf(p,-1,1);return (int)((p+1)*X_MAX/2+0.5f);}
static int tilt_to_y(float p){p=clampf(p,-1,1);return (int)((p+1)*Y_MAX/2+0.5f);}
static void signal_handler(int sig){(void)sig;running=0;}
static void log_message(const char *level,const char *fmt,...){va_list ap;fprintf(stderr,"%s onvif: ",level);va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc('\n',stderr);}

static int write_all_file(const char *path,const char *text){int fd,rc=0;size_t off=0,len=strlen(text);fd=open(path,O_WRONLY);if(fd<0)return-1;while(off<len){ssize_t n=write(fd,text+off,len-off);if(n<0){if(errno==EINTR)continue;rc=-1;break;}off+=(size_t)n;}if(close(fd)<0&&rc==0)rc=-1;return rc;}
static int write_sysfs_string(const char *path,const char *value){int rc;if (!path || !value) {errno = EINVAL;return -1;}rc = write_all_file(path, value);if (rc < 0) {log_message("ERROR","write %s='%s' failed: %s",path,value,strerror(errno));}return rc;}
static int write_sysfs_int(const char *path,int value){char text[32];snprintf(text,sizeof(text),"%d\n",value);return write_sysfs_string(path, text);}
static int status_led_set(int led,int brightness,int mode,int delay_on_ms,int delay_off_ms)
{
    const status_led_paths_t *paths;
    int rc = 0;
    if (led < STATUS_LED_BLUE || led > STATUS_LED_RED) {
        errno = EINVAL;
        return -1;
    }
    if (brightness < 0 || brightness > 100) {
        errno = ERANGE;
        return -1;
    }
    if (mode < STATUS_LED_SOLID || mode > STATUS_LED_BLINK) {
        errno = EINVAL;
        return -1;
    }
    if (delay_on_ms < 0 || delay_off_ms < 0) {
        errno = ERANGE;
        return -1;
    }
    paths = &status_led_paths[led];
    pthread_mutex_lock(&gpio_mutex);
    switch (mode) {
    case STATUS_LED_SOLID:
        if (write_sysfs_string(paths->trigger,"none\n") < 0) {
            rc = -1;
            break;
        }
        if (write_sysfs_int(paths->brightness,brightness) < 0) {
            rc = -1;
        }
        break;
    case STATUS_LED_OFF:
        if (write_sysfs_string(paths->trigger,"none\n") < 0) {
            rc = -1;
            break;
        }
        if (write_sysfs_int(paths->brightness,0) < 0) {
            rc = -1;
        }
        break;
    case STATUS_LED_BLINK:
        if (delay_on_ms <= 0)
            delay_on_ms = 100;
        if (delay_off_ms <= 0)
            delay_off_ms = 100;
        if (write_sysfs_string(paths->trigger,"timer\n") < 0) {
            rc = -1;
            break;
        }
        if (write_sysfs_int(paths->delay_on,delay_on_ms) < 0) {
            rc = -1;
            break;
        }
        if (write_sysfs_int(paths->delay_off,delay_off_ms) < 0) {
            rc = -1;
            break;
        }
        if (write_sysfs_int(paths->brightness,brightness) < 0) {
            rc = -1;
        }
        break;
    }
    pthread_mutex_unlock(&gpio_mutex);
    if (rc == 0) {
        log_message("LED","led=%d brightness=%d mode=%d on=%d off=%d",led,brightness,mode,delay_on_ms,delay_off_ms);}
    return rc;
}
// * ISP funtions
static int read_file(const char *path,char *buf,size_t size){int fd;ssize_t n;if(!buf||size<2){errno=EINVAL;return-1;}fd=open(path,O_RDONLY);if(fd<0)return-1;do n=read(fd,buf,size-1);while(n<0&&errno==EINTR);close(fd);if(n<0)return-1;buf[n]=0;return 0;}
static int last_integer(const char *s,int *value){const char*p=s;char*e;long found=0;int have=0;while(*p){if(*p=='-'||isdigit((unsigned char)*p)){errno=0;long v=strtol(p,&e,0);if(e!=p&&errno==0){found=v;have=1;p=e;continue;}}p++;}if(!have){errno=EPROTO;return-1;}*value=(int)found;return 0;}
static int valid_isp_name(const char *s){if(!s||!*s)return 0;for(;*s;s++)if(!(isalnum((unsigned char)*s)||*s=='_'))return 0;return 1;}
static int isp_get(const char *name,int *value){char cmd[128],reply[512];int rc=-1;if(!valid_isp_name(name)||!value){errno=EINVAL;return-1;}snprintf(cmd,sizeof(cmd),"r %s\n",name);pthread_mutex_lock(&isp_mutex);if(write_all_file(ISP_COMMAND,cmd)==0&&read_file(ISP_COMMAND,reply,sizeof(reply))==0)rc=last_integer(reply,value);pthread_mutex_unlock(&isp_mutex);return rc;}
static int isp_set(const char *name,int value){char cmd[128];int rc;if(!valid_isp_name(name)){errno=EINVAL;return-1;}snprintf(cmd,sizeof(cmd),"w %s %d\n",name,value);pthread_mutex_lock(&isp_mutex);rc=write_all_file(ISP_COMMAND,cmd);pthread_mutex_unlock(&isp_mutex);return rc;}
// * LED functions 
static int blue_led_set(int enabled){return status_led_set(STATUS_LED_BLUE,enabled ? 100 : 0,enabled ? STATUS_LED_SOLID : STATUS_LED_OFF,0,0);}
static int yellow_led_set(int enabled){return status_led_set(STATUS_LED_RED,enabled ? 100 : 0,enabled ? STATUS_LED_SOLID : STATUS_LED_OFF,0,0);}
static int blue_led_blink(int delay_on_ms,int delay_off_ms){return status_led_set(STATUS_LED_BLUE,100,STATUS_LED_BLINK,delay_on_ms,delay_off_ms);}
static int yellow_led_blink(int delay_on_ms,int delay_off_ms){return status_led_set(STATUS_LED_RED,100,STATUS_LED_BLINK,delay_on_ms,delay_off_ms);}
// * IR LED functions
static int gpio_set(int pin,int value){char path[128],text[8];snprintf(path,sizeof(path),"/sys/class/gpio/gpio%d/value",pin);snprintf(text,sizeof(text),"%d\n",value?1:0);return write_all_file(path,text);}
static int gpio_get(int pin,int *value){char path[128],text[32];snprintf(path,sizeof(path),"/sys/class/gpio/gpio%d/value",pin);if(read_file(path,text,sizeof(text))<0)return-1;*value=atoi(text)?1:0;return 0;}
static int ircut_set(int enabled){int rc;char state[8];enabled=enabled?1:0;pthread_mutex_lock(&gpio_mutex);if(enabled){rc=gpio_set(14,1);if(rc==0)rc=gpio_set(15,0);}else{rc=gpio_set(14,0);if(rc==0)rc=gpio_set(15,1);}if(rc==0){snprintf(state,sizeof(state),"%d\n",enabled);rc=write_all_file(IRCUT_STATE,state);}pthread_mutex_unlock(&gpio_mutex);return rc;}
static int ircut_get(int *enabled){char state[16];int rc=0;pthread_mutex_lock(&gpio_mutex);if(read_file(IRCUT_STATE,state,sizeof(state))==0)*enabled=atoi(state)?1:0;else rc=gpio_get(14,enabled);pthread_mutex_unlock(&gpio_mutex);return rc;}

static void image_defaults(image_state_t *s){memset(s,0,sizeof(*s));s->brightness=s->contrast=s->saturation=s->sharpness=128;s->denoise=128;s->sensor_fps=20;s->ae_en=s->awb_en=1;}
static int image_get(image_state_t *s){int rc=0;image_defaults(s);
#define GET_FIELD(name,field)do{if(isp_get(name,&s->field)<0)rc=-1;}while(0)
    GET_FIELD("brightness",brightness);GET_FIELD("contrast",contrast);GET_FIELD("hue",hue);GET_FIELD("saturation",saturation);GET_FIELD("denoise",denoise);GET_FIELD("sharpness",sharpness);GET_FIELD("drc_strength",drc_strength);GET_FIELD("dr_mode",dr_mode);GET_FIELD("daynight",daynight);GET_FIELD("ae_en",ae_en);GET_FIELD("awb_en",awb_en);GET_FIELD("af_en",af_en);GET_FIELD("sen_exp",sensor_exposure);GET_FIELD("sen_gain",sensor_gain);GET_FIELD("sen_fps",sensor_fps);GET_FIELD("mirror",mirror);GET_FIELD("flip",flip);if(ircut_get(&s->ircut)<0)rc=-1;
#undef GET_FIELD
    return rc;
}

static int set_checked(const char*n,int v,int lo,int hi){return isp_set(n,clampi(v,lo,hi));}
static int get_ip(const char *name,char *out,size_t size){int fd=socket(AF_INET,SOCK_DGRAM,0);struct ifreq q;if(fd<0)return-1;memset(&q,0,sizeof(q));q.ifr_addr.sa_family=AF_INET;strncpy(q.ifr_name,name,IFNAMSIZ-1);if(ioctl(fd,SIOCGIFADDR,&q)<0){close(fd);return-1;}snprintf(out,size,"%s",inet_ntoa(((struct sockaddr_in*)&q.ifr_addr)->sin_addr));close(fd);return 0;}
static void make_uuid(void){FILE*f=fopen("/sys/class/net/mlan0/address","r");char mac[32]={0},hex[20]={0};int j=0;if(!f)f=fopen("/sys/class/net/wlan0/address","r");if(!f)return;fgets(mac,sizeof(mac),f);fclose(f);for(int i=0;mac[i]&&j<12;i++)if(isxdigit((unsigned char)mac[i]))hex[j++]=(char)tolower((unsigned char)mac[i]);if(j==12)snprintf(endpoint_uuid,sizeof(endpoint_uuid),"urn:uuid:81360000-0000-4000-8000-%s",hex);}
/* Publish zoom/pan/tilt to the RTSP server (caller holds motor_mutex).
 * zoom is normalized 0..1, pan/tilt are the normalized motor aim so the
 * digital zoom crop stays centered on where the camera points. */
static void write_zoom(void){FILE*f=fopen(ZOOM_STATE_FILE,"w");if(!f)return;fprintf(f,"%.4f %.4f %.4f\n",ptz.zoom,(float)ptz.x/(float)X_MAX,(float)ptz.y/(float)Y_MAX);fclose(f);}
static void save_ptz(void){FILE*f=fopen(STATE_FILE,"w");if(!f)return;fprintf(f,"%d %d %d %d %f\n",ptz.x,ptz.y,ptz.home_x,ptz.home_y,ptz.zoom);for(int i=0;i<PRESET_MAX;i++)if(ptz.presets[i].used)fprintf(f,"P %s %d %d %s %f\n",ptz.presets[i].token,ptz.presets[i].x,ptz.presets[i].y,ptz.presets[i].name,ptz.presets[i].z);fclose(f);}
static void load_ptz(void){FILE*f=fopen(STATE_FILE,"r");char line[256];if(!f)return;if(fgets(line,sizeof(line),f)){int n=sscanf(line,"%d %d %d %d %f",&ptz.x,&ptz.y,&ptz.home_x,&ptz.home_y,&ptz.zoom);if(n<5)ptz.zoom=0.0f;}while(fgets(line,sizeof(line),f)){char token[32],name[64];int x,y;float z=0.0f;int n=sscanf(line,"P %31s %d %d %63s %f",token,&x,&y,name,&z);if(n==4||n==5)for(int i=0;i<PRESET_MAX;i++)if(!ptz.presets[i].used){ptz.presets[i].used=1;ptz.presets[i].x=x;ptz.presets[i].y=y;ptz.presets[i].z=(n==5)?z:0.0f;snprintf(ptz.presets[i].token,32,"%s",token);snprintf(ptz.presets[i].name,64,"%s",name);break;}}fclose(f);}
//static int motor_ioctl_int(unsigned long cmd,int *value){int rc;if(motor_fd<0){errno=ENODEV;return-1;}log_message("IOCTL","cmd=0x%08lx value=%d",cmd,value ? *value : -1);do{ rc=ioctl(motor_fd,cmd,value);log_message("IOCTL","rc=%d errno=%d",rc,errno);}while(rc<0&&errno==EINTR);return rc;}
static int pwm_ioctl(unsigned long request, void *argument)
{
    int rc;
    if (pwm_fd < 0) {
        errno = ENODEV;
        return -1;
    }
    errno = 0;
    do {
        rc = ioctl(pwm_fd, request, argument);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        log_message("ERROR","PWM ioctl 0x%08lx failed: %s",request,strerror(errno));
    }
    return rc;
}
static int motor_pwm_init(void)
{
    pwm_config_t config[2];
    int channel;
    int rc;
    memset(config, 0, sizeof(config));
    /* Layout được khôi phục trực tiếp từ mijia_ctrl tại 0x9384. */
    config[0].value[0]  = 0;      /* PWM channel 0 */
    config[0].value[1]  = 1;
    config[0].value[2]  = 1;
    config[0].value[3]  = 0;
    config[0].value[4]  = 255;
    config[0].value[5]  = 127;
    config[0].value[6]  = 0;
    config[0].value[7]  = 0;
    config[0].value[8]  = 0;
    config[0].value[9]  = 0;
    config[0].value[10] = 1;
    config[0].value[11] = 127;

    /* Cấu hình PWM1 ban đầu giống PWM0, chỉ đổi channel index. */
    memcpy(&config[1],&config[0],sizeof(config[0]));
    config[1].value[0] = 1;
    pwm_fd = open(PWM_DEVICE, O_RDWR);
    if (pwm_fd < 0) {
        log_message("ERROR","open %s failed: %s",PWM_DEVICE,strerror(errno));
        return -1;
    }
    for (channel = 0; channel < 2; channel++) {
        rc = pwm_ioctl(PWM_IOCTL_01,&config[channel]);
        if (rc < 0)
            goto fail;
        rc = pwm_ioctl(PWM_IOCTL_05,&config[channel]);
        if (rc < 0)
            goto fail;
        rc = pwm_ioctl(PWM_IOCTL_09,&config[channel]);
        if (rc < 0)
            goto fail;
        rc = pwm_ioctl(PWM_IOCTL_0E,&config[channel]);
        if (rc < 0)
            goto fail;
        rc = pwm_ioctl(PWM_IOCTL_07,&config[channel]);
        if (rc < 0)
            goto fail;
    }
    /* mijia_ctrl ghi 0x00E4E1C0 = 15,000,000 vào offset 12 của cấu hình PWM1. */
    config[1].value[3] = 15000000U;
    rc = pwm_ioctl(PWM_IOCTL_06,&config[1]);
   if (rc < 0)
        goto fail;
   rc = pwm_ioctl(PWM_IOCTL_0E,&config[1]);
    if(rc < 0)
        goto fail;
    rc = pwm_ioctl(
        PWM_IOCTL_02,&config[1]);
    if (rc < 0)
        goto fail;
    return 0;

fail:
    close(pwm_fd);
    pwm_fd = -1;
    return -1;
}
static int motor_ioctl_int(unsigned long cmd, int *value)
{
    int rc;
    int saved_errno;
    if (motor_fd < 0) {
        errno = ENODEV;
        return -1;
    }
    if (value == NULL) {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    do {
        rc = ioctl(motor_fd, cmd, value);
    } while (rc < 0 && errno == EINTR);
    saved_errno = errno;
    errno = saved_errno;
    return rc;
}
static int motor_move_relative(int dx, int dy)
{
    int dir;
    int dist;
    int hpos;
    int vpos;
    int rc;
    pthread_mutex_lock(&motor_mutex);
    if (ptz.x + dx < 0)
        dx = -ptz.x;
    if (ptz.x + dx > X_MAX)
        dx = X_MAX - ptz.x;
    if (ptz.y + dy < 0)
        dy = -ptz.y;
    if (ptz.y + dy > Y_MAX)
        dy = Y_MAX - ptz.y;
    if (dx != 0) {
        dir = dx > 0 ? 0 : 1;
        dist = dx > 0 ? dx : -dx;
        rc = motor_ioctl_int(H_DIR_SET, &dir);
        if (rc < 0)
            goto fail;
        rc = motor_ioctl_int(H_DIST_SET, &dist);
        if (rc < 0)
            goto fail;
    }
    if (dy != 0) {
        dir = dy > 0 ? 1 : 0;
        dist = dy > 0 ? dy : -dy;
        rc = motor_ioctl_int(V_DIR_SET, &dir);
        if (rc < 0)
            goto fail;
        rc = motor_ioctl_int(V_DIST_SET, &dist);
        if (rc < 0)
            goto fail;
    }
    hpos = -1;
    vpos = -1;
    rc = motor_ioctl_int(H_COORD_GET, &hpos);
    if (rc < 0)
        goto fail;
    rc = motor_ioctl_int(V_COORD_GET, &vpos);
    if (rc < 0)
        goto fail;
    if (hpos < 0 || hpos > X_MAX) {
        errno = ERANGE;
        goto fail;
    }
    if (vpos < 0 || vpos > Y_MAX) {
        errno = ERANGE;
        goto fail;
    }
    ptz.x = hpos;
    ptz.y = vpos;
    save_ptz();
    write_zoom();
    pthread_mutex_unlock(&motor_mutex);
    return 0;
fail:
    pthread_mutex_unlock(&motor_mutex);
    return -1;
}
static int motor_refresh(void){int hpos = -1;int vpos = -1;int rc = 0;pthread_mutex_lock(&motor_mutex);if (motor_ioctl_int(H_COORD_GET, &hpos) < 0 || motor_ioctl_int(V_COORD_GET, &vpos) < 0) {rc = -1;} else if (hpos < 0 || hpos > X_MAX || vpos < 0 || vpos > Y_MAX) {errno = ERANGE;rc = -1;} else {ptz.x = hpos;ptz.y = vpos;save_ptz();write_zoom();}pthread_mutex_unlock(&motor_mutex);return rc;}
static void motor_stop(void){pthread_mutex_lock(&motor_mutex);ptz.moving=0;ptz.zvel=0;pthread_mutex_unlock(&motor_mutex);}
static void *motor_worker(void *unused){(void)unused;for(;;){int active,dx,dy;float z;pthread_mutex_lock(&motor_mutex);active=running&&ptz.moving;dx=ptz.dx;dy=ptz.dy;z=ptz.zvel;pthread_mutex_unlock(&motor_mutex);if(!active)break;if(z!=0.0f){pthread_mutex_lock(&motor_mutex);ptz.zoom+=z*0.02f;if(ptz.zoom<0.0f)ptz.zoom=0.0f;if(ptz.zoom>1.0f)ptz.zoom=1.0f;write_zoom();pthread_mutex_unlock(&motor_mutex);}if(dx||dy){if(motor_move_relative(dx,dy)<0)break;}else usleep(100000);}pthread_mutex_lock(&motor_mutex);ptz.moving=0;ptz.worker_active=0;pthread_mutex_unlock(&motor_mutex);return NULL;}
static void motor_continuous(int dx,int dy,float z){motor_stop();usleep(200000);pthread_mutex_lock(&motor_mutex);ptz.dx=dx;ptz.dy=dy;ptz.zvel=z;ptz.moving=(dx||dy||z!=0.0f);if(ptz.moving&&!ptz.worker_active){ptz.worker_active=1;pthread_create(&ptz.worker,NULL,motor_worker,NULL);pthread_detach(ptz.worker);}pthread_mutex_unlock(&motor_mutex);}
static int xml_tag(const char *xml,const char *name,char *out,size_t size){char a[96],b[96];const char*p,*q;snprintf(a,sizeof(a),"<%s",name);p=strstr(xml,a);if(!p){const char*c=strchr(name,':');if(c){snprintf(a,sizeof(a),"<%s",c+1);p=strstr(xml,a);}}if(!p)return-1;p=strchr(p,'>');if(!p)return-1;p++;snprintf(b,sizeof(b),"</%s>",name);q=strstr(p,b);if(!q){const char*c=strchr(name,':');if(c){snprintf(b,sizeof(b),"</%s>",c+1);q=strstr(p,b);}}if(!q)return-1;size_t n=(size_t)(q-p);if(n>=size)n=size-1;memcpy(out,p,n);out[n]=0;return 0;}
static int xml_attr_float(const char *xml,const char *element,const char *attr,float *value){const char*p=strstr(xml,element);char key[32];if(!p)return-1;snprintf(key,sizeof(key),"%s=\"",attr);p=strstr(p,key);if(!p)return-1;*value=(float)atof(p+strlen(key));return 0;}
static void append(char*out,size_t size,const char*fmt,...){size_t used=strlen(out);va_list ap;if(used>=size-1)return;va_start(ap,fmt);vsnprintf(out+used,size-used,fmt,ap);va_end(ap);}
static const char *SOAP_HEAD="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:tmd=\"http://www.onvif.org/ver10/deviceIO/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>";
static const char *SOAP_TAIL="</s:Body></s:Envelope>";
static void soap_fault(char*out,size_t size,const char*reason){snprintf(out,size,"%s<s:Fault><s:Code><s:Value>s:Sender</s:Value></s:Code><s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>%s",SOAP_HEAD,reason,SOAP_TAIL);}
static int get_int_tag(const char*r,const char*n,int*v){char b[64];if(xml_tag(r,n,b,sizeof(b))<0)return-1;*v=atoi(b);return 0;}
static int set_optional(const char*r,const char*tag,const char*isp,int lo,int hi){int v;if(get_int_tag(r,tag,&v)<0)return 0;return set_checked(isp,v,lo,hi)==0?1:-1;}
static void handle_soap(const char*r,char*out,size_t size){out[0]=0;append(out,size,"%s",SOAP_HEAD);
 if(strstr(r,"GetDeviceInformation"))append(out,size,"<tds:GetDeviceInformationResponse><tds:Manufacturer>Xiaomi/Chuangmi</tds:Manufacturer><tds:Model>Mijia 1080p GM8136</tds:Model><tds:FirmwareVersion>ONVIF-full-1.0</tds:FirmwareVersion><tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>GM8136</tds:HardwareId></tds:GetDeviceInformationResponse>",endpoint_uuid);
 else if(strstr(r,"GetCapabilities"))append(out,size,"<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device><tt:Media><tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media><tt:PTZ><tt:XAddr>http://%s:%d/onvif/ptz_service</tt:XAddr></tt:PTZ><tt:Imaging><tt:XAddr>http://%s:%d/onvif/imaging_service</tt:XAddr></tt:Imaging><tt:DeviceIO><tt:XAddr>http://%s:%d/onvif/deviceio_service</tt:XAddr></tt:DeviceIO></tds:Capabilities></tds:GetCapabilitiesResponse>",onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port());
 else if(strstr(r,"GetServices"))append(out,size,"<tds:GetServicesResponse><tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/ptz_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/imaging_service</tds:XAddr></tds:Service></tds:GetServicesResponse>",onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port());
 else if(strstr(r,"GetProfiles"))append(out,size,"<trt:GetProfilesResponse><trt:Profiles token=\"profile_0\" fixed=\"true\"><tt:Name>MainStream</tt:Name><tt:VideoSourceConfiguration token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/></tt:VideoSourceConfiguration><tt:PTZConfiguration token=\"ptz_0\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>node_0</tt:NodeToken></tt:PTZConfiguration></trt:Profiles></trt:GetProfilesResponse>");
 else if(strstr(r,"GetVideoSources"))append(out,size,"<trt:GetVideoSourcesResponse><trt:VideoSources token=\"source_0\"><tt:Framerate>20</tt:Framerate><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>");
 else if(strstr(r,"GetStreamUri"))append(out,size,"<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%d/live/ch00_0</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>",onvif_host(),onvif_rtsp_port());
 else if(strstr(r,"GetSnapshotUri"))append(out,size,"<trt:GetSnapshotUriResponse>""<trt:MediaUri>""<tt:Uri>http://%s:%d/snapshot.jpg</tt:Uri>""<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>""<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>""<tt:Timeout>PT60S</tt:Timeout>""</trt:MediaUri>""</trt:GetSnapshotUriResponse>",onvif_host(),onvif_http_port());
 else if(strstr(r,"GetNodes"))append(out,size,"<tptz:GetNodesResponse><tptz:PTZNode token=\"node_0\"><tt:Name>PanTiltZoom</tt:Name><tt:MaximumNumberOfPresets>%d</tt:MaximumNumberOfPresets><tt:HomeSupported>true</tt:HomeSupported><tt:SupportedPTZSpaces><tt:AbsolutePanTiltPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:URI></tt:AbsolutePanTiltPositionSpace><tt:AbsoluteZoomPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace</tt:URI></tt:AbsoluteZoomPositionSpace><tt:ContinuousZoomVelocitySpace><tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace</tt:URI></tt:ContinuousZoomVelocitySpace></tt:SupportedPTZSpaces></tptz:PTZNode></tptz:GetNodesResponse>",PRESET_MAX);
 else if(strstr(r,"GetConfigurations"))append(out,size,"<tptz:GetConfigurationsResponse>""<tptz:PTZConfiguration token=\"ptz_0\">""<tt:Name>PTZ</tt:Name>""<tt:UseCount>1</tt:UseCount>""<tt:NodeToken>node_0</tt:NodeToken>""</tptz:PTZConfiguration>""</tptz:GetConfigurationsResponse>");
 else if(strstr(r,"GetConfigurationOptions"))append(out,size,"<tptz:GetConfigurationOptionsResponse>""<tptz:PTZConfigurationOptions>""<tt:Spaces>""<tt:AbsolutePanTiltPositionSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""<tt:YRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:YRange>""</tt:AbsolutePanTiltPositionSpace>""<tt:RelativePanTiltTranslationSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""<tt:YRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:YRange>""</tt:RelativePanTiltTranslationSpace>""<tt:AbsoluteZoomPositionSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>0.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""</tt:AbsoluteZoomPositionSpace>""<tt:RelativeZoomTranslationSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/ZoomSpaces/TranslationGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""</tt:RelativeZoomTranslationSpace>""<tt:ContinuousZoomVelocitySpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""</tt:ContinuousZoomVelocitySpace>""</tt:Spaces>""</tptz:PTZConfigurationOptions>""</tptz:GetConfigurationOptionsResponse>");
 else if(strstr(r,"GetCompatibleConfigurations")){append(out,size,"<tptz:GetCompatibleConfigurationsResponse>""<tptz:PTZConfiguration token=\"ptz_0\">""<tt:Name>TZ</tt:Name>""<tt:UseCount>1</tt:UseCount>""<tt:NodeToken>node_0</tt:NodeToken>""</tptz:PTZConfiguration>""</tptz:GetCompatibleConfigurationsResponse>");}
 else if(strstr(r,"GetStatus")&&!strstr(r,"Imaging")){motor_refresh();pthread_mutex_lock(&motor_mutex);append(out,size,"<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom x=\"%.4f\"/></tt:Position><tt:MoveStatus><tt:PanTilt>%s</tt:PanTilt><tt:Zoom>%s</tt:Zoom></tt:MoveStatus><tt:UtcTime>2026-01-01T00:00:00Z</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>",x_to_pan(ptz.x),y_to_tilt(ptz.y),ptz.zoom,ptz.moving?"MOVING":"IDLE",ptz.zvel!=0.0f?"MOVING":"IDLE");pthread_mutex_unlock(&motor_mutex);}
 else if(strstr(r,"GetPresets")){int i;append(out,size,"<tptz:GetPresetsResponse>");pthread_mutex_lock(&motor_mutex);for(i = 0; i < PRESET_MAX; i++){if(!ptz.presets[i].used)continue;append(out,size,"<tptz:Preset token=\"%s\">""<tt:Name>%s</tt:Name>""<tt:PTZPosition>""<tt:PanTilt x=\"%.4f\" y=\"%.4f\"/>""<tt:Zoom x=\"%.4f\"/>""</tt:PTZPosition>""</tptz:Preset>",ptz.presets[i].token,ptz.presets[i].name,x_to_pan(ptz.presets[i].x),y_to_tilt(ptz.presets[i].y),ptz.presets[i].z);}pthread_mutex_unlock(&motor_mutex);append(out,size,"</tptz:GetPresetsResponse>");}
 else if(strstr(r,"SetPreset")){int idx;pthread_mutex_lock(&motor_mutex);for(idx = 0; idx < PRESET_MAX; idx++){if(!ptz.presets[idx].used)break;}if(idx < PRESET_MAX){ptz.presets[idx].used = 1;snprintf(ptz.presets[idx].token,sizeof(ptz.presets[idx].token),"preset_%d",idx);snprintf(ptz.presets[idx].name,sizeof(ptz.presets[idx].name),"Preset%d",idx);ptz.presets[idx].x = ptz.x;ptz.presets[idx].y = ptz.y;ptz.presets[idx].z = ptz.zoom;save_ptz();}pthread_mutex_unlock(&motor_mutex);append(out,size,"<tptz:SetPresetResponse>""<tptz:PresetToken>preset_%d</tptz:PresetToken>""</tptz:SetPresetResponse>",idx);}
 else if(strstr(r,"GotoPreset")){char token[64];int p;if(xml_tag(r,"tptz:PresetToken",token,sizeof(token)) == 0){for(p=0;p<PRESET_MAX;p++){if(ptz.presets[p].used && strcmp(ptz.presets[p].token,token) == 0){int mdx = ptz.presets[p].x - ptz.x;int mdy = ptz.presets[p].y - ptz.y;motor_move_relative(mdx, mdy);pthread_mutex_lock(&motor_mutex);ptz.zoom = clampf(ptz.presets[p].z,0.0f,1.0f);write_zoom();pthread_mutex_unlock(&motor_mutex);break;}}}append(out,size,"<tptz:GotoPresetResponse/>");}
 else if(strstr(r,"AbsoluteMove")){float x, y, z=0.0f;int tx, ty;int adx, ady;log_message("PTZ","AbsoluteMove request:%s",r);if(xml_attr_float(r,"PanTilt","x",&x) < 0 || xml_attr_float(r,"PanTilt","y",&y) < 0){log_message("PTZ","cannot parse PanTilt");soap_fault(out,size,"Invalid PanTilt");return;}tx = pan_to_x(x);ty = tilt_to_y(y);adx = tx - ptz.x;ady = ty - ptz.y;motor_stop();if(motor_move_relative(adx, ady) < 0){log_message("PTZ","motor_move_relative failed errno=%d (%s)",errno,strerror(errno));soap_fault(out,size,"Motor failure");return;}if(xml_attr_float(r,"Zoom","x",&z)==0){pthread_mutex_lock(&motor_mutex);ptz.zoom = clampf(z,0.0f,1.0f);write_zoom();pthread_mutex_unlock(&motor_mutex);}append(out,size,"<tptz:AbsoluteMoveResponse/>");} 
 else if(strstr(r,"RelativeMove")){float x,y,z=0.0f;int dx,dy;if(xml_attr_float(r,"PanTilt","x",&x) < 0 || xml_attr_float(r,"PanTilt","y",&y) < 0){soap_fault(out,size,"Invalid translation");return;}dx = (x > 0.0f) ? 1 : (x < 0.0f) ? -1 : 0;dy = (y > 0.0f) ? 1 : (y < 0.0f) ? -1 : 0;if(motor_move_relative(dx,dy) < 0){soap_fault(out,size,"Motor failure");return;}if(xml_attr_float(r,"Zoom","x",&z)==0){pthread_mutex_lock(&motor_mutex);ptz.zoom += z;ptz.zoom = clampf(ptz.zoom,0.0f,1.0f);write_zoom();pthread_mutex_unlock(&motor_mutex);}append(out,size,"<tptz:RelativeMoveResponse/>");} 
 else if(strstr(r,"ContinuousMove")){log_message("PTZ","ContinuousMove request:%s",r);float x=0,y=0,z=0;xml_attr_float(r,"PanTilt","x",&x);xml_attr_float(r,"PanTilt","y",&y);xml_attr_float(r,"Zoom","x",&z);log_message("PTZ","velocity x=%f y=%f z=%f",x,y,z);motor_continuous(x>0.05?1:(x<-0.05?-1:0),y>0.05?1:(y<-0.05?-1:0),z>0.05?1:(z<-0.05?-1:0));append(out,size,"<tptz:ContinuousMoveResponse/>");}
 else if(strstr(r,"<tptz:Stop")||strstr(r,"<Stop")){motor_stop();append(out,size,"<tptz:StopResponse/>");}
 else if(strstr(r,"GetImagingSettings")){image_state_t s;image_get(&s);append(out,size,"<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>%d</tt:Brightness><tt:ColorSaturation>%d</tt:ColorSaturation><tt:Contrast>%d</tt:Contrast><tt:Sharpness>%d</tt:Sharpness><tt:Exposure><tt:Mode>%s</tt:Mode><tt:ExposureTime>%d</tt:ExposureTime><tt:Gain>%d</tt:Gain></tt:Exposure><tt:WhiteBalance><tt:Mode>%s</tt:Mode></tt:WhiteBalance><tt:WideDynamicRange><tt:Mode>%s</tt:Mode><tt:Level>%d</tt:Level></tt:WideDynamicRange><tt:IrCutFilter>%s</tt:IrCutFilter><tt:Focus><tt:AutoFocusMode>%s</tt:AutoFocusMode></tt:Focus><tt:Extension><tt:Chuangmi><tt:Hue>%d</tt:Hue><tt:Denoise>%d</tt:Denoise><tt:DayNight>%d</tt:DayNight><tt:Mirror>%d</tt:Mirror><tt:Flip>%d</tt:Flip><tt:SensorFPS>%d</tt:SensorFPS></tt:Chuangmi></tt:Extension></timg:ImagingSettings></timg:GetImagingSettingsResponse>",s.brightness,s.saturation,s.contrast,s.sharpness,s.ae_en?"AUTO":"MANUAL",s.sensor_exposure,s.sensor_gain,s.awb_en?"AUTO":"MANUAL",s.dr_mode?"ON":"OFF",s.drc_strength,s.ircut?"ON":"OFF",s.af_en?"AUTO":"MANUAL",s.hue,s.denoise,s.daynight,s.mirror,s.flip,s.sensor_fps);}
 else if(strstr(r,"GetOptions")){append(out,size,"<timg:GetOptionsResponse><timg:ImagingOptions><tt:Brightness><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Brightness><tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:ColorSaturation><tt:Contrast><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Contrast><tt:Sharpness><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Sharpness><tt:Exposure><tt:Mode>AUTO</tt:Mode><tt:Mode>MANUAL</tt:Mode><tt:MinExposureTime>1</tt:MinExposureTime><tt:MaxExposureTime>10000</tt:MaxExposureTime><tt:MinGain>0</tt:MinGain><tt:MaxGain>8191</tt:MaxGain><tt:MinIris>0</tt:MinIris><tt:MaxIris>1024</tt:MaxIris></tt:Exposure><tt:WhiteBalance><tt:Mode>AUTO</tt:Mode><tt:Mode>MANUAL</tt:Mode></tt:WhiteBalance><tt:WideDynamicRange><tt:Mode>OFF</tt:Mode><tt:Mode>ON</tt:Mode><tt:Level><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Level></tt:WideDynamicRange><tt:IrCutFilterModes>ON</tt:IrCutFilterModes><tt:IrCutFilterModes>OFF</tt:IrCutFilterModes></timg:ImagingOptions></timg:GetOptionsResponse>");}
 else if(strstr(r,"SetImagingSettings")){int status=0,x;char b[64];int q; q=set_optional(r,"tt:Brightness","brightness",0,255);if(q<0)status=-1;q=set_optional(r,"tt:Contrast","contrast",0,255);if(q<0)status=-1;q=set_optional(r,"tt:ColorSaturation","saturation",0,255);if(q<0)status=-1;q=set_optional(r,"tt:Sharpness","sharpness",0,255);if(q<0)status=-1;
   if(xml_tag(r,"tt:Mode",b,sizeof(b))==0&&strstr(r,"Exposure"))if(isp_set("ae_en",!strcmp(b,"AUTO"))<0)status=-1;
   if(get_int_tag(r,"tt:ExposureTime",&x)==0&&set_checked("sen_exp",x,1,10000)<0)status=-1;
   if(get_int_tag(r,"tt:Gain",&x)==0&&set_checked("sen_gain",x,0,8191)<0)status=-1;
   if(strstr(r,"WhiteBalance")&&xml_tag(strstr(r,"WhiteBalance"),"tt:Mode",b,sizeof(b))==0)if(isp_set("awb_en",!strcmp(b,"AUTO"))<0)status=-1;
   if(strstr(r,"WideDynamicRange")&&xml_tag(strstr(r,"WideDynamicRange"),"tt:Mode",b,sizeof(b))==0)if(isp_set("dr_mode",!strcmp(b,"ON"))<0)status=-1;
   if(strstr(r,"WideDynamicRange")&&get_int_tag(strstr(r,"WideDynamicRange"),"tt:Level",&x)==0&&set_checked("drc_strength",x,0,255)<0)status=-1;
   if(xml_tag(r,"tt:IrCutFilter",b,sizeof(b))==0&&ircut_set(!strcmp(b,"ON"))<0)status=-1;
   q=set_optional(r,"tt:Hue","hue",-255,255);if(q<0)status=-1;q=set_optional(r,"tt:Denoise","denoise",0,255);if(q<0)status=-1;q=set_optional(r,"tt:DayNight","daynight",0,1);if(q<0)status=-1;q=set_optional(r,"tt:Mirror","mirror",0,1);if(q<0)status=-1;q=set_optional(r,"tt:Flip","flip",0,1);if(q<0)status=-1;q=set_optional(r,"tt:SensorFPS","sen_fps",1,30);if(q<0)status=-1;
   if(status<0){soap_fault(out,size,"Failed to apply imaging settings");return;}append(out,size,"<timg:SetImagingSettingsResponse/>");}
 else if(strstr(r,"SetBlueLEDBlink")){int on_ms = 500;int off_ms = 500;get_int_tag(r,"OnTime",&on_ms);get_int_tag(r,"OffTime",&off_ms);on_ms = clampi(on_ms,50,60000);off_ms = clampi(off_ms,50,60000);if(blue_led_blink(on_ms,off_ms) < 0){soap_fault(out,size,"Blue LED blink failed");return;}append(out,size,"<tmd:SetBlueLEDBlinkResponse/>");}
 else if(strstr(r,"SetBlueLED")){int v;if(get_int_tag(r,"Enabled",&v) < 0 || blue_led_set(v != 0) < 0){soap_fault(out,size,"Blue LED failed");return;}append(out,size,"<tmd:SetBlueLEDResponse/>");}
 else if(strstr(r,"SetYellowLEDBlink")){int on_ms = 500;int off_ms = 500;get_int_tag(r,"OnTime",&on_ms);get_int_tag(r,"OffTime",&off_ms);on_ms = clampi(on_ms,50,60000);off_ms = clampi(off_ms,50,60000);if(yellow_led_blink(on_ms,off_ms) < 0){soap_fault(out,size,"Yellow LED blink failed");return;}append(out,size,"<tmd:SetYellowLEDBlinkResponse/>");}
 else if(strstr(r,"SetYellowLED")){int v;if(get_int_tag(r,"Enabled",&v) < 0 || yellow_led_set(v != 0) < 0){soap_fault(out,size,"Yellow LED failed");return;}append(out,size,"<tmd:SetYellowLEDResponse/>");}
 else if(strstr(r,"SetIrCut")){int v;if(get_int_tag(r,"Enabled",&v) < 0 || ircut_set(v != 0) < 0){soap_fault(out,size,"IR-cut failed");return;}append(out,size,"<tmd:SetIrCutResponse/>");}
else{soap_fault(out,size,"Action not supported");return;}append(out,size,"%s",SOAP_TAIL);}

static void http_reply(int fd,int code,const char*type,const char*body){char h[512];size_t n=body?strlen(body):0;int l=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nServer: GM8136-ONVIF\r\n\r\n",code,code==200?"OK":"Error",type,(unsigned long)n);send(fd,h,l,0);if(n)send(fd,body,n,0);}
static void *client_thread(void*arg)
{
    int fd=(int)(intptr_t)arg,used=0,n;
    char*req=calloc(1,MAX_REQUEST),*out=calloc(1,MAX_RESPONSE);
    if(!req||!out){
        close(fd);
        free(req);
        free(out);
        return NULL;
    }
    while(used<MAX_REQUEST-1&&(n=recv(fd,req+used,MAX_REQUEST-1-used,0))>0){
        used+=n;
        req[used]=0;
        char*end=strstr(req,"\r\n\r\n");
        if(end){
            char*c=strcasestr(req,"Content-Length:");
            int len=c?atoi(c+15):0;
            if(used>=(int)(end+4-req)+len)break;
        }
    }
    // * HTTP snapshot handler
    if(strstr(req,"GET /snapshot.jpg"))
    {
        http_reply(fd,200,"image/jpeg","");
        close(fd);
        free(req);
        free(out);
        return NULL;
    }
    if(strstr(req,"POST /onvif/")){
        char*body=strstr(req,"\r\n\r\n");
        handle_soap(body?body+4:req,out,MAX_RESPONSE);
        http_reply(fd,200,"application/soap+xml; charset=utf-8",out);
    }
    else http_reply(fd,404,"text/plain","Not found\n");
    free(req);
    free(out);
    close(fd);
    return NULL;
}

static void *http_server(void*arg)
{
    (void)arg;
    int s=socket(AF_INET,SOCK_STREAM,0),one=1;
    struct sockaddr_in a;
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    memset(&a,0,sizeof(a));
    a.sin_family=AF_INET;
    a.sin_port=htons(HTTP_PORT);
    a.sin_addr.s_addr=INADDR_ANY;
    if(bind(s,(void*)&a,sizeof(a))<0||listen(s,16)<0){
        log_message("ERROR","HTTP %s",strerror(errno));
        return NULL;
    }
    while(running){
        int fd=accept(s,NULL,NULL);
        if(fd<0)continue;
        pthread_t t;
        if(!pthread_create(&t,NULL,client_thread,(void*)(intptr_t)fd))
            pthread_detach(t);
        else close(fd);
    }
    close(s);
    return NULL;
}

static void *discovery_server(void*arg)
{
    (void)arg;
    int s=socket(AF_INET,SOCK_DGRAM,0),
    one=1;
    struct sockaddr_in a;
    struct ip_mreq m;
    char b[8192];
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    memset(&a,0,sizeof(a));
    a.sin_family=AF_INET;
    a.sin_port=htons(WSD_PORT);
    a.sin_addr.s_addr=INADDR_ANY;
    if(bind(s,(void*)&a,sizeof(a))<0)
        return NULL;
    m.imr_multiaddr.s_addr=inet_addr(WSD_GROUP);
    m.imr_interface.s_addr=INADDR_ANY;
    setsockopt(s,IPPROTO_IP,IP_ADD_MEMBERSHIP,&m,sizeof(m));
    while(running){
        struct sockaddr_in from;
        socklen_t fl=sizeof(from);
        int n=recvfrom(s,b,sizeof(b)-1,0,(void*)&from,&fl);
        if(n<=0)continue;
        b[n]=0;
        if(!strstr(b,"Probe"))
            continue;
        char id[256]="urn:uuid:probe";
        xml_tag(b,"a:MessageID",id,sizeof(id));
        char x[4096];
        int l=snprintf(x,sizeof(x),"<?xml version=\"1.0\"?><e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><e:Header><a:MessageID>urn:uuid:%ld</a:MessageID><a:RelatesTo>%s</a:RelatesTo><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action></e:Header><e:Body><d:ProbeMatches><d:ProbeMatch><a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference><d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/ptz onvif://www.onvif.org/name/chuangmi-v2</d:Scopes><d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>",(long)time(NULL),id,endpoint_uuid,onvif_host(),onvif_http_port());
        sendto(s,x,l,0,(void*)&from,fl);
    }
    close(s);
    return NULL;
}

int main(int argc, char **argv)
{
    int i;
    pthread_t http,wsd;
    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--public-host=", 14)) {
            snprintf(public_host, sizeof(public_host), "%s", argv[i] + 14);
        } else if (!strncmp(argv[i], "--public-http-port=", 19)) {
            public_http_port = atoi(argv[i] + 19);
        } else if (!strncmp(argv[i], "--public-rtsp-port=", 19)) {
            public_rtsp_port = atoi(argv[i] + 19);
        }
    }
    if (public_http_port < 0 || public_http_port > 65535)
        public_http_port = 0;
    if (public_rtsp_port < 0 || public_rtsp_port > 65535)
        public_rtsp_port = 0;
    signal(SIGINT,signal_handler);
    signal(SIGTERM,signal_handler);
    signal(SIGPIPE,SIG_IGN);
    if(get_ip("mlan0",local_ip,sizeof(local_ip))<0)
        get_ip("wlan0",local_ip,sizeof(local_ip));
    make_uuid();
    load_ptz();
    write_zoom();
    if (motor_pwm_init() < 0) {
        log_message("ERROR","motor PWM is unavailable; physical PTZ may not move");
    }
    motor_fd=open(MOTOR_DEVICE,O_RDWR);
    if(motor_fd<0)
        log_message("WARN","open %s: %s",MOTOR_DEVICE,strerror(errno));
    else{
        motor_refresh();log_message("INFO","motor X=%d Y=%d",ptz.x,ptz.y);
    }
    pthread_create(&http,NULL,http_server,NULL);
    pthread_create(&wsd,NULL,discovery_server,NULL);
    log_message("INFO","device=%s http=%s:%d",endpoint_uuid,local_ip,HTTP_PORT);
    while(running)sleep(1);
    motor_stop();pthread_cancel(http);
    pthread_cancel(wsd);pthread_join(http,NULL);
    pthread_join(wsd,NULL);
    if (motor_fd >= 0) {
        close(motor_fd);
        motor_fd = -1;
    }
    if (pwm_fd >= 0) {
        close(pwm_fd);
        pwm_fd = -1;
    }
    return 0;
}
