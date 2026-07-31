/*
 * onvif_server_gm8136.c
 * Single-file ONVIF-lite device server for chuangmi/GM8136.
 *
 * Implements:
 *   - WS-Discovery Probe/ProbeMatches (UDP 3702)
 *   - Device: GetDeviceInformation, GetCapabilities, GetServices,
 *             GetSystemDateAndTime, GetHostname
 *   - Media: GetProfiles, GetVideoSources, GetVideoEncoderConfigurations,
 *            GetStreamUri, GetSnapshotUri
 *   - PTZ: GetNodes, GetConfigurations, GetConfigurationOptions, GetStatus,
 *          AbsoluteMove, RelativeMove, ContinuousMove, Stop,
 *          GetPresets, SetPreset, RemovePreset, GotoPreset,
 *          GotoHomePosition, SetHomePosition
 *   - Imaging: GetImagingSettings, GetOptions, SetImagingSettings,
 *              Move/Stop/GetStatus (reports unsupported fixed focus)
 *
 * This implementation intentionally calls mijiactrl directly with numeric
 * arguments. It does not guess /dev/motor ioctl values.
 *
 * Build:
 *   arm-linux-gcc -Os -Wall -Wextra -pthread -o onvif_server onvif_server_gm8136.c
 *
 * Run:
 *   ONVIF_USER=admin ONVIF_PASS=admin ./onvif_server
 *
 * Default endpoints:
 *   http://CAMERA_IP:8899/onvif/device_service
 *   rtsp://CAMERA_IP:554/live/ch00_0
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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ONVIF_HTTP_PORT 8899
#define ONVIF_DISCOVERY_PORT 3702
#define ONVIF_DISCOVERY_GROUP "239.255.255.250"
#define RTSP_PORT 554
#define MAX_REQ 65536
#define MAX_XML 32768
#define PTZ_X_MAX 31
#define PTZ_Y_MAX 15
#define PRESET_MAX 16
#define MOTOR_LOCK "/dev/shm/onvif_motor.lock"
#define STATE_FILE "/dev/shm/onvif_ptz_state"

#ifndef MIJIACTRL_PATH
#define MIJIACTRL_PATH "/tmp/sd/tools/bin/mijiactrl"
#endif

static volatile sig_atomic_t running = 1;
static char local_ip[64] = "127.0.0.1";
static char device_uuid[96] = "urn:uuid:8f4d8b90-8136-4000-8000-000000000001";
static const char *onvif_user;
static const char *onvif_pass;
static pthread_mutex_t ptz_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int x, y;
    int home_x, home_y;
    int moving;
    int dx, dy;
    pthread_t thread;
    int thread_active;
    struct { int used, x, y; char token[32], name[64]; } presets[PRESET_MAX];
} ptz_state_t;
static ptz_state_t ptz = { .x=15, .y=7, .home_x=15, .home_y=7, .moving=0, .dx=0, .dy=0, .thread_active=0 };

static void logmsg(const char *lvl, const char *fmt, ...) {
    va_list ap; time_t t=time(NULL); struct tm tmv; char ts[32];
    localtime_r(&t,&tmv); strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tmv);
    fprintf(stderr,"%s %-5s onvif: ",ts,lvl); va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); fputc('\n',stderr);
}
static void on_signal(int s) { (void)s; running=0; }
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }

static void xml_escape(const char *in,char *out,size_t n){
    size_t o=0; while(*in && o+6<n){ const char *r=NULL; switch(*in){case '&':r="&amp;";break;case '<':r="&lt;";break;case '>':r="&gt;";break;case '\"':r="&quot;";break;case '\'':r="&apos;";break;} if(r){size_t l=strlen(r);if(o+l>=n)break;memcpy(out+o,r,l);o+=l;}else out[o++]=*in;in++;} out[o]=0;
}
static int get_ip(const char *ifname,char *out,size_t n){
    int fd=socket(AF_INET,SOCK_DGRAM,0); struct ifreq ifr; if(fd<0)return -1;
    memset(&ifr,0,sizeof(ifr)); ifr.ifr_addr.sa_family=AF_INET; strncpy(ifr.ifr_name,ifname,IFNAMSIZ-1);
    if(ioctl(fd,SIOCGIFADDR,&ifr)<0){close(fd);return -1;} snprintf(out,n,"%s",inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr)); close(fd); return 0;
}
static void make_uuid(void){
    FILE *f=fopen("/sys/class/net/mlan0/address","r"); char mac[32]={0},hex[32]={0}; int j=0;
    if(!f)f=fopen("/sys/class/net/wlan0/address","r"); if(f){fgets(mac,sizeof(mac),f);fclose(f);for(int i=0;mac[i]&&j<12;i++)if(isxdigit((unsigned char)mac[i]))hex[j++]=tolower((unsigned char)mac[i]);hex[j]=0;if(j==12)snprintf(device_uuid,sizeof(device_uuid),"urn:uuid:8f4d8b90-8136-4000-8000-%s",hex);}
}
static int extract_tag(const char *xml,const char *tag,char *out,size_t n){
    char a[96],b[96]; const char *p,*q; snprintf(a,sizeof(a),"<%s",tag); p=strstr(xml,a);
    if(!p){const char *c=strchr(tag,':'); if(c){snprintf(a,sizeof(a),"<%s",c+1);p=strstr(xml,a);}} if(!p)return -1;
    p=strchr(p,'>'); if(!p)return -1; p++; snprintf(b,sizeof(b),"</%s>",tag); q=strstr(p,b);
    if(!q){const char *c=strchr(tag,':');if(c){snprintf(b,sizeof(b),"</%s>",c+1);q=strstr(p,b);}} if(!q)return -1;
    size_t l=(size_t)(q-p); if(l>=n)l=n-1; memcpy(out,p,l);out[l]=0; return 0;
}
static int extract_float_after(const char *xml,const char *needle,const char *attr,float *v){
    const char *p=strstr(xml,needle); char key[32]; if(!p)return -1; snprintf(key,sizeof(key),"%s=\"",attr);p=strstr(p,key);if(!p)return -1;p+=strlen(key);*v=(float)atof(p);return 0;
}

static void save_state(void){FILE*f=fopen(STATE_FILE,"w");if(!f)return;fprintf(f,"%d %d %d %d\n",ptz.x,ptz.y,ptz.home_x,ptz.home_y);for(int i=0;i<PRESET_MAX;i++)if(ptz.presets[i].used)fprintf(f,"P %s %d %d %s\n",ptz.presets[i].token,ptz.presets[i].x,ptz.presets[i].y,ptz.presets[i].name);fclose(f);}
static void load_state(void){FILE*f=fopen(STATE_FILE,"r");char line[256];if(!f)return;if(fgets(line,sizeof(line),f))sscanf(line,"%d %d %d %d",&ptz.x,&ptz.y,&ptz.home_x,&ptz.home_y);while(fgets(line,sizeof(line),f)){char tok[32],name[64];int x,y;if(sscanf(line,"P %31s %d %d %63s",tok,&x,&y,name)==4)for(int i=0;i<PRESET_MAX;i++)if(!ptz.presets[i].used){ptz.presets[i].used=1;ptz.presets[i].x=x;ptz.presets[i].y=y;snprintf(ptz.presets[i].token,sizeof(ptz.presets[i].token),"%s",tok);snprintf(ptz.presets[i].name,sizeof(ptz.presets[i].name),"%s",name);break;}}fclose(f);}

static int motor_move_delta(int dx,int dy){
    int lockfd,st; pid_t pid; char xs[16],ys[16]; dx=clampi(dx,-PTZ_X_MAX,PTZ_X_MAX);dy=clampi(dy,-PTZ_Y_MAX,PTZ_Y_MAX);if(!dx&&!dy)return 0;
    lockfd=open(MOTOR_LOCK,O_CREAT|O_RDWR,0600);if(lockfd<0)return -1;if(flock(lockfd,LOCK_EX)<0){close(lockfd);return -1;}
    snprintf(xs,sizeof(xs),"%+d",dx);snprintf(ys,sizeof(ys),"%+d",dy);pid=fork();
    if(pid==0){execl(MIJIACTRL_PATH,MIJIACTRL_PATH,"MOVE",xs,ys,(char*)NULL);_exit(127);} if(pid<0){flock(lockfd,LOCK_UN);close(lockfd);return -1;}
    if(waitpid(pid,&st,0)<0)st=-1;flock(lockfd,LOCK_UN);close(lockfd);return st!=-1&&WIFEXITED(st)&&WEXITSTATUS(st)==0?0:-1;
}
static int motor_goto(int tx,int ty){
    int dx,dy,rc; tx=clampi(tx,0,PTZ_X_MAX);ty=clampi(ty,0,PTZ_Y_MAX);pthread_mutex_lock(&ptz_mutex);dx=tx-ptz.x;dy=ty-ptz.y;pthread_mutex_unlock(&ptz_mutex);rc=motor_move_delta(dx,dy);if(!rc){pthread_mutex_lock(&ptz_mutex);ptz.x=tx;ptz.y=ty;save_state();pthread_mutex_unlock(&ptz_mutex);}return rc;
}
static int pan_to_x(float p){p=clampf(p,-1,1);return (int)(((p+1.0f)*PTZ_X_MAX/2.0f)+0.5f);} static int tilt_to_y(float p){p=clampf(p,-1,1);return (int)(((p+1.0f)*PTZ_Y_MAX/2.0f)+0.5f);} static float x_to_pan(int x){return ((float)x*2.0f/PTZ_X_MAX)-1.0f;} static float y_to_tilt(int y){return ((float)y*2.0f/PTZ_Y_MAX)-1.0f;}
static void *ptz_worker(void *arg){(void)arg;for(;;){int run,dx,dy;pthread_mutex_lock(&ptz_mutex);run=ptz.moving&&running;dx=ptz.dx;dy=ptz.dy;pthread_mutex_unlock(&ptz_mutex);if(!run)break;pthread_mutex_lock(&ptz_mutex);int tx=clampi(ptz.x+dx,0,PTZ_X_MAX),ty=clampi(ptz.y+dy,0,PTZ_Y_MAX);pthread_mutex_unlock(&ptz_mutex);if(motor_goto(tx,ty)<0)break;usleep(180000);}pthread_mutex_lock(&ptz_mutex);ptz.moving=0;ptz.thread_active=0;pthread_mutex_unlock(&ptz_mutex);return NULL;}
static void continuous_start(int dx,int dy){pthread_mutex_lock(&ptz_mutex);ptz.moving=0;pthread_mutex_unlock(&ptz_mutex);usleep(200000);pthread_mutex_lock(&ptz_mutex);ptz.dx=dx;ptz.dy=dy;ptz.moving=1;if(!ptz.thread_active){ptz.thread_active=1;pthread_create(&ptz.thread,NULL,ptz_worker,NULL);pthread_detach(ptz.thread);}pthread_mutex_unlock(&ptz_mutex);}
static void continuous_stop(void){pthread_mutex_lock(&ptz_mutex);ptz.moving=0;pthread_mutex_unlock(&ptz_mutex);}

static int proc_read_number(const char *path,float *v){FILE*f=fopen(path,"r");char s[256];if(!f)return -1;size_t n=fread(s,1,sizeof(s)-1,f);fclose(f);s[n]=0;char*p=s;while(*p && !isdigit((unsigned char)*p)&&*p!='-'&&*p!='.')p++;if(!*p)return -1;*v=(float)atof(p);return 0;}
static int proc_write_number(const char *path,float v){FILE*f=fopen(path,"w");if(!f)return -1;fprintf(f,"%.2f\n",v);int rc=ferror(f)?-1:0;fclose(f);return rc;}

static const char *soap_head=
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
"<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>";
static const char *soap_tail="</s:Body></s:Envelope>";
static void xmlcat(char *out,size_t n,const char *fmt,...){size_t l=strlen(out);if(l>=n-1)return;va_list ap;va_start(ap,fmt);vsnprintf(out+l,n-l,fmt,ap);va_end(ap);}
static void fault(char *out,size_t n,const char *reason){snprintf(out,n,"%s<s:Fault><s:Code><s:Value>s:Sender</s:Value></s:Code><s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>%s",soap_head,reason,soap_tail);}

static void handle_soap(const char *req,char *out,size_t n){
    out[0]=0; xmlcat(out,n,"%s",soap_head);
    if(strstr(req,"GetDeviceInformation")){
        xmlcat(out,n,"<tds:GetDeviceInformationResponse><tds:Manufacturer>Xiaomi/Chuangmi</tds:Manufacturer><tds:Model>Mijia 1080p GM8136</tds:Model><tds:FirmwareVersion>custom-onvif-1.0</tds:FirmwareVersion><tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>GM8136</tds:HardwareId></tds:GetDeviceInformationResponse>",device_uuid);
    } else if(strstr(req,"GetSystemDateAndTime")){
        time_t t=time(NULL);struct tm u,l;gmtime_r(&t,&u);localtime_r(&t,&l);
        xmlcat(out,n,"<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>GMT+07:00</tt:TZ></tt:TimeZone><tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",u.tm_hour,u.tm_min,u.tm_sec,u.tm_year+1900,u.tm_mon+1,u.tm_mday);
    } else if(strstr(req,"GetHostname")){
        char host[64]="chuangmi-v2";gethostname(host,sizeof(host)-1);xmlcat(out,n,"<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>%s</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>",host);
    } else if(strstr(req,"GetServices")){
        xmlcat(out,n,"<tds:GetServicesResponse>"
          "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service>"
          "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service>"
          "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/ptz_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service>"
          "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/imaging_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service>"
          "</tds:GetServicesResponse>",local_ip,ONVIF_HTTP_PORT,local_ip,ONVIF_HTTP_PORT,local_ip,ONVIF_HTTP_PORT,local_ip,ONVIF_HTTP_PORT);
    } else if(strstr(req,"GetCapabilities")){
        xmlcat(out,n,"<tds:GetCapabilitiesResponse><tds:Capabilities>"
          "<tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device>"
          "<tt:Media><tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media>"
          "<tt:PTZ><tt:XAddr>http://%s:%d/onvif/ptz_service</tt:XAddr></tt:PTZ>"
          "<tt:Imaging><tt:XAddr>http://%s:%d/onvif/imaging_service</tt:XAddr></tt:Imaging>"
          "</tds:Capabilities></tds:GetCapabilitiesResponse>",local_ip,ONVIF_HTTP_PORT,local_ip,ONVIF_HTTP_PORT,local_ip,ONVIF_HTTP_PORT,local_ip,ONVIF_HTTP_PORT);
    } else if(strstr(req,"GetProfiles")){
        xmlcat(out,n,"<trt:GetProfilesResponse><trt:Profiles token=\"profile_0\" fixed=\"true\"><tt:Name>MainStream</tt:Name><tt:VideoSourceConfiguration token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/></tt:VideoSourceConfiguration><tt:VideoEncoderConfiguration token=\"venc_0\"><tt:Name>H264</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>20</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>2048</tt:BitrateLimit></tt:RateControl><tt:H264><tt:GovLength>20</tt:GovLength><tt:H264Profile>High</tt:H264Profile></tt:H264></tt:VideoEncoderConfiguration><tt:PTZConfiguration token=\"ptzcfg_0\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>ptznode_0</tt:NodeToken></tt:PTZConfiguration></trt:Profiles></trt:GetProfilesResponse>");
    } else if(strstr(req,"GetVideoSources")){
        xmlcat(out,n,"<trt:GetVideoSourcesResponse><trt:VideoSources token=\"source_0\"><tt:Framerate>20</tt:Framerate><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>");
    } else if(strstr(req,"GetStreamUri")){
        xmlcat(out,n,"<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%d/live/ch00_0</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>",local_ip,RTSP_PORT);
    } else if(strstr(req,"GetSnapshotUri")){
        xmlcat(out,n,"<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>http://%s:%d/onvif/snapshot.jpg</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>",local_ip,ONVIF_HTTP_PORT);
    } else if(strstr(req,"GetNodes")){
        xmlcat(out,n,"<tptz:GetNodesResponse><tptz:PTZNode token=\"ptznode_0\" FixedHomePosition=\"false\"><tt:Name>PanTilt Motor</tt:Name><tt:SupportedPTZSpaces><tt:AbsolutePanTiltPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:AbsolutePanTiltPositionSpace><tt:RelativePanTiltTranslationSpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:RelativePanTiltTranslationSpace><tt:ContinuousPanTiltVelocitySpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:ContinuousPanTiltVelocitySpace></tt:SupportedPTZSpaces><tt:MaximumNumberOfPresets>%d</tt:MaximumNumberOfPresets><tt:HomeSupported>true</tt:HomeSupported></tptz:PTZNode></tptz:GetNodesResponse>",PRESET_MAX);
    } else if(strstr(req,"GetConfigurations")){
        xmlcat(out,n,"<tptz:GetConfigurationsResponse><tptz:PTZConfiguration token=\"ptzcfg_0\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>ptznode_0</tt:NodeToken><tt:DefaultAbsolutePantTiltPositionSpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:DefaultAbsolutePantTiltPositionSpace><tt:DefaultRelativePanTiltTranslationSpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace</tt:DefaultRelativePanTiltTranslationSpace><tt:DefaultContinuousPanTiltVelocitySpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:DefaultContinuousPanTiltVelocitySpace></tptz:PTZConfiguration></tptz:GetConfigurationsResponse>");
    } else if(strstr(req,"GetStatus")){
        pthread_mutex_lock(&ptz_mutex);float pan=x_to_pan(ptz.x),tilt=y_to_tilt(ptz.y);int mv=ptz.moving;pthread_mutex_unlock(&ptz_mutex);
        xmlcat(out,n,"<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\" space=\"http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace\"/></tt:Position><tt:MoveStatus><tt:PanTilt>%s</tt:PanTilt><tt:Zoom>IDLE</tt:Zoom></tt:MoveStatus><tt:UtcTime>2026-01-01T00:00:00Z</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>",pan,tilt,mv?"MOVING":"IDLE");
    } else if(strstr(req,"AbsoluteMove")){
        float x=0,y=0;if(extract_float_after(req,"PanTilt","x",&x)<0||extract_float_after(req,"PanTilt","y",&y)<0){fault(out,n,"Invalid PanTilt position");return;}continuous_stop();if(motor_goto(pan_to_x(x),tilt_to_y(y))<0){fault(out,n,"Motor move failed");return;}xmlcat(out,n,"<tptz:AbsoluteMoveResponse/>");
    } else if(strstr(req,"RelativeMove")){
        float x=0,y=0;if(extract_float_after(req,"PanTilt","x",&x)<0||extract_float_after(req,"PanTilt","y",&y)<0){fault(out,n,"Invalid PanTilt translation");return;}pthread_mutex_lock(&ptz_mutex);int tx=clampi(ptz.x+(int)(x*PTZ_X_MAX),0,PTZ_X_MAX),ty=clampi(ptz.y+(int)(y*PTZ_Y_MAX),0,PTZ_Y_MAX);pthread_mutex_unlock(&ptz_mutex);continuous_stop();if(motor_goto(tx,ty)<0){fault(out,n,"Motor move failed");return;}xmlcat(out,n,"<tptz:RelativeMoveResponse/>");
    } else if(strstr(req,"ContinuousMove")){
        float x=0,y=0;extract_float_after(req,"PanTilt","x",&x);extract_float_after(req,"PanTilt","y",&y);continuous_start(x>0.05f?1:(x<-0.05f?-1:0),y>0.05f?1:(y<-0.05f?-1:0));xmlcat(out,n,"<tptz:ContinuousMoveResponse/>");
    } else if(strstr(req,"<tptz:Stop")||strstr(req,"<Stop")){
        continuous_stop();xmlcat(out,n,"<tptz:StopResponse/>");
    } else if(strstr(req,"SetHomePosition")){
        pthread_mutex_lock(&ptz_mutex);ptz.home_x=ptz.x;ptz.home_y=ptz.y;save_state();pthread_mutex_unlock(&ptz_mutex);xmlcat(out,n,"<tptz:SetHomePositionResponse/>");
    } else if(strstr(req,"GotoHomePosition")){
        pthread_mutex_lock(&ptz_mutex);int x=ptz.home_x,y=ptz.home_y;pthread_mutex_unlock(&ptz_mutex);if(motor_goto(x,y)<0){fault(out,n,"Motor move failed");return;}xmlcat(out,n,"<tptz:GotoHomePositionResponse/>");
    } else if(strstr(req,"GetPresets")){
        xmlcat(out,n,"<tptz:GetPresetsResponse>");pthread_mutex_lock(&ptz_mutex);for(int i=0;i<PRESET_MAX;i++)if(ptz.presets[i].used)xmlcat(out,n,"<tptz:Preset token=\"%s\"><tt:Name>%s</tt:Name><tt:PTZPosition><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/></tt:PTZPosition></tptz:Preset>",ptz.presets[i].token,ptz.presets[i].name,x_to_pan(ptz.presets[i].x),y_to_tilt(ptz.presets[i].y));pthread_mutex_unlock(&ptz_mutex);xmlcat(out,n,"</tptz:GetPresetsResponse>");
    } else if(strstr(req,"SetPreset")){
        char name[64]="Preset",token[32]="";extract_tag(req,"tptz:PresetName",name,sizeof(name));extract_tag(req,"tptz:PresetToken",token,sizeof(token));pthread_mutex_lock(&ptz_mutex);int idx=-1;for(int i=0;i<PRESET_MAX;i++)if((token[0]&&ptz.presets[i].used&&!strcmp(token,ptz.presets[i].token))||(!token[0]&&!ptz.presets[i].used)){idx=i;break;}if(idx>=0){ptz.presets[idx].used=1;ptz.presets[idx].x=ptz.x;ptz.presets[idx].y=ptz.y;if(!token[0])snprintf(token,sizeof(token),"preset_%d",idx);snprintf(ptz.presets[idx].token,sizeof(ptz.presets[idx].token),"%s",token);snprintf(ptz.presets[idx].name,sizeof(ptz.presets[idx].name),"%s",name);save_state();}pthread_mutex_unlock(&ptz_mutex);if(idx<0){fault(out,n,"Preset storage full");return;}xmlcat(out,n,"<tptz:SetPresetResponse><tptz:PresetToken>%s</tptz:PresetToken></tptz:SetPresetResponse>",token);
    } else if(strstr(req,"GotoPreset")){
        char token[32];if(extract_tag(req,"tptz:PresetToken",token,sizeof(token))<0){fault(out,n,"Missing preset token");return;}int x=-1,y=-1;pthread_mutex_lock(&ptz_mutex);for(int i=0;i<PRESET_MAX;i++)if(ptz.presets[i].used&&!strcmp(token,ptz.presets[i].token)){x=ptz.presets[i].x;y=ptz.presets[i].y;break;}pthread_mutex_unlock(&ptz_mutex);if(x<0||motor_goto(x,y)<0){fault(out,n,"Preset not found or motor failed");return;}xmlcat(out,n,"<tptz:GotoPresetResponse/>");
    } else if(strstr(req,"RemovePreset")){
        char token[32];if(extract_tag(req,"tptz:PresetToken",token,sizeof(token))<0){fault(out,n,"Missing preset token");return;}pthread_mutex_lock(&ptz_mutex);for(int i=0;i<PRESET_MAX;i++)if(ptz.presets[i].used&&!strcmp(token,ptz.presets[i].token))ptz.presets[i].used=0;save_state();pthread_mutex_unlock(&ptz_mutex);xmlcat(out,n,"<tptz:RemovePresetResponse/>");
    } else if(strstr(req,"GetImagingSettings")){
        float sharp=50;proc_read_number("/proc/vcap300/vcap0/sharpness/param",&sharp);xmlcat(out,n,"<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>50</tt:Brightness><tt:ColorSaturation>50</tt:ColorSaturation><tt:Contrast>50</tt:Contrast><tt:Sharpness>%.1f</tt:Sharpness><tt:Exposure><tt:Mode>AUTO</tt:Mode><tt:Priority>LowNoise</tt:Priority></tt:Exposure><tt:WhiteBalance><tt:Mode>AUTO</tt:Mode></tt:WhiteBalance><tt:Focus><tt:AutoFocusMode>MANUAL</tt:AutoFocusMode><tt:DefaultSpeed>0</tt:DefaultSpeed></tt:Focus></timg:ImagingSettings></timg:GetImagingSettingsResponse>",sharp);
    } else if(strstr(req,"SetImagingSettings")){
        char val[64];if(extract_tag(req,"tt:Sharpness",val,sizeof(val))==0)proc_write_number("/proc/vcap300/vcap0/sharpness/param",(float)atof(val));xmlcat(out,n,"<timg:SetImagingSettingsResponse/>");
    } else if(strstr(req,"GetOptions")&&strstr(req,"imaging")){
        xmlcat(out,n,"<timg:GetOptionsResponse><timg:ImagingOptions><tt:Brightness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Brightness><tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:ColorSaturation><tt:Contrast><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Contrast><tt:Sharpness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Sharpness></timg:ImagingOptions></timg:GetOptionsResponse>");
    } else if(strstr(req,"timg:Move")||strstr(req,"timg:Stop")||strstr(req,"GetMoveOptions")){
        fault(out,n,"Focus actuator is not available on this device");return;
    } else {
        fault(out,n,"Action not supported");return;
    }
    xmlcat(out,n,"%s",soap_tail);
}

static void http_send(int fd,int code,const char *type,const char *body){char h[512];size_t l=body?strlen(body):0;int n=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nServer: GM8136-ONVIF/1.0\r\n\r\n",code,code==200?"OK":"Error",type,(unsigned long)l);send(fd,h,n,0);if(l)send(fd,body,l,0);}
static void *client_thread(void *arg){int fd=(int)(intptr_t)arg;char *req=calloc(1,MAX_REQ),*xml=calloc(1,MAX_XML);if(!req||!xml){close(fd);free(req);free(xml);return NULL;}int used=0,r;while(used<MAX_REQ-1&&(r=recv(fd,req+used,MAX_REQ-1-used,0))>0){used+=r;req[used]=0;char*h=strstr(req,"\r\n\r\n");if(h){int cl=0;char*c=strcasestr(req,"Content-Length:");if(c)cl=atoi(c+15);if(used>=(h+4-req)+cl)break;}}if(strstr(req,"GET /onvif/snapshot.jpg")){http_send(fd,404,"text/plain","Snapshot is provided by rtspd integration and is not configured\n");}else if(strstr(req,"POST /onvif/")){handle_soap(strstr(req,"\r\n\r\n")?strstr(req,"\r\n\r\n")+4:req,xml,MAX_XML);http_send(fd,200,"application/soap+xml; charset=utf-8",xml);}else http_send(fd,404,"text/plain","Not found\n");free(req);free(xml);close(fd);return NULL;}
static void *http_server(void *arg){(void)arg;int s=socket(AF_INET,SOCK_STREAM,0),one=1;struct sockaddr_in a;if(s<0)return NULL;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(ONVIF_HTTP_PORT);a.sin_addr.s_addr=INADDR_ANY;if(bind(s,(struct sockaddr*)&a,sizeof(a))<0||listen(s,16)<0){logmsg("ERROR","HTTP bind/listen: %s",strerror(errno));close(s);return NULL;}logmsg("INFO","SOAP HTTP listening on %d",ONVIF_HTTP_PORT);while(running){int c=accept(s,NULL,NULL);if(c<0){if(errno==EINTR)continue;break;}pthread_t t;if(!pthread_create(&t,NULL,client_thread,(void*)(intptr_t)c))pthread_detach(t);else close(c);}close(s);return NULL;}

static void *discovery_server(void *arg){(void)arg;int s=socket(AF_INET,SOCK_DGRAM,0),one=1;struct sockaddr_in a;struct ip_mreq m;char buf[8192];if(s<0)return NULL;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(ONVIF_DISCOVERY_PORT);a.sin_addr.s_addr=INADDR_ANY;if(bind(s,(struct sockaddr*)&a,sizeof(a))<0){logmsg("ERROR","Discovery bind: %s",strerror(errno));close(s);return NULL;}m.imr_multiaddr.s_addr=inet_addr(ONVIF_DISCOVERY_GROUP);m.imr_interface.s_addr=INADDR_ANY;setsockopt(s,IPPROTO_IP,IP_ADD_MEMBERSHIP,&m,sizeof(m));logmsg("INFO","WS-Discovery listening on %s:%d",ONVIF_DISCOVERY_GROUP,ONVIF_DISCOVERY_PORT);while(running){struct sockaddr_in from;socklen_t fl=sizeof(from);int n=recvfrom(s,buf,sizeof(buf)-1,0,(struct sockaddr*)&from,&fl);if(n<=0)continue;buf[n]=0;if(!strstr(buf,"Probe"))continue;char msgid[256]="urn:uuid:probe";extract_tag(buf,"a:MessageID",msgid,sizeof(msgid));char rsp[4096];int l=snprintf(rsp,sizeof(rsp),"<?xml version=\"1.0\" encoding=\"UTF-8\"?><e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><e:Header><a:MessageID>urn:uuid:%ld-%d</a:MessageID><a:RelatesTo>%s</a:RelatesTo><a:To>http://www.w3.org/2005/08/addressing/anonymous</a:To><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action></e:Header><e:Body><d:ProbeMatches><d:ProbeMatch><a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference><d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/ptz onvif://www.onvif.org/name/chuangmi-v2</d:Scopes><d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>",(long)time(NULL),getpid(),msgid,device_uuid,local_ip,ONVIF_HTTP_PORT);sendto(s,rsp,l,0,(struct sockaddr*)&from,fl);}close(s);return NULL;}

int main(void){pthread_t h,d;signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);onvif_user=getenv("ONVIF_USER");onvif_pass=getenv("ONVIF_PASS");if(get_ip("mlan0",local_ip,sizeof(local_ip))<0)get_ip("wlan0",local_ip,sizeof(local_ip));make_uuid();load_state();logmsg("INFO","device=%s ip=%s user=%s",device_uuid,local_ip,onvif_user?onvif_user:"(none)");if(access(MIJIACTRL_PATH,X_OK)<0)logmsg("WARN","%s not executable; PTZ calls will fail",MIJIACTRL_PATH);pthread_create(&h,NULL,http_server,NULL);pthread_create(&d,NULL,discovery_server,NULL);while(running)sleep(1);continuous_stop();pthread_cancel(h);pthread_cancel(d);pthread_join(h,NULL);pthread_join(d,NULL);return 0;}
