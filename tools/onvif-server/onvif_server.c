/*
 * onvif_server.c - compact ONVIF server for Chuangmi/GM8136
 * Direct motor access: /dev/motor (no mijiactrl dependency)
 *
 * Build:
 *   arm-linux-gcc -std=gnu99 -Os -Wall -Wextra -pthread onvif_server.c -o onvif_server
 * Run:
 *   ./onvif_server
 *
 * Services:
 *   WS-Discovery UDP 239.255.255.250:3702
 *   Device/Media/PTZ/Imaging SOAP HTTP :8899
 *   RTSP URI: rtsp://<camera>:554/live/ch00_0
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
#include <unistd.h>

#define HTTP_PORT 8899
#define RTSP_PORT 554
#define WSD_PORT 3702
#define WSD_GROUP "239.255.255.250"
#define MOTOR_DEV "/dev/motor"
#define MAX_REQ 65536
#define MAX_XML 49152
#define X_MAX 31
#define Y_MAX 15
#define PRESET_MAX 16
#define STATE_FILE "/tmp/onvif_ptz.state"

/* Exact ioctl ABI recovered from the vendor motor.ko. */
#define MOTOR_MAGIC 'M'
#define H_DIR_SET   _IOW(MOTOR_MAGIC,  3, int)
#define H_DIST_SET  _IOW(MOTOR_MAGIC,  4, int)
#define H_COORD_GET _IOW(MOTOR_MAGIC,  5, int)
#define H_COORD_SET _IOW(MOTOR_MAGIC,  6, int)
#define V_DIR_SET   _IOW(MOTOR_MAGIC, 23, int)
#define V_DIST_SET  _IOW(MOTOR_MAGIC, 24, int)
#define V_COORD_GET _IOW(MOTOR_MAGIC, 25, int)
#define V_COORD_SET _IOW(MOTOR_MAGIC, 26, int)

static volatile sig_atomic_t alive=1;
static char ipaddr[64]="127.0.0.1";
static char uuid[96]="urn:uuid:81360000-0000-4000-8000-000000000001";
static int motor_fd=-1;
static pthread_mutex_t motor_lock=PTHREAD_MUTEX_INITIALIZER;

typedef struct { int valid,x,y; char token[32],name[64]; } preset_t;
typedef struct {
 int x,y,home_x,home_y,moving,dx,dy,worker_active;
 pthread_t worker;
 preset_t preset[PRESET_MAX];
} ptz_t;
static ptz_t ptz={.x=15,.y=7,.home_x=15,.home_y=7};

static int clampi(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static float clampf(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
static float x2pan(int x){return ((float)x*2.0f/X_MAX)-1.0f;}
static float y2tilt(int y){return ((float)y*2.0f/Y_MAX)-1.0f;}
static int pan2x(float p){p=clampf(p,-1,1);return (int)((p+1)*X_MAX/2+0.5f);}
static int tilt2y(float p){p=clampf(p,-1,1);return (int)((p+1)*Y_MAX/2+0.5f);}
static void sigfn(int s){(void)s;alive=0;}
static void logx(const char*l,const char*f,...){va_list a;fprintf(stderr,"%s onvif: ",l);va_start(a,f);vfprintf(stderr,f,a);va_end(a);fputc('\n',stderr);}

static int getip(const char*n,char*out,size_t z){int s=socket(AF_INET,SOCK_DGRAM,0);struct ifreq q;if(s<0)return-1;memset(&q,0,sizeof(q));q.ifr_addr.sa_family=AF_INET;strncpy(q.ifr_name,n,IFNAMSIZ-1);if(ioctl(s,SIOCGIFADDR,&q)<0){close(s);return-1;}snprintf(out,z,"%s",inet_ntoa(((struct sockaddr_in*)&q.ifr_addr)->sin_addr));close(s);return 0;}
static void make_uuid(void){FILE*f=fopen("/sys/class/net/mlan0/address","r");char m[32]={0},h[20]={0};int j=0;if(!f)f=fopen("/sys/class/net/wlan0/address","r");if(!f)return;fgets(m,sizeof(m),f);fclose(f);for(int i=0;m[i]&&j<12;i++)if(isxdigit((unsigned char)m[i]))h[j++]=(char)tolower((unsigned char)m[i]);if(j==12)snprintf(uuid,sizeof(uuid),"urn:uuid:81360000-0000-4000-8000-%s",h);}
static void save_state(void){FILE*f=fopen(STATE_FILE,"w");if(!f)return;fprintf(f,"%d %d %d %d\n",ptz.x,ptz.y,ptz.home_x,ptz.home_y);for(int i=0;i<PRESET_MAX;i++)if(ptz.preset[i].valid)fprintf(f,"P %s %d %d %s\n",ptz.preset[i].token,ptz.preset[i].x,ptz.preset[i].y,ptz.preset[i].name);fclose(f);}
static void load_state(void){FILE*f=fopen(STATE_FILE,"r");char b[256];if(!f)return;if(fgets(b,sizeof(b),f))sscanf(b,"%d %d %d %d",&ptz.x,&ptz.y,&ptz.home_x,&ptz.home_y);while(fgets(b,sizeof(b),f)){char t[32],n[64];int x,y;if(sscanf(b,"P %31s %d %d %63s",t,&x,&y,n)==4)for(int i=0;i<PRESET_MAX;i++)if(!ptz.preset[i].valid){ptz.preset[i].valid=1;ptz.preset[i].x=x;ptz.preset[i].y=y;strncpy(ptz.preset[i].token,t,31);strncpy(ptz.preset[i].name,n,63);break;}}fclose(f);}

static int mioc(unsigned long cmd,int*v){int r;if(motor_fd<0){errno=ENODEV;return-1;}do r=ioctl(motor_fd,cmd,v);while(r<0&&errno==EINTR);return r;}
static int motor_open(void){motor_fd=open(MOTOR_DEV,O_RDWR|O_CLOEXEC);return motor_fd<0?-1:0;}
static int motor_pos(int*x,int*y){int a=0,b=0;if(mioc(H_COORD_GET,&a)<0||mioc(V_COORD_GET,&b)<0)return-1;*x=clampi(a,0,X_MAX);*y=clampi(b,0,Y_MAX);return 0;}
static int axis(unsigned long dc,unsigned long sc,int d){int dir,n;if(!d)return 0;dir=d>0?1:0;n=d>0?d:-d;if(mioc(dc,&dir)<0)return-1;return mioc(sc,&n);}
static int motor_goto_locked(int tx,int ty){int x,y;tx=clampi(tx,0,X_MAX);ty=clampi(ty,0,Y_MAX);if(motor_pos(&x,&y)<0){x=ptz.x;y=ptz.y;}if(axis(H_DIR_SET,H_DIST_SET,tx-x)<0)return-1;if(axis(V_DIR_SET,V_DIST_SET,ty-y)<0)return-1;if(motor_pos(&ptz.x,&ptz.y)<0){ptz.x=tx;ptz.y=ty;}save_state();return 0;}
static int motor_goto(int x,int y){int r;pthread_mutex_lock(&motor_lock);r=motor_goto_locked(x,y);pthread_mutex_unlock(&motor_lock);return r;}
static void motor_refresh(void){pthread_mutex_lock(&motor_lock);if(motor_pos(&ptz.x,&ptz.y)==0)save_state();pthread_mutex_unlock(&motor_lock);}
static void stop_move(void){pthread_mutex_lock(&motor_lock);ptz.moving=0;pthread_mutex_unlock(&motor_lock);}
static void *move_worker(void*a){(void)a;for(;;){int run,dx,dy,tx,ty;pthread_mutex_lock(&motor_lock);run=alive&&ptz.moving;dx=ptz.dx;dy=ptz.dy;tx=clampi(ptz.x+dx,0,X_MAX);ty=clampi(ptz.y+dy,0,Y_MAX);pthread_mutex_unlock(&motor_lock);if(!run)break;if(motor_goto(tx,ty)<0)break;usleep(100000);}pthread_mutex_lock(&motor_lock);ptz.moving=0;ptz.worker_active=0;pthread_mutex_unlock(&motor_lock);return NULL;}
static void continual(int dx,int dy){stop_move();usleep(200000);pthread_mutex_lock(&motor_lock);ptz.dx=dx;ptz.dy=dy;ptz.moving=(dx||dy);if(ptz.moving&&!ptz.worker_active){ptz.worker_active=1;pthread_create(&ptz.worker,NULL,move_worker,NULL);pthread_detach(ptz.worker);}pthread_mutex_unlock(&motor_lock);}

static int tag(const char*x,const char*n,char*out,size_t z){char a[96],b[96];const char*p,*q;snprintf(a,sizeof(a),"<%s",n);p=strstr(x,a);if(!p){const char*c=strchr(n,':');if(c){snprintf(a,sizeof(a),"<%s",c+1);p=strstr(x,a);}}if(!p)return-1;p=strchr(p,'>');if(!p)return-1;p++;snprintf(b,sizeof(b),"</%s>",n);q=strstr(p,b);if(!q){const char*c=strchr(n,':');if(c){snprintf(b,sizeof(b),"</%s>",c+1);q=strstr(p,b);}}if(!q)return-1;size_t l=(size_t)(q-p);if(l>=z)l=z-1;memcpy(out,p,l);out[l]=0;return 0;}
static int attrf(const char*x,const char*n,const char*a,float*v){const char*p=strstr(x,n);char k[32];if(!p)return-1;snprintf(k,sizeof(k),"%s=\"",a);p=strstr(p,k);if(!p)return-1;*v=(float)atof(p+strlen(k));return 0;}
static void add(char*o,size_t z,const char*f,...){size_t l=strlen(o);va_list a;if(l>=z-1)return;va_start(a,f);vsnprintf(o+l,z-l,f,a);va_end(a);}
static const char*H="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>";
static const char*T="</s:Body></s:Envelope>";
static void fault(char*o,size_t z,const char*m){snprintf(o,z,"%s<s:Fault><s:Code><s:Value>s:Sender</s:Value></s:Code><s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>%s",H,m,T);}

static void soap(const char*r,char*o,size_t z){o[0]=0;add(o,z,"%s",H);
 if(strstr(r,"GetDeviceInformation"))add(o,z,"<tds:GetDeviceInformationResponse><tds:Manufacturer>Xiaomi/Chuangmi</tds:Manufacturer><tds:Model>Mijia 1080p GM8136</tds:Model><tds:FirmwareVersion>ONVIF-direct-motor-1.0</tds:FirmwareVersion><tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>GM8136</tds:HardwareId></tds:GetDeviceInformationResponse>",uuid);
 else if(strstr(r,"GetCapabilities"))add(o,z,"<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device><tt:Media><tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media><tt:PTZ><tt:XAddr>http://%s:%d/onvif/ptz_service</tt:XAddr></tt:PTZ><tt:Imaging><tt:XAddr>http://%s:%d/onvif/imaging_service</tt:XAddr></tt:Imaging></tds:Capabilities></tds:GetCapabilitiesResponse>",ipaddr,HTTP_PORT,ipaddr,HTTP_PORT,ipaddr,HTTP_PORT,ipaddr,HTTP_PORT);
 else if(strstr(r,"GetServices"))add(o,z,"<tds:GetServicesResponse><tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/ptz_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>http://%s:%d/onvif/imaging_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service></tds:GetServicesResponse>",ipaddr,HTTP_PORT,ipaddr,HTTP_PORT,ipaddr,HTTP_PORT,ipaddr,HTTP_PORT);
 else if(strstr(r,"GetSystemDateAndTime")){time_t q=time(NULL);struct tm u;gmtime_r(&q,&u);add(o,z,"<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings><tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",u.tm_hour,u.tm_min,u.tm_sec,u.tm_year+1900,u.tm_mon+1,u.tm_mday);}
 else if(strstr(r,"GetProfiles"))add(o,z,"<trt:GetProfilesResponse><trt:Profiles token=\"profile_0\" fixed=\"true\"><tt:Name>MainStream</tt:Name><tt:VideoSourceConfiguration token=\"vsrc_0\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>source_0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/></tt:VideoSourceConfiguration><tt:VideoEncoderConfiguration token=\"venc_0\"><tt:Name>H264</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>20</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>2048</tt:BitrateLimit></tt:RateControl><tt:H264><tt:GovLength>20</tt:GovLength><tt:H264Profile>High</tt:H264Profile></tt:H264></tt:VideoEncoderConfiguration><tt:PTZConfiguration token=\"ptz_0\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>node_0</tt:NodeToken></tt:PTZConfiguration></trt:Profiles></trt:GetProfilesResponse>");
 else if(strstr(r,"GetVideoSources"))add(o,z,"<trt:GetVideoSourcesResponse><trt:VideoSources token=\"source_0\"><tt:Framerate>20</tt:Framerate><tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>");
 else if(strstr(r,"GetStreamUri"))add(o,z,"<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%d/live/ch00_0</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>",ipaddr,RTSP_PORT);
 else if(strstr(r,"GetNodes"))add(o,z,"<tptz:GetNodesResponse><tptz:PTZNode token=\"node_0\"><tt:Name>Chuangmi PanTilt</tt:Name><tt:SupportedPTZSpaces><tt:AbsolutePanTiltPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:AbsolutePanTiltPositionSpace><tt:RelativePanTiltTranslationSpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:RelativePanTiltTranslationSpace><tt:ContinuousPanTiltVelocitySpace><tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:ContinuousPanTiltVelocitySpace></tt:SupportedPTZSpaces><tt:MaximumNumberOfPresets>%d</tt:MaximumNumberOfPresets><tt:HomeSupported>true</tt:HomeSupported></tptz:PTZNode></tptz:GetNodesResponse>",PRESET_MAX);
 else if(strstr(r,"GetConfigurations"))add(o,z,"<tptz:GetConfigurationsResponse><tptz:PTZConfiguration token=\"ptz_0\"><tt:Name>PTZ</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>node_0</tt:NodeToken></tptz:PTZConfiguration></tptz:GetConfigurationsResponse>");
 else if(strstr(r,"GetStatus")){motor_refresh();pthread_mutex_lock(&motor_lock);add(o,z,"<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/></tt:Position><tt:MoveStatus><tt:PanTilt>%s</tt:PanTilt><tt:Zoom>IDLE</tt:Zoom></tt:MoveStatus><tt:UtcTime>2026-01-01T00:00:00Z</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>",x2pan(ptz.x),y2tilt(ptz.y),ptz.moving?"MOVING":"IDLE");pthread_mutex_unlock(&motor_lock);}
 else if(strstr(r,"AbsoluteMove")){float x,y;if(attrf(r,"PanTilt","x",&x)<0||attrf(r,"PanTilt","y",&y)<0){fault(o,z,"Invalid PanTilt");return;}stop_move();if(motor_goto(pan2x(x),tilt2y(y))<0){fault(o,z,"Motor failure");return;}add(o,z,"<tptz:AbsoluteMoveResponse/>");}
 else if(strstr(r,"RelativeMove")){float x,y;if(attrf(r,"PanTilt","x",&x)<0||attrf(r,"PanTilt","y",&y)<0){fault(o,z,"Invalid translation");return;}motor_refresh();pthread_mutex_lock(&motor_lock);int tx=clampi(ptz.x+(int)(x*X_MAX),0,X_MAX),ty=clampi(ptz.y+(int)(y*Y_MAX),0,Y_MAX);pthread_mutex_unlock(&motor_lock);stop_move();if(motor_goto(tx,ty)<0){fault(o,z,"Motor failure");return;}add(o,z,"<tptz:RelativeMoveResponse/>");}
 else if(strstr(r,"ContinuousMove")){float x=0,y=0;attrf(r,"PanTilt","x",&x);attrf(r,"PanTilt","y",&y);continual(x>0.05?1:(x<-0.05?-1:0),y>0.05?1:(y<-0.05?-1:0));add(o,z,"<tptz:ContinuousMoveResponse/>");}
 else if(strstr(r,"<tptz:Stop")||strstr(r,"<Stop")){stop_move();add(o,z,"<tptz:StopResponse/>");}
 else if(strstr(r,"SetHomePosition")){motor_refresh();pthread_mutex_lock(&motor_lock);ptz.home_x=ptz.x;ptz.home_y=ptz.y;save_state();pthread_mutex_unlock(&motor_lock);add(o,z,"<tptz:SetHomePositionResponse/>");}
 else if(strstr(r,"GotoHomePosition")){pthread_mutex_lock(&motor_lock);int x=ptz.home_x,y=ptz.home_y;pthread_mutex_unlock(&motor_lock);if(motor_goto(x,y)<0){fault(o,z,"Motor failure");return;}add(o,z,"<tptz:GotoHomePositionResponse/>");}
 else if(strstr(r,"GetPresets")){add(o,z,"<tptz:GetPresetsResponse>");pthread_mutex_lock(&motor_lock);for(int i=0;i<PRESET_MAX;i++)if(ptz.preset[i].valid)add(o,z,"<tptz:Preset token=\"%s\"><tt:Name>%s</tt:Name><tt:PTZPosition><tt:PanTilt x=\"%.4f\" y=\"%.4f\"/></tt:PTZPosition></tptz:Preset>",ptz.preset[i].token,ptz.preset[i].name,x2pan(ptz.preset[i].x),y2tilt(ptz.preset[i].y));pthread_mutex_unlock(&motor_lock);add(o,z,"</tptz:GetPresetsResponse>");}
 else if(strstr(r,"SetPreset")){char n[64]="Preset",t[32]="";tag(r,"tptz:PresetName",n,sizeof(n));tag(r,"tptz:PresetToken",t,sizeof(t));motor_refresh();pthread_mutex_lock(&motor_lock);int k=-1;for(int i=0;i<PRESET_MAX;i++)if((t[0]&&ptz.preset[i].valid&&!strcmp(t,ptz.preset[i].token))||(!t[0]&&!ptz.preset[i].valid)){k=i;break;}if(k>=0){if(!t[0])snprintf(t,sizeof(t),"preset_%d",k);ptz.preset[k].valid=1;ptz.preset[k].x=ptz.x;ptz.preset[k].y=ptz.y;snprintf(ptz.preset[k].token,32,"%s",t);snprintf(ptz.preset[k].name,64,"%s",n);save_state();}pthread_mutex_unlock(&motor_lock);if(k<0){fault(o,z,"Preset full");return;}add(o,z,"<tptz:SetPresetResponse><tptz:PresetToken>%s</tptz:PresetToken></tptz:SetPresetResponse>",t);}
 else if(strstr(r,"GotoPreset")){char t[32];if(tag(r,"tptz:PresetToken",t,sizeof(t))<0){fault(o,z,"Missing preset");return;}int x=-1,y=-1;pthread_mutex_lock(&motor_lock);for(int i=0;i<PRESET_MAX;i++)if(ptz.preset[i].valid&&!strcmp(t,ptz.preset[i].token)){x=ptz.preset[i].x;y=ptz.preset[i].y;break;}pthread_mutex_unlock(&motor_lock);if(x<0||motor_goto(x,y)<0){fault(o,z,"Preset unavailable");return;}add(o,z,"<tptz:GotoPresetResponse/>");}
 else if(strstr(r,"RemovePreset")){char t[32];if(tag(r,"tptz:PresetToken",t,sizeof(t))<0){fault(o,z,"Missing preset");return;}pthread_mutex_lock(&motor_lock);for(int i=0;i<PRESET_MAX;i++)if(ptz.preset[i].valid&&!strcmp(t,ptz.preset[i].token))ptz.preset[i].valid=0;save_state();pthread_mutex_unlock(&motor_lock);add(o,z,"<tptz:RemovePresetResponse/>");}
 else if(strstr(r,"GetImagingSettings"))add(o,z,"<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>50</tt:Brightness><tt:ColorSaturation>50</tt:ColorSaturation><tt:Contrast>50</tt:Contrast><tt:Sharpness>50</tt:Sharpness><tt:Exposure><tt:Mode>AUTO</tt:Mode></tt:Exposure><tt:WhiteBalance><tt:Mode>AUTO</tt:Mode></tt:WhiteBalance><tt:Focus><tt:AutoFocusMode>MANUAL</tt:AutoFocusMode></tt:Focus></timg:ImagingSettings></timg:GetImagingSettingsResponse>");
 else if(strstr(r,"GetOptions"))add(o,z,"<timg:GetOptionsResponse><timg:ImagingOptions><tt:Brightness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Brightness><tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:ColorSaturation><tt:Contrast><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Contrast><tt:Sharpness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Sharpness></timg:ImagingOptions></timg:GetOptionsResponse>");
 else if(strstr(r,"SetImagingSettings"))add(o,z,"<timg:SetImagingSettingsResponse/>");
 else {fault(o,z,"Action not supported");return;} add(o,z,"%s",T);
}

static void reply(int f,int c,const char*t,const char*b){char h[512];size_t n=b?strlen(b):0;int l=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",c,c==200?"OK":"Error",t,(unsigned long)n);send(f,h,l,0);if(n)send(f,b,n,0);}
static void *client(void*a){int f=(int)(intptr_t)a,u=0,n;char*r=calloc(1,MAX_REQ),*o=calloc(1,MAX_XML);if(!r||!o){close(f);free(r);free(o);return NULL;}while(u<MAX_REQ-1&&(n=recv(f,r+u,MAX_REQ-1-u,0))>0){u+=n;r[u]=0;char*e=strstr(r,"\r\n\r\n");if(e){char*c=strcasestr(r,"Content-Length:");int z=c?atoi(c+15):0;if(u>=(int)(e+4-r)+z)break;}}if(strstr(r,"POST /onvif/")){char*b=strstr(r,"\r\n\r\n");soap(b?b+4:r,o,MAX_XML);reply(f,200,"application/soap+xml; charset=utf-8",o);}else reply(f,404,"text/plain","Not found\n");free(r);free(o);close(f);return NULL;}
static void *httpd(void*a){(void)a;int s=socket(AF_INET,SOCK_STREAM,0),one=1;struct sockaddr_in q;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&q,0,sizeof(q));q.sin_family=AF_INET;q.sin_port=htons(HTTP_PORT);q.sin_addr.s_addr=INADDR_ANY;if(bind(s,(void*)&q,sizeof(q))<0||listen(s,16)<0){logx("ERROR","HTTP: %s",strerror(errno));return NULL;}while(alive){int f=accept(s,NULL,NULL);if(f<0)continue;pthread_t t;if(!pthread_create(&t,NULL,client,(void*)(intptr_t)f))pthread_detach(t);else close(f);}close(s);return NULL;}
static void *wsdd(void*a){(void)a;int s=socket(AF_INET,SOCK_DGRAM,0),one=1;struct sockaddr_in q;struct ip_mreq m;char b[8192];setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&q,0,sizeof(q));q.sin_family=AF_INET;q.sin_port=htons(WSD_PORT);q.sin_addr.s_addr=INADDR_ANY;if(bind(s,(void*)&q,sizeof(q))<0)return NULL;m.imr_multiaddr.s_addr=inet_addr(WSD_GROUP);m.imr_interface.s_addr=INADDR_ANY;setsockopt(s,IPPROTO_IP,IP_ADD_MEMBERSHIP,&m,sizeof(m));while(alive){struct sockaddr_in f;socklen_t z=sizeof(f);int n=recvfrom(s,b,sizeof(b)-1,0,(void*)&f,&z);if(n<=0)continue;b[n]=0;if(!strstr(b,"Probe"))continue;char id[256]="urn:uuid:unknown";tag(b,"a:MessageID",id,sizeof(id));char x[4096];int l=snprintf(x,sizeof(x),"<?xml version=\"1.0\"?><e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><e:Header><a:MessageID>urn:uuid:%ld</a:MessageID><a:RelatesTo>%s</a:RelatesTo><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action></e:Header><e:Body><d:ProbeMatches><d:ProbeMatch><a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference><d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/ptz onvif://www.onvif.org/name/chuangmi-v2</d:Scopes><d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>",(long)time(NULL),id,uuid,ipaddr,HTTP_PORT);sendto(s,x,l,0,(void*)&f,z);}close(s);return NULL;}
int main(void){pthread_t h,w;signal(SIGINT,sigfn);signal(SIGTERM,sigfn);signal(SIGPIPE,SIG_IGN);getip("mlan0",ipaddr,sizeof(ipaddr));if(!strcmp(ipaddr,"127.0.0.1"))getip("wlan0",ipaddr,sizeof(ipaddr));make_uuid();load_state();if(motor_open()<0)logx("WARN","open %s: %s",MOTOR_DEV,strerror(errno));else{motor_refresh();logx("INFO","motor X=%d Y=%d",ptz.x,ptz.y);}pthread_create(&h,NULL,httpd,NULL);pthread_create(&w,NULL,wsdd,NULL);logx("INFO","device=%s http=%s:%d",uuid,ipaddr,HTTP_PORT);while(alive)sleep(1);stop_move();pthread_cancel(h);pthread_cancel(w);pthread_join(h,NULL);pthread_join(w,NULL);if(motor_fd>=0)close(motor_fd);return 0;}
