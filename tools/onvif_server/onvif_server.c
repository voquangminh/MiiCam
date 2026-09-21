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
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <strings.h>
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
#define SNAP_TRIGGER_FILE "/dev/shm/rtspd_snapshot"
#define SNAP_LAST_FILE "/dev/shm/rtspd_last_snapshot_path"
#define SNAP_LOCK_FILE "/dev/shm/rtspd_web_snapshot_lock"
#define SNAP_MIN_INTERVAL 3
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
static char pidfile_path[128] = "";
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
/* Read a whole file (binary-safe) into a malloc'd buffer. */
static int read_file_bin(const char *path,char **out,size_t *outlen){int fd=open(path,O_RDONLY);if(fd<0)return -1;size_t cap=65536,len=0;char*b=malloc(cap);if(!b){close(fd);return -1;}for(;;){if(len==cap){cap*=2;char*n=realloc(b,cap);if(!n){free(b);close(fd);return -1;}b=n;}ssize_t r=read(fd,b+len,cap-len);if(r<=0){if(r<0&&errno==EINTR)continue;break;}len+=(size_t)r;}close(fd);*out=b;*outlen=len;return 0;}
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
/* Parse an ISP proc reply ("128", "DR_LINEAR", "DAY_MODE", "enable", ...) to an
 * integer; -1 when the format is unknown. Numeric replies win over symbols. */
static int isp_reply_val(const char *reply){int v;static const char *one[]={"DR_WDR","NIGHT_MODE","NIGHT","enable","ON"},*zero[]={"DR_LINEAR","DAY_MODE","DAY","disable","OFF"};unsigned i;
	if(last_integer(reply,&v)==0)return v;
	for(i=0;i<sizeof(one)/sizeof(*one);i++){if(strstr(reply,one[i]))return 1;if(strstr(reply,zero[i]))return 0;}
	return -1;}
static int isp_get(const char *name,int *value){char cmd[128],reply[512];int rc=-1,v;if(!valid_isp_name(name)||!value){errno=EINVAL;return-1;}snprintf(cmd,sizeof(cmd),"r %s\n",name);pthread_mutex_lock(&isp_mutex);if(write_all_file(ISP_COMMAND,cmd)==0&&read_file(ISP_COMMAND,reply,sizeof(reply))==0&&(v=isp_reply_val(reply))>=0){*value=v;rc=0;}pthread_mutex_unlock(&isp_mutex);return rc;}
/* Write an ISP parameter and VERIFY the driver actually applied it. Some
 * parameters (dr_mode=1/WDR on this sensor) are rejected by the kernel with a
 * noisy "[ISP_ERR]: failed to execute" per attempt; refused (param,value)
 * pairs are remembered and short-circuited so we never re-trigger that spam,
 * while the ONVIF client gets an honest fault instead of fake success. */
static int isp_set(const char *name,int value){static struct{char n[24];int rej;}refused[16];static int rn;char cmd[128],rep[128]={0};int rc,i,ent=-1,cur;
	if(!valid_isp_name(name)){errno=EINVAL;return-1;}
	pthread_mutex_lock(&isp_mutex);
	for(i=0;i<rn;i++)if(strcmp(refused[i].n,name)==0){ent=i;break;}
	if(ent>=0&&refused[ent].rej==value){pthread_mutex_unlock(&isp_mutex);errno=EOPNOTSUPP;return-1;}
	snprintf(cmd,sizeof(cmd),"w %s %d\n",name,value);
	rc=write_all_file(ISP_COMMAND,cmd);
	if(rc==0){	/* proc gives no reply for writes - issue an explicit read to verify */
		snprintf(cmd,sizeof(cmd),"r %s\n",name);
		if(write_all_file(ISP_COMMAND,cmd)==0&&read_file(ISP_COMMAND,rep,sizeof(rep))==0){
			cur=isp_reply_val(rep);
			if(cur>=0&&cur!=value){if(ent<0&&rn<(int)(sizeof(refused)/sizeof(*refused))){ent=rn++;snprintf(refused[ent].n,sizeof(refused[ent].n),"%s",name);}refused[ent].rej=value;rc=-1;log_message("ISP","driver refused %s=%d (reply='%s', cached)",name,value,rep);}
		}
	}
	pthread_mutex_unlock(&isp_mutex);
	return rc;}
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

static void image_defaults(image_state_t *s){memset(s,0,sizeof(*s));s->brightness=s->contrast=s->saturation=s->sharpness=128;s->denoise=128;s->sensor_fps=15;s->ae_en=s->awb_en=1;}
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

    /* /dev/ftpwmtmr010 is a single-opener device: only one process may hold
     * it open at a time (subsequent open() returns EINVAL). onvif only needs
     * it to configure the MS41909 clock during init — actual PTZ moves go
     * through /dev/motor. The driver persists register state across close()
     * (same reason ir_led -e -> -s keeps working), so release it now to stop
     * starving ir_led/motor_ctrl/tracking for the daemon's whole lifetime. */
    close(pwm_fd);
    pwm_fd = -1;
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
    ptz.x += dx;
    ptz.y += dy;
    save_ptz();
    write_zoom();
    pthread_mutex_unlock(&motor_mutex);
    return 0;
fail:
    pthread_mutex_unlock(&motor_mutex);
    return -1;
}
static int motor_refresh(void){return 0;}
static void motor_stop(void){pthread_mutex_lock(&motor_mutex);ptz.moving=0;ptz.zvel=0;pthread_mutex_unlock(&motor_mutex);}
static void *motor_worker(void *unused){(void)unused;for(;;){int active,dx,dy;float z;pthread_mutex_lock(&motor_mutex);active=running&&ptz.moving;dx=ptz.dx;dy=ptz.dy;z=ptz.zvel;pthread_mutex_unlock(&motor_mutex);if(!active)break;if(z!=0.0f){pthread_mutex_lock(&motor_mutex);ptz.zoom+=z*0.02f;if(ptz.zoom<0.0f)ptz.zoom=0.0f;if(ptz.zoom>1.0f)ptz.zoom=1.0f;write_zoom();pthread_mutex_unlock(&motor_mutex);}if(dx||dy){if(motor_move_relative(dx,dy)<0)break;}else usleep(100000);}pthread_mutex_lock(&motor_mutex);ptz.moving=0;ptz.worker_active=0;pthread_mutex_unlock(&motor_mutex);return NULL;}
static void motor_continuous(int dx,int dy,float z){motor_stop();usleep(200000);pthread_mutex_lock(&motor_mutex);ptz.dx=dx;ptz.dy=dy;ptz.zvel=z;ptz.moving=(dx||dy||z!=0.0f);if(ptz.moving&&!ptz.worker_active){ptz.worker_active=1;pthread_create(&ptz.worker,NULL,motor_worker,NULL);pthread_detach(ptz.worker);}pthread_mutex_unlock(&motor_mutex);}
static const char *xml_find_open(const char *xml,const char *name){char a[96];const char*c=strchr(name,':');snprintf(a,sizeof(a),"<%s",name);for(;;){const char*p=strstr(xml,a);if(!p&&c){snprintf(a,sizeof(a),"<%s",c+1);p=strstr(xml,a);}if(!p)return NULL;p+=strlen(a);if(*p=='>'||*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='/')return p-strlen(a);xml=p;}}
static int xml_tag(const char *xml,const char *name,char *out,size_t size){char b[96];const char*p,*q;const char*c=strchr(name,':');p=xml_find_open(xml,name);if(!p)return-1;p=strchr(p,'>');if(!p)return-1;p++;snprintf(b,sizeof(b),"</%s>",name);q=strstr(p,b);if(!q&&c){snprintf(b,sizeof(b),"</%s>",c+1);q=strstr(p,b);}if(!q)return-1;size_t n=(size_t)(q-p);if(n>=size)n=size-1;memcpy(out,p,n);out[n]=0;return 0;}
static int xml_attr_float(const char *xml,const char *element,const char *attr,float *value){const char*p=strstr(xml,element);char key[32];if(!p)return-1;snprintf(key,sizeof(key),"%s=\"",attr);p=strstr(p,key);if(!p)return-1;*value=(float)atof(p+strlen(key));return 0;}
static void append(char*out,size_t size,const char*fmt,...){size_t used=strlen(out);va_list ap;if(used>=size-1)return;va_start(ap,fmt);vsnprintf(out+used,size-used,fmt,ap);va_end(ap);}
static const char *SOAP_HEAD="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:tmd=\"http://www.onvif.org/ver10/deviceIO/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>";
static const char *SOAP_TAIL="</s:Body></s:Envelope>";
static void soap_fault(char*out,size_t size,const char*reason){snprintf(out,size,"%s<s:Fault><s:Code><s:Value>s:Sender</s:Value></s:Code><s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>%s",SOAP_HEAD,reason,SOAP_TAIL);}
static int get_int_tag(const char*r,const char*n,int*v){char b[64];if(xml_tag(r,n,b,sizeof(b))<0)return-1;*v=atoi(b);return 0;}
static int set_optional(const char*r,const char*tag,const char*isp,int lo,int hi){int v;if(get_int_tag(r,tag,&v)<0)return 0;return set_checked(isp,v,lo,hi)==0?1:-1;}

/* ---- config.cfg ----
 * Read a KEY="value" / KEY=value line straight from /tmp/sd/config.cfg.
 * Returns 0 and fills out on success (quoting/whitespace stripped). */
static int cfg_read_key(const char *key, char *out, size_t size)
{
    FILE *f = fopen("/tmp/sd/config.cfg", "r");
    char line[256];
    int k;
    if (!f || !key || !out)
    {
        if (f) fclose(f);
        return -1;
    }
    out[0] = 0;
    k = (int)strlen(key);
    while (fgets(line, sizeof(line), f))
    {
        char *v, *e;
        if (strncmp(line, key, (size_t)k) != 0 || line[k] != '=')
            continue;
        v = line + k + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (*v == '"') v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == '"' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        strncpy(out, v, size - 1);
        out[size - 1] = 0;
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* ---- WS-UsernameToken authentication (optional, fails OPEN) ----
 * Auth is only enabled when both ONVIF_USER and ONVIF_PASS are set in
 * config.cfg. When disabled every request passes (backward compatible with
 * the pre-existing open server). When enabled the client must present a
 * valid <wsse:Security><wsse:UsernameToken> (PasswordText or PasswordDigest)
 * matching ONVIF_USER/ONVIF_PASS. */
static char onvif_user[64] = "";
static char onvif_pass[128] = "";
static int auth_required = 0;

/* ---- minimal SHA-1 (FIPS 180-4) ---- */
typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; } sha1_ctx;
static uint32_t sha1_rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }
static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80];
    uint32_t a, b, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (i = 16; i < 80; i++) { t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]; w[i] = sha1_rol(t, 1); }
    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    for (i = 0; i < 80; i++)
    {
        if (i < 20)      { t = (b & d) | (~b & e); k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;         k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e); k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;         k = 0xCA62C1D6; }
        t = sha1_rol(a, 5) + t + f + k + w[i];
        f = e; e = d; d = sha1_rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}
static void sha1_init(sha1_ctx *c) { c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->len = 0; }
static void sha1_update(sha1_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    while (n > 0)
    {
        size_t used = (size_t)(c->len % 64);
        size_t have = 64 - used;
        if (n >= have) { memcpy(c->buf + used, p, have); sha1_block(c, c->buf); c->len += have; p += have; n -= have; }
        else { memcpy(c->buf + used, p, n); c->len += n; n = 0; }
    }
}
static void sha1_final(sha1_ctx *c, unsigned char out[20])
{
    uint64_t bits = c->len * 8;
    unsigned char pad[128];
    size_t used = (size_t)(c->len % 64), want, i;
    pad[0] = 0x80;
    for (i = 1; i < sizeof(pad); i++) pad[i] = 0;
    want = (used < 56) ? 56 - used : 120 - used;
    sha1_update(c, pad, want);
    for (i = 0; i < 8; i++) pad[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(c, pad, 8);
    for (i = 0; i < 5; i++) { out[i * 4] = (unsigned char)(c->h[i] >> 24); out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16); out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8); out[i * 4 + 3] = (unsigned char)c->h[i]; }
}
/* ---- Base64 (RFC 4648) ---- */
static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const unsigned char *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3)
    {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[j++] = b64tab[(v >> 18) & 63]; out[j++] = b64tab[(v >> 12) & 63];
        out[j++] = b64tab[(v >> 6) & 63]; out[j++] = b64tab[v & 63];
    }
    if (i < len)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        out[j++] = b64tab[(v >> 18) & 63];
        out[j++] = b64tab[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? b64tab[(v >> 6) & 63] : '=';
        out[j++] = '=';
    }
    out[j] = 0;
}
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64_decode(const char *in, unsigned char *out, size_t *outlen)
{
    size_t o = 0, i;
    for (i = 0; in[i] && in[i] != '='; i += 4)
    {
        int a, b, c, d;
        if (!in[i + 1]) return -1;
        a = b64val(in[i]); b = b64val(in[i + 1]);
        c = b64val(in[i + 2]); d = b64val(in[i + 3]);
        if (a < 0 || b < 0) return -1;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
        if (c >= 0) out[o++] = (unsigned char)(((b & 0x0F) << 4) | (c >> 2));
        if (d >= 0) out[o++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    *outlen = o;
    return 0;
}
static int auth_check(const char *body)
{
    char user[64] = "", pass[128] = "", ptype[32] = "";
    char nonce[128] = "", created[64] = "";
    if (!auth_required) return 1;
    if (xml_tag(body, "wsse:Username", user, sizeof(user)) < 0 && xml_tag(body, "Username", user, sizeof(user)) < 0)
        return 0;
    if (xml_tag(body, "wsse:Password", pass, sizeof(pass)) < 0 && xml_tag(body, "Password", pass, sizeof(pass)) < 0)
        return 0;
    if (strcmp(user, onvif_user) != 0) return 0;
    if (strcasestr(body, "PasswordDigest")) strcpy(ptype, "digest");
    else if (strcasestr(body, "PasswordText")) strcpy(ptype, "text");
    else strcpy(ptype, "digest");
    if (strcmp(ptype, "text") == 0)
        return strcmp(pass, onvif_pass) == 0;
    /* PasswordDigest = base64(sha1(nonce + created + password)) */
    {
        unsigned char raw[64], sum[20];
        size_t rn = 0;
        char calc[64];
        sha1_ctx c;
        xml_tag(body, "wsse:Nonce", nonce, sizeof(nonce));
        xml_tag(body, "wsu:Created", created, sizeof(created));
        if (!created[0]) xml_tag(body, "Created", created, sizeof(created));
        if (nonce[0] && b64_decode(nonce, raw, &rn) < 0) rn = 0;
        sha1_init(&c);
        if (rn) sha1_update(&c, raw, rn);
        if (created[0]) sha1_update(&c, created, strlen(created));
        sha1_update(&c, onvif_pass, strlen(onvif_pass));
        sha1_final(&c, sum);
        b64_encode(sum, 20, calc);
        return strcmp(calc, pass) == 0;
    }
}

/* ---- current encoder state from the gmlib proc dump (same as codec_ctrl) ---- */
typedef struct { int w, h, fps, gop, bitrate, bitrate_max, rate_mode; } enc_state_t;
static void parse_int_after(const char *line, const char *key, int *out)
{
    const char *p = strstr(line, key);
    if (!p) return;
    p += strlen(key);
    while (*p == '(' || *p == ' ') p++;
    *out = atoi(p);
}
static void enc_get(enc_state_t *e)
{
    char *buf = NULL, *save = NULL, *line;
    size_t n = 0;
    int in_video = 0;
    e->w = 1280; e->h = 720; e->fps = 15; e->gop = 30;
    e->bitrate = 8192; e->bitrate_max = 10240; e->rate_mode = 1;
    if (read_file_bin("/proc/videograph/gmlib_setting", &buf, &n) < 0) return;
    line = strtok_r(buf, "\n", &save);
    while (line)
    {
        if (strstr(line, "H264E(")) in_video = 1;
        if (strstr(line, "AUDIO_ENC(")) in_video = 0;
        if (in_video)
        {
            parse_int_after(line, "dim_width(",  &e->w);
            parse_int_after(line, "dim_height(", &e->h);
            parse_int_after(line, "framerate(",  &e->fps);
            parse_int_after(line, "gop(",        &e->gop);
            parse_int_after(line, "bitrate_max(", &e->bitrate_max);
            parse_int_after(line, "bitrate(",    &e->bitrate);
            parse_int_after(line, "rate_mode(",  &e->rate_mode);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(buf);
    if (!e->bitrate || !e->w || !e->h) { e->bitrate = 8192; e->w = 1280; e->h = 720; }
}

/* ---- hostname / MAC / netmask helpers ---- */
static void get_hostname_str(char *out, size_t size)
{
    int fd = open("/proc/sys/kernel/hostname", O_RDONLY);
    ssize_t r;
    out[0] = 0;
    if (fd >= 0)
    {
        r = read(fd, out, size - 1);
        close(fd);
        if (r > 0)
        {
            out[r] = 0;
            out[strcspn(out, "\r\n")] = 0;
            if (out[0]) return;
        }
    }
    strncpy(out, "chuangmi720p", size - 1);
    out[size - 1] = 0;
}
static int set_hostname_str(const char *name)
{
    int fd;
    if (!name || !name[0]) return -1;
    fd = open("/proc/sys/kernel/hostname", O_WRONLY);
    if (fd < 0) return -1;
    write(fd, name, strlen(name));
    close(fd);
    return 0;
}
static void get_mac_str(char *out, size_t size)
{
    int fd;
    ssize_t r;
    out[0] = 0;
    fd = open("/sys/class/net/mlan0/address", O_RDONLY);
    if (fd < 0) fd = open("/sys/class/net/wlan0/address", O_RDONLY);
    if (fd < 0) { strncpy(out, "00:00:00:00:00:00", size - 1); return; }
    r = read(fd, out, size - 1);
    close(fd);
    if (r > 0) { out[r] = 0; out[strcspn(out, "\r\n")] = 0; }
    else strncpy(out, "00:00:00:00:00:00", size - 1);
}
static int net_prefix_len(const char *ifname)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q;
    uint32_t m;
    int bits = 24;
    if (fd < 0) return 24;
    memset(&q, 0, sizeof(q));
    strncpy(q.ifr_name, ifname, IFNAMSIZ - 1);
    q.ifr_addr.sa_family = AF_INET;
    if (ioctl(fd, SIOCGIFNETMASK, &q) == 0)
    {
        m = ((struct sockaddr_in *)&q.ifr_netmask)->sin_addr.s_addr;
        m = ntohl(m);
        bits = 0;
        while (m) { bits += (int)(m & 1); m >>= 1; }
        if (!bits) bits = 24;
    }
    close(fd);
    return bits;
}

/* ---- date/time helpers ---- */
static void fmt_date_time(char *out, size_t size, time_t t, int utc)
{
    struct tm tmv;
    struct tm *tm = utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv);
    if (!tm) { out[0] = 0; return; }
    snprintf(out, size, "<tt:Time><tt:Hour>%02d</tt:Hour><tt:Minute>%02d</tt:Minute><tt:Second>%02d</tt:Second></tt:Time><tt:Date><tt:Year>%04d</tt:Year><tt:Month>%02d</tt:Month><tt:Day>%02d</tt:Day></tt:Date>",
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}
static long long days_from_civil(int y, int m, int d)
{
    long long era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
static long ymdhms_to_time(int y, int m, int d, int h, int mn, int s)
{
    return (long)(days_from_civil(y, m, d) * 86400LL + h * 3600LL + mn * 60LL + s);
}
static int text_tag_n(const char *r, const char *tag, char *out, size_t size)
{
    const char *p = r;
    size_t n, tl = strlen(tag);
    while ((p = strstr(p, tag)) != NULL)
    {
        const char *bef = (p > r) ? p - 1 : r;
        if ((*bef == '<' || *bef == ':') && p[tl] == '>')
        {
            const char *val = p + tl + 1;
            const char *cl = strchr(val, '<');
            if (!cl) return -1;
            n = (size_t)(cl - val);
            if (n >= size) n = size - 1;
            memcpy(out, val, n);
            out[n] = 0;
            return 0;
        }
        p += tl;
    }
    return -1;
}
static int get_int_tag_n(const char *r, const char *tag, int *v)
{
    char b[64];
    if (text_tag_n(r, tag, b, sizeof(b)) < 0) return -1;
    *v = atoi(b);
    return 0;
}
static int utc_tz_offset(void)
{
    time_t now = time(NULL);
    struct tm gv, lv;
    long off;
    gmtime_r(&now, &gv);
    localtime_r(&now, &lv);
    off = (long)(lv.tm_hour - gv.tm_hour) * 3600 + (long)(lv.tm_min - gv.tm_min) * 60;
    return (int)off;
}

/* ---- rtspd ctrl file (one command line per send, like codec_ctrl) ---- */
static int ctrl_send(const char *cmd)
{
    int fd = open("/tmp/rtspd.ctrl", O_WRONLY | O_CREAT | O_APPEND, 0644);
    ssize_t w;
    size_t n;
    if (fd < 0) return -1;
    n = strlen(cmd);
    w = write(fd, cmd, n);
    close(fd);
    return w == (ssize_t)n ? 0 : -1;
}

/* ---- reboot after reply (detached worker so the HTTP reply goes out first) ---- */
static void *reboot_worker(void *arg)
{
    (void)arg;
    usleep(800000);
    sync();
    reboot(RB_AUTOBOOT);
    execl("/sbin/reboot", "reboot", (char *)NULL);
    execl("/bin/busybox", "busybox", "reboot", (char *)NULL);
    return NULL;
}

/* ---- live encoder config XML (shares encoder state with rtspd) ---- */
static void append_enc_cfg(char *out, size_t size, const enc_state_t *e)
{
    const char *prof = "Main";
    int kbps;
    if (e->w >= 1280 && e->h >= 720) prof = "High";
    else if (e->w <= 640) prof = "Baseline";
    kbps = e->bitrate;
    if (kbps < 32) kbps = 32;
    append(out, size,
        "<tt:VideoEncoderConfiguration token=\"venc_cfg_0\"><tt:Name>VideoEncoder</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution><tt:Quality>50</tt:Quality><tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl><tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>%s</tt:H264Profile></tt:H264><tt:Multicast><tt:Address><tt:Type>Multicast</tt:Type><tt:IPv4Address>239.255.255.250</tt:IPv4Address></tt:Address><tt:Port>37020</tt:Port><tt:TTL>5</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration>",
        e->w, e->h, e->fps, kbps, e->gop, prof);
}
static void append_profile_tail(char *out, size_t size)
{
    append(out, size,
        "<tt:AudioSourceConfiguration token=\"asrc_cfg_0\"><tt:Name>AudioSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>asrc_0</tt:SourceToken></tt:AudioSourceConfiguration>"
        "<tt:AudioEncoderConfiguration token=\"aenc_cfg_0\"><tt:Name>AudioEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>AAC</tt:Encoding><tt:Bitrate>16000</tt:Bitrate><tt:SampleRate>16000</tt:SampleRate><tt:Multicast><tt:Address><tt:Type>Multicast</tt:Type><tt:IPv4Address>239.255.255.250</tt:IPv4Address></tt:Address><tt:Port>37020</tt:Port><tt:TTL>5</tt:TTL><tt:AutoStart>true</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:AudioEncoderConfiguration>"
        "<tt:PTZConfiguration token=\"ptz_0\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>node_0</tt:NodeToken></tt:PTZConfiguration>");
}

static void handle_soap(const char*r,char*out,size_t size){out[0]=0;append(out,size,"%s",SOAP_HEAD);
 if(strstr(r,"GetDeviceInformation"))append(out,size,"<tds:GetDeviceInformationResponse><tds:Manufacturer>Xiaomi/Chuangmi</tds:Manufacturer><tds:Model>Mijia 1080p GM8136</tds:Model><tds:FirmwareVersion>ONVIF-full-1.0</tds:FirmwareVersion><tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>GM8136</tds:HardwareId></tds:GetDeviceInformationResponse>",endpoint_uuid);
 else if(strstr(r,"GetCapabilities")&&!strstr(r,"GetServiceCapabilities"))append(out,size,"<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device><tt:Media><tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media><tt:PTZ><tt:XAddr>http://%s:%d/onvif/ptz_service</tt:XAddr></tt:PTZ><tt:Imaging><tt:XAddr>http://%s:%d/onvif/imaging_service</tt:XAddr></tt:Imaging><tt:DeviceIO><tt:XAddr>http://%s:%d/onvif/deviceio_service</tt:XAddr></tt:DeviceIO></tds:Capabilities></tds:GetCapabilitiesResponse>",onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port());
 else if(strstr(r,"GetServices"))append(out,size,"<tds:GetServicesResponse><tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/ptz_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/imaging_service</tds:XAddr></tds:Service></tds:GetServicesResponse>",onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port(),onvif_host(),onvif_http_port());
 else if(strstr(r,"GetProfiles")){enc_state_t e;enc_get(&e);append(out,size,"<trt:GetProfilesResponse><trt:Profiles token=\"profile_0\" fixed=\"true\"><tt:Name>MainStream</tt:Name><tt:VideoSourceConfiguration token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/></tt:VideoSourceConfiguration>",e.w,e.h);append_enc_cfg(out,size,&e);append_profile_tail(out,size);append(out,size,"</trt:Profiles></trt:GetProfilesResponse>");}
 else if(strstr(r,"GetProfile")){enc_state_t e;enc_get(&e);append(out,size,"<trt:GetProfileResponse><trt:Profile token=\"profile_0\" fixed=\"true\"><tt:Name>MainStream</tt:Name><tt:VideoSourceConfiguration token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/></tt:VideoSourceConfiguration>",e.w,e.h);append_enc_cfg(out,size,&e);append_profile_tail(out,size);append(out,size,"</trt:Profile></trt:GetProfileResponse>");}
 else if(strstr(r,"GetAudioSources"))append(out,size,"<trt:GetAudioSourcesResponse><trt:AudioSources token=\"asrc_0\"><tt:Name>AudioSource</tt:Name><tt:Channels>1</tt:Channels></trt:AudioSources></trt:GetAudioSourcesResponse>");
 else if(strstr(r,"GetAudioSourceConfigurations"))append(out,size,"<trt:GetAudioSourceConfigurationsResponse><trt:AudioSourceConfigurations token=\"asrc_cfg_0\"><tt:Name>AudioSourceConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>asrc_0</tt:SourceToken></trt:AudioSourceConfigurations></trt:GetAudioSourceConfigurationsResponse>");
 else if(strstr(r,"GetAudioEncoderConfigurations"))append(out,size,"<trt:GetAudioEncoderConfigurationsResponse><trt:AudioEncoderConfigurations token=\"aenc_cfg_0\"><tt:Name>AudioEncoderConfig</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>AAC</tt:Encoding><tt:Bitrate>16000</tt:Bitrate><tt:SampleRate>16000</tt:SampleRate><tt:Multicast><tt:Address><tt:Type>Multicast</tt:Type><tt:IPv4Address>239.255.255.250</tt:IPv4Address></tt:Address><tt:Port>37020</tt:Port><tt:TTL>5</tt:TTL><tt:AutoStart>true</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></trt:AudioEncoderConfigurations></trt:GetAudioEncoderConfigurationsResponse>");
 else if(strstr(r,"GetAudioEncoderConfigurationOptions"))append(out,size,"<trt:GetAudioEncoderConfigurationOptionsResponse><trt:Options><tt:Encoding>AAC</tt:Encoding><tt:Bitrate><tt:Min>8000</tt:Min><tt:Max>192000</tt:Max></tt:Bitrate><tt:SampleRate><tt:Min>8000</tt:Min><tt:Max>48000</tt:Max></tt:SampleRate><tt:Multicast><tt:Address><tt:Type>Multicast</tt:Type><tt:IPv4Address>239.255.255.250</tt:IPv4Address></tt:Address><tt:Port>37020</tt:Port><tt:TTL>5</tt:TTL><tt:AutoStart>true</tt:AutoStart></tt:Multicast></trt:Options></trt:GetAudioEncoderConfigurationOptionsResponse>");
 else if(strstr(r,"GetVideoSources")&&!strstr(r,"Configuration")){enc_state_t e;enc_get(&e);append(out,size,"<trt:GetVideoSourcesResponse><trt:VideoSources token=\"source_0\"><tt:Framerate>%d</tt:Framerate><tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>",e.fps,e.w,e.h);}
 else if(strstr(r,"GetStreamUri"))append(out,size,"<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%d/live/ch00_0</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>",onvif_host(),onvif_rtsp_port());
 else if(strstr(r,"GetSnapshotUri"))append(out,size,"<trt:GetSnapshotUriResponse>""<trt:MediaUri>""<tt:Uri>http://%s:%d/snapshot.jpg</tt:Uri>""<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>""<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>""<tt:Timeout>PT60S</tt:Timeout>""</trt:MediaUri>""</trt:GetSnapshotUriResponse>",onvif_host(),onvif_http_port());
 else if(strstr(r,"GetNodes"))append(out,size,"<tptz:GetNodesResponse><tptz:PTZNode token=\"node_0\"><tt:Name>PanTiltZoom</tt:Name><tt:MaximumNumberOfPresets>%d</tt:MaximumNumberOfPresets><tt:HomeSupported>true</tt:HomeSupported><tt:SupportedPTZSpaces><tt:AbsolutePanTiltPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:URI></tt:AbsolutePanTiltPositionSpace><tt:AbsoluteZoomPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace</tt:URI></tt:AbsoluteZoomPositionSpace><tt:ContinuousZoomVelocitySpace><tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace</tt:URI></tt:ContinuousZoomVelocitySpace></tt:SupportedPTZSpaces></tptz:PTZNode></tptz:GetNodesResponse>",PRESET_MAX);
 else if(strstr(r,"GetConfigurations"))append(out,size,"<tptz:GetConfigurationsResponse>""<tptz:PTZConfiguration token=\"ptz_0\">""<tt:Name>PTZ</tt:Name>""<tt:UseCount>1</tt:UseCount>""<tt:NodeToken>node_0</tt:NodeToken>""</tptz:PTZConfiguration>""</tptz:GetConfigurationsResponse>");
 else if(strstr(r,"GetConfigurationOptions"))append(out,size,"<tptz:GetConfigurationOptionsResponse>""<tptz:PTZConfigurationOptions>""<tt:Spaces>""<tt:AbsolutePanTiltPositionSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""<tt:YRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:YRange>""</tt:AbsolutePanTiltPositionSpace>""<tt:RelativePanTiltTranslationSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""<tt:YRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:YRange>""</tt:RelativePanTiltTranslationSpace>""<tt:AbsoluteZoomPositionSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>0.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""</tt:AbsoluteZoomPositionSpace>""<tt:RelativeZoomTranslationSpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/ZoomSpaces/TranslationGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""</tt:RelativeZoomTranslationSpace>""<tt:ContinuousZoomVelocitySpace>""<tt:URI>""http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace""</tt:URI>""<tt:XRange>""<tt:Min>-1.0</tt:Min>""<tt:Max>1.0</tt:Max>""</tt:XRange>""</tt:ContinuousZoomVelocitySpace>""</tt:Spaces>""</tptz:PTZConfigurationOptions>""</tptz:GetConfigurationOptionsResponse>");
 else if(strstr(r,"GetCompatibleConfigurations")){append(out,size,"<tptz:GetCompatibleConfigurationsResponse>""<tptz:PTZConfiguration token=\"ptz_0\">""<tt:Name>TZ</tt:Name>""<tt:UseCount>1</tt:UseCount>""<tt:NodeToken>node_0</tt:NodeToken>""</tptz:PTZConfiguration>""</tptz:GetCompatibleConfigurationsResponse>");}
 else if(strstr(r,"GetStatus")&&!strstr(r,"Imaging")){char isots[32];time_t now=time(NULL);struct tm gtm;gmtime_r(&now,&gtm);strftime(isots,sizeof(isots),"%Y-%m-%dT%H:%M:%SZ",&gtm);motor_refresh();pthread_mutex_lock(&motor_mutex);append(out,size,"<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom x=\"%.4f\"/></tt:Position><tt:MoveStatus><tt:PanTilt>%s</tt:PanTilt><tt:Zoom>%s</tt:Zoom></tt:MoveStatus><tt:UtcTime>%s</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>",x_to_pan(ptz.x),y_to_tilt(ptz.y),ptz.zoom,ptz.moving?"MOVING":"IDLE",ptz.zvel!=0.0f?"MOVING":"IDLE",isots);pthread_mutex_unlock(&motor_mutex);}
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
  /* ---- media: video encoder configs (live, via gmlib + rtspd ctrl) ---- */
  else if(strstr(r,"GetVideoEncoderConfigurations")){enc_state_t e;enc_get(&e);append(out,size,"<trt:GetVideoEncoderConfigurationsResponse><trt:Configurations>");append_enc_cfg(out,size,&e);append(out,size,"</trt:Configurations></trt:GetVideoEncoderConfigurationsResponse>");}
  else if(strstr(r,"GetVideoEncoderConfigurationOptions"))append(out,size,"<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options><tt:Encoding>H264</tt:Encoding><tt:QualityRange><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:QualityRange><tt:Resolution><tt:Width><tt:Min>160</tt:Min><tt:Max>1280</tt:Max></tt:Width><tt:Height><tt:Min>120</tt:Min><tt:Max>720</tt:Max></tt:Height></tt:Resolution><tt:H264><tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>60</tt:Max></tt:GovLengthRange><tt:H264Profiles><tt:Profile>Baseline</tt:Profile><tt:Profile>Main</tt:Profile><tt:Profile>High</tt:Profile></tt:H264Profiles></tt:H264><tt:Extension><tt:JPEG><tt:ResolutionsAvailable/></tt:JPEG></tt:Extension></trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>");
  else if(strstr(r,"GetVideoEncoderConfiguration")){enc_state_t e;enc_get(&e);append(out,size,"<trt:GetVideoEncoderConfigurationResponse>");append_enc_cfg(out,size,&e);append(out,size,"</trt:GetVideoEncoderConfigurationResponse>");}
  else if(strstr(r,"SetVideoEncoderConfiguration")){int w=0,h=0,fr=0,kbps=0,gov=0,any=0;char pr[16]="";if(get_int_tag_n(r,"Width",&w)==0&&w)any=1;if(get_int_tag_n(r,"Height",&h)==0&&h)any=1;if(get_int_tag_n(r,"FrameRateLimit",&fr)==0&&fr)any=1;if(get_int_tag_n(r,"BitrateLimit",&kbps)==0&&kbps)any=1;if(get_int_tag_n(r,"GovLength",&gov)==0&&gov)any=1;if(text_tag_n(r,"H264Profile",pr,sizeof(pr))==0&&pr[0])any=1;if(!any){soap_fault(out,size,"No settings to apply");return;}if(w&&h){char cmd[64];if(w>1280)w=1280;if(h>720)h=720;if(w<160)w=160;if(h<120)h=120;snprintf(cmd,sizeof(cmd),"resolution %dx%d\n",w,h);ctrl_send(cmd);}if(fr){char cmd[32];if(fr>30)fr=30;if(fr<1)fr=1;snprintf(cmd,sizeof(cmd),"fps %d\n",fr);ctrl_send(cmd);}if(gov){char cmd[32];if(gov>60)gov=60;if(gov<1)gov=1;snprintf(cmd,sizeof(cmd),"gop %d\n",gov);ctrl_send(cmd);}if(kbps){char cmd[48];if(kbps<32)kbps=32;snprintf(cmd,sizeof(cmd),"bitrate %d\n",kbps);ctrl_send(cmd);}if(pr[0]){int profid=0;if(strcasestr(pr,"Baseline"))profid=66;else if(strcasestr(pr,"Extended")||strcasestr(pr,"88"))profid=0;else if(strcasestr(pr,"High"))profid=100;else if(strcasestr(pr,"Main"))profid=77;if(profid){char cmd[32];snprintf(cmd,sizeof(cmd),"h264profile %d\n",profid);ctrl_send(cmd);}}append(out,size,"<trt:SetVideoEncoderConfigurationResponse/>");}
  /* ---- media: video source configs / scopes ---- */
  else if(strstr(r,"GetVideoSourceConfigurations"))append(out,size,"<trt:GetVideoSourceConfigurationsResponse><trt:Configurations token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"1280\" height=\"720\"/></trt:Configurations></trt:GetVideoSourceConfigurationsResponse>");
  else if(strstr(r,"GetVideoSourceConfigurationOptions"))append(out,size,"<trt:GetVideoSourceConfigurationOptionsResponse><trt:Options><tt:BoundsRange><tt:Width><tt:Min>160</tt:Min><tt:Max>1280</tt:Max></tt:Width><tt:Height><tt:Min>120</tt:Min><tt:Max>720</tt:Max></tt:Height></tt:BoundsRange><tt:MaximumNumberOfProfiles>1</tt:MaximumNumberOfProfiles></trt:Options></trt:GetVideoSourceConfigurationOptionsResponse>");
  else if(strstr(r,"GetVideoSourceConfiguration"))append(out,size,"<trt:GetVideoSourceConfigurationResponse><trt:Configuration token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"1280\" height=\"720\"/></trt:Configuration></trt:GetVideoSourceConfigurationResponse>");
  else if(strstr(r,"GetScopes"))append(out,size,"<tds:GetScopesResponse><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/chuangmi720p</tt:ScopeItem></tds:Scopes><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/hardware/GM8136</tt:ScopeItem></tds:Scopes><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes><tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/type/ptz</tt:ScopeItem></tds:Scopes></tds:GetScopesResponse>");
  /* ---- device: date/time ---- */
  else if(strstr(r,"SetSystemDateAndTime")){int y=0,mo=0,d=0,h=0,mi=0,se=0;char tbs[24];long tzoff=0,t;struct timeval tv;if(get_int_tag_n(r,"Year",&y)<0||y<1970||y>2200){soap_fault(out,size,"Invalid DateTime");return;}get_int_tag_n(r,"Month",&mo);get_int_tag_n(r,"Day",&d);get_int_tag_n(r,"Hour",&h);get_int_tag_n(r,"Minute",&mi);get_int_tag_n(r,"Second",&se);if(text_tag_n(r,"TZ",tbs,sizeof(tbs))==0&&tbs[0]){const char*p=tbs;int sign=1,hh=0,mm=0;if(*p=='+'||*p=='-'){sign=(*p=='-')?-1:1;p++;}else if(strncmp(p,"UTC",3)==0){p+=3;if(*p=='+'||*p=='-'){sign=(*p=='-')?-1:1;p++;}}hh=atoi(p);{const char*cp=strchr(p,':');if(cp)mm=atoi(cp+1);}tzoff=(long)sign*(hh*3600+mm*60);}t=ymdhms_to_time(y,mo?mo:1,d?d:1,h,mi,se);if(!strstr(r,"UTCDateTime"))t-=tzoff;tv.tv_sec=(time_t)t;tv.tv_usec=0;if(settimeofday(&tv,NULL)<0){soap_fault(out,size,"settimeofday failed");return;}append(out,size,"<tds:SetSystemDateAndTimeResponse/>");}
  else if(strstr(r,"GetSystemDateAndTime")){time_t now=time(NULL);char utc[256],loc[256],tzs[24];int off=utc_tz_offset();fmt_date_time(utc,sizeof(utc),now,1);fmt_date_time(loc,sizeof(loc),now,0);snprintf(tzs,sizeof(tzs),"UTC%c%02d:%02d",off<0?'-':'+',(int)((off<0?-off:off)/3600),(int)(((off<0?-off:off)%3600)/60));append(out,size,"<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tds:DateTimeType>Manual</tds:DateTimeType><tds:DaylightSavings>false</tds:DaylightSavings><tds:TimeZone><tds:TZ>%s</tds:TZ></tds:TimeZone><tds:UTCDateTime>%s</tds:UTCDateTime><tds:LocalDateTime>%s</tds:LocalDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",tzs,utc,loc);}
  /* ---- device: hostname / network / users / reboot ---- */
  else if(strstr(r,"GetHostname")){char hn[64];get_hostname_str(hn,sizeof(hn));append(out,size,"<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>%s</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>",hn);}
  else if(strstr(r,"SetHostname")){char hn[64];if(text_tag_n(r,"Name",hn,sizeof(hn))<0){soap_fault(out,size,"Missing Name");return;}hn[strcspn(hn,"<>")]=0;if(set_hostname_str(hn)<0){soap_fault(out,size,"set hostname failed");return;}append(out,size,"<tds:SetHostnameResponse/>");}
  else if(strstr(r,"GetNetworkInterfaces")){char ip[64]="0.0.0.0",mac[32]="00:00:00:00:00:00";int pre=24;const char*ifn="mlan0";if(get_ip(ifn,ip,sizeof(ip))<0){ifn="wlan0";if(get_ip(ifn,ip,sizeof(ip))<0)strcpy(ip,"0.0.0.0");}get_mac_str(mac,sizeof(mac));pre=net_prefix_len(ifn);append(out,size,"<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"network_1\"><tt:Enabled>true</tt:Enabled><tt:Info><tt:Name>%s</tt:Name><tt:HwAddress>%s</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info><tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Manual><tt:Address>%s</tt:Address><tt:PrefixLength>%d</tt:PrefixLength></tt:Manual></tt:IPv4></tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>",ifn,mac,ip,pre);}
  else if(strstr(r,"GetNetworkProtocols"))append(out,size,"<tds:GetNetworkProtocolsResponse><tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port></tds:NetworkProtocols><tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port></tds:NetworkProtocols></tds:GetNetworkProtocolsResponse>",onvif_rtsp_port(),onvif_http_port());
  else if(strstr(r,"GetUsers")){if(auth_required)append(out,size,"<tds:GetUsersResponse><tds:User><tt:Username>%s</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User></tds:GetUsersResponse>",onvif_user);else append(out,size,"<tds:GetUsersResponse/>");}
  else if(strstr(r,"SystemReboot")){append(out,size,"<tds:SystemRebootResponse><tds:Message>System rebooting</tds:Message></tds:SystemRebootResponse>");{pthread_t rt;pthread_create(&rt,NULL,reboot_worker,NULL);pthread_detach(rt);}}
  /* ---- PTZ home + preset ops (home fields live in ptz struct) ---- */
  else if(strstr(r,"SetHomePosition")){pthread_mutex_lock(&motor_mutex);ptz.home_x=ptz.x;ptz.home_y=ptz.y;ptz.home_zoom=ptz.zoom;pthread_mutex_unlock(&motor_mutex);append(out,size,"<tptz:SetHomePositionResponse/>");}
  else if(strstr(r,"GotoHomePosition")){int mdx, mdy;float mdz;pthread_mutex_lock(&motor_mutex);mdx=ptz.home_x-ptz.x;mdy=ptz.home_y-ptz.y;mdz=ptz.home_zoom;pthread_mutex_unlock(&motor_mutex);motor_move_relative(mdx,mdy);pthread_mutex_lock(&motor_mutex);ptz.zoom=clampf(mdz,0.0f,1.0f);write_zoom();pthread_mutex_unlock(&motor_mutex);append(out,size,"<tptz:GotoHomePositionResponse/>");}
  else if(strstr(r,"GetHomePosition"))append(out,size,"<tptz:GetHomePositionResponse><tptz:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/><tt:Zoom x=\"%.4f\"/></tptz:Position></tptz:GetHomePositionResponse>",x_to_pan(ptz.home_x),y_to_tilt(ptz.home_y),ptz.home_zoom);
  else if(strstr(r,"RemovePreset")){char token[64];int p;if(xml_tag(r,"tptz:PresetToken",token,sizeof(token))==0){for(p=0;p<PRESET_MAX;p++){if(ptz.presets[p].used&&strcmp(ptz.presets[p].token,token)==0){pthread_mutex_lock(&motor_mutex);ptz.presets[p].used=0;save_ptz();pthread_mutex_unlock(&motor_mutex);break;}}}append(out,size,"<tptz:RemovePresetResponse/>");}
  /* ---- capabilities (dispatch by action prefix) ---- */
  else if(strstr(r,"GetServiceCapabilities")){if(strstr(r,"tptz"))append(out,size,"<tptz:GetServiceCapabilitiesResponse><tptz:Capabilities><tt:EFlip>false</tt:EFlip><tt:PanTiltSpaces>true</tt:PanTiltSpaces><tt:ZoomSpaces>true</tt:ZoomSpaces><tt:Presets>%d</tt:Presets><tt:AuxiliaryCommands>false</tt:AuxiliaryCommands></tptz:Capabilities></tptz:GetServiceCapabilitiesResponse>",PRESET_MAX);else if(strstr(r,"trt"))append(out,size,"<trt:GetServiceCapabilitiesResponse><trt:Capabilities><tt:ProfileCapabilities><tt:MaximumNumberOfProfiles>1</tt:MaximumNumberOfProfiles></tt:ProfileCapabilities><tt:StreamingCapabilities><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></trt:Capabilities></trt:GetServiceCapabilitiesResponse>");else append(out,size,"<tds:GetServiceCapabilitiesResponse><tds:Capabilities><tt:ProfileCapabilities><tt:MaximumNumberOfProfiles>1</tt:MaximumNumberOfProfiles></tt:ProfileCapabilities><tt:Network><tt:IPFilter>false</tt:IPFilter><tt:IZeroConfiguration>false</tt:IZeroConfiguration><tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network><tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2></tt:Security><tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve><tt:DiscoveryBye>false</tt:DiscoveryBye></tt:System></tds:Capabilities></tds:GetServiceCapabilitiesResponse>");}
else{soap_fault(out,size,"Action not supported");return;}append(out,size,"%s",SOAP_TAIL);}

static void http_reply_bytes(int fd,int code,const char*type,const char*body,size_t n){char h[512];int l=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nServer: GM8136-ONVIF\r\n\r\n",code,code==200?"OK":"Error",type,(unsigned long)n);send(fd,h,l,0);if(n)send(fd,body,n,0);}
static void http_reply(int fd,int code,const char*type,const char*body){http_reply_bytes(fd,code,type,body,body?strlen(body):0);}
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
    // * HTTP snapshot handler (rtspd produces the JPEG, like the web /api/snapshot)
    if(strstr(req,"GET /snapshot.jpg"))
    {
        struct stat st;
        int base_sig=-1,nowt=(int)time(NULL),last=0,elapsed=0;
        char stamp[16]={0},path[512]={0};
        if(stat(SNAP_LAST_FILE,&st)==0)base_sig=(int)st.st_mtime;
        if(read_file(SNAP_LOCK_FILE,stamp,sizeof(stamp))==0)last=atoi(stamp);
        if(nowt>=last+SNAP_MIN_INTERVAL){
            char sb[16];snprintf(sb,sizeof(sb),"%d\n",nowt);
            write_all_file(SNAP_LOCK_FILE,sb);
            int tfd=open(SNAP_TRIGGER_FILE,O_WRONLY|O_CREAT|O_TRUNC,0644);
            if(tfd>=0){write(tfd,"1",1);close(tfd);}
        }
        char *jpg=NULL;size_t jlen=0;
        while(elapsed<3000){
            if(stat(SNAP_LAST_FILE,&st)==0&&(base_sig<0||(int)st.st_mtime>base_sig)){
                path[0]=0;
                if(read_file(SNAP_LAST_FILE,path,sizeof(path))==0){
                    char *e=path+strlen(path);
                    while(e>path&&(e[-1]=='\n'||e[-1]=='\r'))*--e=0;
                    if(path[0]&&read_file_bin(path,&jpg,&jlen)==0)break;
                }
            }
            usleep(120000);
            elapsed+=120;
        }
        if(jpg&&jlen)http_reply_bytes(fd,200,"image/jpeg",jpg,jlen);
        else http_reply(fd,200,"image/jpeg","");
        free(jpg);
        close(fd);
        free(req);
        free(out);
        return NULL;
    }
    if(strstr(req,"POST /onvif/")){
        char*body=strstr(req,"\r\n\r\n");
        if(!auth_check(body?body+4:req)){
            soap_fault(out,MAX_RESPONSE,"Not authorized");
            http_reply(fd,401,"application/soap+xml; charset=utf-8",out);
        }else{
            handle_soap(body?body+4:req,out,MAX_RESPONSE);
            http_reply(fd,200,"application/soap+xml; charset=utf-8",out);
        }
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
        } else if (!strncmp(argv[i], "--user=", 7)) {
            snprintf(onvif_user, sizeof(onvif_user), "%s", argv[i] + 7);
        } else if (!strncmp(argv[i], "--pass=", 7)) {
            snprintf(onvif_pass, sizeof(onvif_pass), "%s", argv[i] + 7);
        } else if (!strncmp(argv[i], "-P", 2) && i + 1 < argc) {
            snprintf(pidfile_path, sizeof(pidfile_path), "%s", argv[i + 1]);
            i++;
        }
    }
    if (!onvif_user[0]) cfg_read_key("ONVIF_USER", onvif_user, sizeof(onvif_user));
    if (!onvif_pass[0]) cfg_read_key("ONVIF_PASS", onvif_pass, sizeof(onvif_pass));
    auth_required = (onvif_user[0] && onvif_pass[0]) ? 1 : 0;
    if (auth_required) log_message("INFO", "WS-UsernameToken auth ENABLED (user %s)", onvif_user);
    else log_message("INFO", "ONVIF auth DISABLED (set ONVIF_USER/ONVIF_PASS to enable)");
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
    if (pidfile_path[0]) {
        char pbuf[16];
        int pf;
        snprintf(pbuf,sizeof(pbuf),"%d\n",(int)getpid());
        pf=open(pidfile_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(pf>=0){int plen=(int)strlen(pbuf);write(pf,pbuf,(size_t)plen);close(pf);}
    }
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
    if (pidfile_path[0])
        unlink(pidfile_path);
    return 0;
}
