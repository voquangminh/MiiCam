/* @file rtspd.c
 *  Simple RTSP server demo
 * Copyright (C) 2013 GM Corp. (http://www.grain-media.com)
 *
 * $Revision: 1.5 $
 * $Date: 2014/12/30 05:37:57 $
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <libgen.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <dirent.h>

#include "gmlib.h"
#include <ctype.h>
#include "librtsp.h"
#include "log/log.h"
#include "algorithm/capture_motion_detection.c"

#define DVR_ENC_EBST_ENABLE      0x55887799
#define DVR_ENC_EBST_DISABLE     0

#define ENC_TYPE_H264            0
#define ENC_TYPE_MPEG4           1
#define ENC_TYPE_MJPEG           2

#define CAP_CH_NUM               1
#define RTSP_NUM_PER_CAP         1			   // * default is 4 - fix to 1 for stable stream
#define CAP_PATH_NUM             4
#define ENC_TRACK_NUM            4

#define SDPSTR_MAX               512		   // * default is 128
#define SR_MAX                   64
#define VQ_MAX                   (SR_MAX)
#define VQ_LEN                   100
#define AQ_MAX                   64	           	// * 1 MP2 and 1 AMR for live streaming, another 2 for file streaming.
#define AQ_LEN                   16				// * Higher value will increase latency; stock v5 firmware used 32
#define AV_NAME_MAX              127

#define RTP_HZ                   90000		   // * timestamp HW clock

#define ERR_GOTO(x, y)           do { ret = x; goto y; } while(0)
#define MUTEX_FAILED(x)          (x == ERR_MUTEX)
#define VIDEO_FRAME_NUMBER       VQ_LEN+1

#define NONE_BS_EVENT            0
#define START_BS_EVENT           1
#define STOP_BS_EVENT            2

#define MAX_SNAPSHOT_LEN         (256 * 1024)
#define MD_DATA_LEN              (720 * 576 / 4)

#define MOTION_ON_SCRIPT         "/tmp/sd/firmware/scripts/motion_on.sh"
#define MOTION_OFF_SCRIPT        "/tmp/sd/firmware/scripts/motion_off.sh"

#define RTSPD_LOGFILE            "/tmp/sd/log/rtspd.log"

#define CREATE_SNAPSHOT_FILE     "/dev/shm/rtspd_snapshot"
#define LAST_SNAPSHOT_PATH       "/dev/shm/rtspd_last_snapshot_path"

#define CREATE_VIDEO_FILE        "/dev/shm/rtspd_video"
#define LAST_VIDEO_PATH          "/dev/shm/rtspd_last_video_path"

/* ePTZ (digital zoom) control - written by the ONVIF server:
 * line format: "<zoom> <pan> <tilt>"
 *   zoom: 0.0 (1x) .. 1.0 (max zoom)
 *   pan : 0.0 .. 1.0  crop center X, normalized (0.5 = center)
 *   tilt: 0.0 .. 1.0  crop center Y, normalized (0.5 = center)
 */
#define RTSPD_ZOOM_FILE          "/dev/shm/rtspd_zoom"
#define MAX_ZOOM_FACTOR          4.0f
#define ZOOM_SMOOTH_STEP         0.05f

#define OSD_PALETTE_COLOR_AQUA              0xCA48CA93        /* YCrYCb */
#define OSD_PALETTE_COLOR_BLACK             0x10801080
#define OSD_PALETTE_COLOR_BLUE              0x296e29f0
#define OSD_PALETTE_COLOR_BROWN             0x51A1515B
#define OSD_PALETTE_COLOR_DODGERBLUE        0x693F69CB
#define OSD_PALETTE_COLOR_GRAY              0xB580B580
#define OSD_PALETTE_COLOR_GREEN             0x5151515B
#define OSD_PALETTE_COLOR_KHAKI             0x72897248
#define OSD_PALETTE_COLOR_LIGHTGREEN        0x90229036
#define OSD_PALETTE_COLOR_MAGENTA           0x6EDE6ECA
#define OSD_PALETTE_COLOR_ORANGE            0x98BC9851
#define OSD_PALETTE_COLOR_PINK              0xA5B3A589
#define OSD_PALETTE_COLOR_RED               0x52F0525A
#define OSD_PALETTE_COLOR_SLATEBLUE         0x3D603DA6
#define OSD_PALETTE_COLOR_WHITE             0xEB80EB80
#define OSD_PALETTE_COLOR_YELLOW            0xD292D210

#define RECORDING_DURATION       20
#define RECORDING_MAX_DURATION   30

#define CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num)    \
    do {    \
        if ((ch_num >= CAP_CH_NUM || ch_num < 0) || \
            (sub_num >= RTSP_NUM_PER_CAP || sub_num < 0)) {    \
            log_error("%s: ch_num=%d, sub_num=%d error!",__FUNCTION__, ch_num, sub_num);    \
            return -1; \
        }    \
    } while(0)    \

struct mdt_alg_t mdt_alg = {mb_cell_en: NULL};
struct mdt_result_t mdt_result[ENC_TRACK_NUM];

typedef struct {
    void *obj;
    gm_cap_attr_t cap_attr;
    gm_3dnr_attr_t dnr_attr;
} gm_cap_info_t;

typedef struct {
    void *obj;
    int enc_type;
    union {
        gm_h264e_attr_t h264e_attr;
        gm_mpeg4e_attr_t mpeg4e_attr;
        gm_mjpege_attr_t mjpege_attr;
    } codec;
} gm_enc_info_t;

typedef struct {
    gm_cap_info_t cap;
    gm_enc_info_t enc[ENC_TRACK_NUM];
    void *bindfd[ENC_TRACK_NUM];
} gm_enc_t;

void *enc_groupfd;
void *enc_audio_groupfd;
gm_enc_t enc_param[CAP_CH_NUM][CAP_PATH_NUM];

typedef int (*open_container_fn)(int ch_num, int sub_num);
typedef int (*close_container_fn)(int ch_num, int sub_num);

typedef enum st_opt_type {
    OPT_NONE=0,
    RTSP_LIVE_STREAMING,
} opt_type_t;

typedef struct st_vbs {
    int enabled;                  // * DVR_ENC_EBST_ENABLE: enabled, DVR_ENC_EBST_DISABLE: disabled
    int enc_type;                 // * 0:ENC_TYPE_H264, 1:ENC_TYPE_MPEG4, 2:ENC_TYPE_MJPEG
} vbs_t;

typedef struct st_priv_vbs {
    char sdpstr[SDPSTR_MAX];
    int qno;
    int offs;
    int len;
    unsigned int tv_ms;
    int cap_ch;
    int cap_path;
    int rec_track;
    char *bs_buf;
    unsigned int bs_buf_len;
    pthread_mutex_t priv_vbs_mutex;
} priv_vbs_t;

typedef struct st_bs {
    int event;                    // * Config change please set 1 for enqueue_thread to config this
    int enabled;                  // * DVR_ENC_EBST_ENABLE: enabled, DVR_ENC_EBST_DISABLE: disabled
    opt_type_t opt_type;          // * 1:rtsp_live_streaming, 2: file_avi_recording 3:file_h264_recording
    vbs_t video;                  // * VIDEO, 0: main-bitstream, 1: sub1-bitstream, 2:sub2-bitstream
} avbs_t;

typedef struct st_priv_bs {
    int play;
    int congest;
    int sr;
     char name[AV_NAME_MAX];
    open_container_fn open;
    close_container_fn close;
    priv_vbs_t video;             // * VIDEO, 0: main-bitstream, 1: sub1-bitstream, 2:sub2-bitstream
    priv_vbs_t audio;             // * AUDIO, 0: main-bitstream, 1: sub1-bitstream, 2:sub2-bitstream
} priv_avbs_t;

typedef struct st_av {
    // * Public data
    avbs_t bs[RTSP_NUM_PER_CAP];  // * VIDEO, 0: main-bitstream, 1: sub1-bitstream, 2:sub2-bitstream
    // * Update data
    pthread_mutex_t ubs_mutex;
    // * Private data
    int enabled;                  // * DVR_ENC_EBST_ENABLE: enabled, DVR_ENC_EBST_DISABLE: disabled
    priv_avbs_t priv_bs[RTSP_NUM_PER_CAP];
} av_t;

pthread_t enqueue_thread_id   		= 0;
pthread_t encode_thread_id    		= 0;
pthread_t audio_encode_thread_id 	= 0;
pthread_t zoom_thread_id       		= 0;
pthread_t motion_thread_id    		= 0;
pthread_t media_thread_id     		= 0;
pthread_t osd_thread_id       		= 0;

unsigned int sys_tick         = 0;
struct timeval sys_sec        = {-1, -1};
int sys_port                  = 554;
char *ipptr                   = NULL;

static int rtspd_sysinit      = 0;
static int rtspd_set_event    = 0;
static int rtspd_avail_ch     = 0;

char *snapshot_buf            = 0;
static int snapshot_create    = 0;
static int video_create       = 0;
static int motion_detected    = 0;

pthread_mutex_t stream_queue_mutex;
av_t enc[CAP_CH_NUM];
gm_system_t gm_system;

void *groupfd;                    // * Return of gm_new_groupfd()
void *bindfd;                     // * Return of gm_bind()
void *capture_object;
void *encode_object;
void *sub_enc_object;             // * Create encoder object (scaler)
void *sub_bindfd;                 // * Create encoder object (scaler) bind

void *audio_bindfd;
void *audio_grab_object;
void *audio_encode_object;

char *audio_data;

/* ePTZ zoom state */
static pthread_mutex_t zoom_mutex = PTHREAD_MUTEX_INITIALIZER;
static float zoom_factor  = 1.0f;   // * current zoom factor (1.0 = no zoom)
static float zoom_pan     = 0.5f;   // * crop center X, normalized
static float zoom_tilt    = 0.5f;   // * crop center Y, normalized
static float zoom_target  = 1.0f;   // * target zoom factor
static float zoom_tgt_pan = 0.5f;
static float zoom_tgt_tilt = 0.5f;

static unsigned short rtspd_osd_font2_text[64];
static int rtspd_osd_font2_ready = 0;
static pthread_mutex_t rtspd_osd_mutex = PTHREAD_MUTEX_INITIALIZER;

FILE *logfile = NULL;             // * File for logging

struct VideoRecording {
    int recording;
    int waiting_for_keyframe;
    struct timeval record_start;
    FILE *fh;
    FILE *fh_aac;
    char file_path[80];
    char audio_file_path[80];
} VideoRecorder;

struct timeval last_motion;

struct CommandLineArguments {
    int framerate;
    int height;
    int width;
    int bitrate;
    int bitrateMode;
    int gop;
    int encoderType;
    int snapshot;
    int record;
    int motion;
    int osd;
    int font_zoom;
    int osd_bg_color;
    char osd_text[32];

    /* Audio configuration (all types / sample rates supported by gmlib) */
    int audio_sample_rate;    /* 8000, 16000, 32000, 44100, 48000... */
    int audio_sample_size;    /* bits per sample: 8, 16 */
    int audio_bitrate;        /* in bits/sec */
    int audio_frame_samples;  /* samples per frame: PCM 250~2048, AAC 1024*n, ADPCM 505*n, G711 320*n */
    int audio_channel_type;   /* GM_MONO / GM_STEREO */
    int audio_encode_type;    /* GM_PCM / GM_AAC / GM_ADPCM / GM_G711_ALAW / GM_G711_ULAW */
    int audio_enabled;        /* 0 = RTSP stream without audio, 1 = with audio */

    /* Capture flip (gm_cap_flip_t) */
    int h_flip;
    int v_flip;

    /* Capture rotation (gm_rotation_attr_t) */
    int rotation;             /* 0, 90, 180, 270 */

    /* H264 profile_setting */
    int h264_profile;         /* 0=default, 66=baseline, 77=main, 100=high */
    int h264_level;           /* 0=default, 31=3.1, 40=4.0, 41=4.1, 50=5.0, 51=5.1 */
    int h264_config;          /* 0=default, 1=perf, 2=light, 3=quality */
    int h264_coding;          /* 0=default, 1=CABAC, 2=CAVLC */

    /* H264 VUI color info */
    int vui_colorspace;       /* matrix_coefficient: 0=undef, 1=bt709, 5=bt470bg, 6=smpte170 */
    int vui_full_range;       /* 0=limited, 1=full */

    /* Sample aspect ratio (SAR) */
    int sar_width;
    int sar_height;

    /* ROI encoding (gm_enc_roi_attr_t) */
    int roi_enabled;
    unsigned int roi_x, roi_y, roi_w, roi_h;

    /* ROI QP regions (gm_h264_roiqp_attr_t) - 8 regions */
    int roiqp_enabled;

    /* Fractional framerate (gm_h264e_attr_t.fps_ratio) */
    int fps_ratio_num;
    int fps_ratio_den;
} cliArgs;

/* Read HOSTNAME from config file. Try common locations. */
static void read_hostname(char *out, size_t outlen)
{
    FILE *f;
    char line[256];
    out[0] = '\0';
    f = fopen("/tmp/sd/config.cfg", "r");
	
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "HOSTNAME=", 9) != 0)
            continue;
        char *v = line + 9;
        if (*v == '"')
            v++;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == '"'  || end[-1] == ' ')) {
            *--end = '\0';
        }
        strncpy(out, v, outlen - 1);
        out[outlen - 1] = '\0';
        break;
    }
    fclose(f);
}

static gm_palette_table_t rtspd_osd_palette = {
    palette_table: {
        OSD_PALETTE_COLOR_AQUA,
        OSD_PALETTE_COLOR_BLACK,
        OSD_PALETTE_COLOR_BLUE,
        OSD_PALETTE_COLOR_BROWN,
        OSD_PALETTE_COLOR_DODGERBLUE,
        OSD_PALETTE_COLOR_GRAY,
        OSD_PALETTE_COLOR_GREEN,
        OSD_PALETTE_COLOR_KHAKI,
        OSD_PALETTE_COLOR_LIGHTGREEN,
        OSD_PALETTE_COLOR_MAGENTA,
        OSD_PALETTE_COLOR_ORANGE,
        OSD_PALETTE_COLOR_PINK,
        OSD_PALETTE_COLOR_RED,
        OSD_PALETTE_COLOR_SLATEBLUE,
        OSD_PALETTE_COLOR_WHITE,
        OSD_PALETTE_COLOR_YELLOW
    }
};

static void rtspd_set_osd_text(void *capture_obj, const char *line1, const char *line2);
static void rtspd_update_osd_text(void *capture_obj);
static void rtspd_enable_osd_font(void *capture_obj, const char *text)
{
    char line1[64];
    char timestamp[32];
    time_t now;
    struct tm tm;

    if (text == NULL || text[0] == '\0')
        snprintf(line1, sizeof(line1), "chuangmi");
    else
        snprintf(line1, sizeof(line1), "%s", text);
    if (capture_obj != NULL) {
        now = time(NULL);
        localtime_r(&now, &tm);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
        rtspd_set_osd_text(capture_obj, line1, timestamp);
        rtspd_update_osd_text(capture_obj);
    }
}

static void rtspd_set_osd_palette(void)
{
    gm_set_palette_table(&rtspd_osd_palette);
}

static void rtspd_set_osd_text(void *capture_obj, const char *line1, const char *line2)
{
    gm_osd_font2_t osd_font2;
    
    int h_words;
    int v_words;
    int line1_len;
    int line2_len;
    int i;

    if (capture_obj == NULL || line1 == NULL || line2 == NULL)
        return;
    line1_len = strlen(line1);
    line2_len = strlen(line2);
    h_words = (line1_len > line2_len) ? line1_len : line2_len;
    v_words = 2;
    if (h_words > 32)
        h_words = 32;
    if (capture_obj == NULL || line1 == NULL || line2 == NULL)
        return;
    line1_len = strlen(line1);
    if (line1_len > h_words)
        line1_len = h_words;
    line2_len = strlen(line2);
    if (line2_len > h_words)
        line2_len = h_words;
    pthread_mutex_lock(&rtspd_osd_mutex);

    for (i = 0; i < (int)(sizeof(rtspd_osd_font2_text) / sizeof(rtspd_osd_font2_text[0])); i++)
        rtspd_osd_font2_text[i] = (unsigned short) ' ';
    for (i = 0; i < line1_len; i++)
        rtspd_osd_font2_text[i] = (unsigned short) line1[i];
    for (i = 0; i < line2_len; i++)
        rtspd_osd_font2_text[h_words + i] = (unsigned short) line2[i];

    memset(&osd_font2, 0, sizeof(osd_font2));
    osd_font2.enabled = 1;
    osd_font2.win_idx = 0;
    osd_font2.align_type = GM_OSD_ALIGN_TOP_LEFT;
    osd_font2.x = 10;
    osd_font2.y = 10;
    osd_font2.h_words = h_words;
    osd_font2.v_words = v_words;
    osd_font2.h_space = 0;
    osd_font2.v_space = 0;
    osd_font2.font_index_len = h_words * v_words;
    osd_font2.font_index = rtspd_osd_font2_text;
    osd_font2.font_alpha = GM_OSD_FONT_ALPHA_75;
    osd_font2.win_alpha = GM_OSD_FONT_ALPHA_75;
    osd_font2.font_palette_idx = 14;   							// WHITE
    osd_font2.priority = GM_OSD_PRIORITY_MARK_ON_OSD;
    osd_font2.smooth.enabled = 1;
    osd_font2.smooth.level = GM_OSD_FONT_SMOOTH_LEVEL_WEAK;
    osd_font2.marquee.mode = GM_OSD_MARQUEE_MODE_NONE;
    osd_font2.win_palette_idx  = cliArgs.osd_bg_color;  		// background color
    osd_font2.border.enabled = 0;   							// without border
    osd_font2.border.width = 1;
    osd_font2.border.type = GM_OSD_BORDER_TYPE_WIN;
    osd_font2.border.palette_idx = 1;
    osd_font2.font_zoom = cliArgs.font_zoom;

    int ret;
    ret = gm_set_osd_font2(capture_obj, &osd_font2);
    rtspd_osd_font2_ready = 1;
    pthread_mutex_unlock(&rtspd_osd_mutex);
}

static void rtspd_update_osd_text(void *capture_obj)
{
    char timestamp[32];
    char line1[64];
    time_t now;
    struct tm tm;

    if (capture_obj == NULL)
        return;
    if (cliArgs.osd_text[0] != '\0')
        snprintf(line1, sizeof(line1), "%s", cliArgs.osd_text);
    else
        snprintf(line1, sizeof(line1), "chuangmi");
    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
    rtspd_set_osd_text(capture_obj, line1, timestamp);
}

static void *rtspd_osd_thread(void *arg)
{
    void *capture_obj = arg;

    while (rtspd_sysinit == 1) {
        rtspd_update_osd_text(capture_obj);
        sleep(1);
    }
    return NULL;
}

static char *rtsp_enc_type_str[] = {
    "H264 ",
    "MPEG4",
    "MJPEG"
};

char *rtsp_password = NULL;
char *rtsp_username = NULL;
static int rtsp_use_auth = 0;

static void create_directory(const char *dir)
{
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for(p = tmp + 1; *p; p++) {
        if(*p == '/') {
           *p = 0;
           mkdir(tmp, S_IRWXU);
           *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

int start_recording(void)
{
    struct tm *sTm;
    time_t now = time(0);
    sTm = gmtime(&now);

    char dirstring[40];
    char filestring[40];
    char audio_filestring[40];

    strftime(dirstring, sizeof(dirstring), "/tmp/sd/RECORDED_VIDEOS/%Y/%m/%d", sTm);

    struct stat st = {0};
    if (stat(dirstring, &st) != 0) {
        create_directory(dirstring);
    }

    strftime(filestring, sizeof(filestring), "video_%H%M%S.h264", sTm);
    strftime(audio_filestring, sizeof(audio_filestring), "audio_%H%M%S.aac", sTm);

    sprintf(VideoRecorder.file_path, "%s/%s%c", dirstring, filestring, '\0');
    sprintf(VideoRecorder.audio_file_path, "%s/%s%c", dirstring, audio_filestring, '\0');
    log_info("Video recording to %s", VideoRecorder.file_path);
    log_info("Audio recording to %s", VideoRecorder.audio_file_path);

    // * Open video recording file
    VideoRecorder.fh = fopen(VideoRecorder.file_path, "wb");
    if (VideoRecorder.fh == NULL) {
        log_error("Failed to open file %s", VideoRecorder.file_path);
        return -1;
    }

    VideoRecorder.fh_aac = fopen(VideoRecorder.audio_file_path, "wb");
    if (VideoRecorder.fh_aac == NULL) {
        log_error("Failed to open audio file %s", VideoRecorder.audio_file_path);
        fclose(VideoRecorder.fh);
        VideoRecorder.fh = NULL;
        return -1;
    }

    // * Write filename of last video to file
    FILE *last_video_path = fopen(LAST_VIDEO_PATH, "wb");
    if (last_video_path == NULL) {
        log_error("Failed to open file: %s", LAST_VIDEO_PATH);
        return -1;
    }

    fputs(VideoRecorder.file_path, last_video_path);
    fclose(last_video_path);

    gettimeofday(&VideoRecorder.record_start, NULL);
    VideoRecorder.recording = 1;
    VideoRecorder.waiting_for_keyframe = 1;

    return 0;
}

int stop_recording(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    log_info("Stopping video recording after %ld seconds", now.tv_sec - VideoRecorder.record_start.tv_sec);
    // * Stop accepting new writes first, then close the files (safe vs. video/audio threads)
    VideoRecorder.recording = 0;
    // * Close video file
    if (VideoRecorder.fh)
        fclose(VideoRecorder.fh);
    VideoRecorder.fh = NULL;
    if (VideoRecorder.fh_aac)
        fclose(VideoRecorder.fh_aac);
    VideoRecorder.fh_aac = NULL;
    // * Reset all Recorder settings to zero
    VideoRecorder.waiting_for_keyframe = 1;
    VideoRecorder.file_path[0] = '\0';
    VideoRecorder.audio_file_path[0] = '\0';
    return 0;
}

int init_recording(void) {
    // * Only create when it doesn't exist (we'll write it in init script)
    if (access(LAST_VIDEO_PATH, F_OK ) == -1) {
        FILE *last_video_path = fopen(LAST_VIDEO_PATH, "wb");
        if (last_video_path == NULL) {
            log_error("Failed to open file: %s", LAST_VIDEO_PATH);
            return -1;
        }
        fputs("unknown", last_video_path);
        fclose(last_video_path);
    }
    return 0;
}

static int set_cap_motion(int cap_vch, unsigned int id, unsigned int value)
{
    int ret = 0;
    gm_cap_motion_t cap_motion;

    cap_motion.id = id; //alpha
    cap_motion.value = value;
    ret = gm_set_cap_motion(cap_vch, &cap_motion);
    if (ret < 0) {
        log_error("Failed to run gm_set_cap_motion");
        return -1;
    }
    return 0;
}

// * Training motion
static int set_interesting_area(int ch)
{
    int ret = 0;
    int mb_w_num, mb_h_num;
    int h, w;
    gm_enc_t *param;

    mdt_alg.u_width       = gm_system.cap[ch].dim.width;
    mdt_alg.u_height      = gm_system.cap[ch].dim.height;
    mdt_alg.u_mb_width    = 32;
    mdt_alg.u_mb_height   = 32;
    mdt_alg.training_time = 15;
    mdt_alg.frame_count   = 0;
    mdt_alg.sensitive_th  = 80;
    mdt_alg.alarm_th      = 17;  // mb_h_num * mb_w_num * (5/100)

    mb_w_num              = (mdt_alg.u_width + (mdt_alg.u_mb_width - 1)) / mdt_alg.u_mb_width;
    mb_h_num              = (mdt_alg.u_height + (mdt_alg.u_mb_height - 1)) / mdt_alg.u_mb_height;
    mdt_alg.mb_w_num      = mb_w_num;
    mdt_alg.mb_h_num      = mb_h_num;

    mdt_alg.mb_cell_en = (unsigned char *)malloc(sizeof(unsigned char) * mb_w_num * mb_h_num);
    if (mdt_alg.mb_cell_en == NULL) {
        log_error("Failed to allocate mb_cell_en");
        ret = -1;
        goto err_ext;
    }
    memset(mdt_alg.mb_cell_en, 0, (sizeof(unsigned char) * mb_w_num * mb_h_num));
    // * Set Area
    for (h = 0; h < mb_h_num; h++) {
        for (w = 0; w < mb_w_num; w++) {
            mdt_alg.mb_cell_en[(h * mb_w_num + w)] = 1;
        }
    }

    set_cap_motion(ch, 0, 32);      // * Alpha
    set_cap_motion(ch, 1, 7371);    // * TBG
    set_cap_motion(ch, 2, 7);       // * Init val
    set_cap_motion(ch, 3, 9);       // * TB
    set_cap_motion(ch, 4, 11);      // * Sigma
    set_cap_motion(ch, 5, 15);      // * Prune
    set_cap_motion(ch, 7, 0x9ffb0); // * Alpha accuracy
    set_cap_motion(ch, 8, 9);       // * TG
    set_cap_motion(ch, 10, 0x7fe0); // * One min alpha

    param = &enc_param[0][0];
    ret = motion_detection_update(param->bindfd[0], &mdt_alg);
    if (ret != 0) {
        log_error("Failed to execute motion_detection_update");
        ret = -1;
        goto err_ext;
    }

err_ext:
    if (mdt_alg.mb_cell_en)
        free(mdt_alg.mb_cell_en);
    return ret;
}


int init_snapshot(void)
{
    // * Only create the file if it doesn't exist (it's filled it init script)
    if (access(LAST_SNAPSHOT_PATH, F_OK ) == -1) {
        FILE *fd = fopen(LAST_SNAPSHOT_PATH, "wb");
        if (fd == NULL) {
            log_error("Failed to open file %s", LAST_SNAPSHOT_PATH);
            return -1;
        }
        fputs("unknown", fd);
        fclose(fd);
    }
    return 0;
}

void take_snapshot(void)
{
    struct tm *sTm;
    time_t now = time(0);
    sTm = gmtime(&now);

    char dirstring[40];
    char filestring[40];
    char full_file_path[80];

    int snapshot_len = 0;
    FILE *snapshot_fd = NULL;
    FILE *snapshot_name_fd = NULL;

    gm_enc_t *param;
    snapshot_t snapshot;

    if (snapshot_buf == NULL) {
        snapshot_buf = (char *)malloc(MAX_SNAPSHOT_LEN);
        if (snapshot_buf == NULL) {
            log_error("Failed allocating snapshot memory buffer");
            return;
        }
    }

    param = &enc_param[0][0];
    snapshot.bindfd = param->bindfd[0];
    snapshot.image_quality = 80;                        // The value of image quality from 1(worst) ~ 100(best)
    snapshot.bs_buf = snapshot_buf;
    snapshot.bs_buf_len = MAX_SNAPSHOT_LEN;
    snapshot.bs_width = 1280;
    snapshot.bs_height = 720;

    snapshot_len = gm_request_snapshot(&snapshot, 500); // Timeout value 500ms

    if (snapshot_len > 0) {
        strftime(dirstring, sizeof(dirstring), "/tmp/sd/RECORDED_IMAGES/%Y/%m/%d", sTm);
        struct stat st = {0};
        if (stat(dirstring, &st) != 0) {
            create_directory(dirstring);
        }
        strftime(filestring, sizeof(filestring), "snapshot_%H%M%S.jpg", sTm);
        sprintf(full_file_path, "%s/%s%c", dirstring, filestring, '\0');
        log_info("Image %s size %d bytes", full_file_path, snapshot_len);
        // * Write image to file
        snapshot_fd = fopen(full_file_path, "wb");
        if (snapshot_fd == NULL) {
            log_error("Failed to open file %s", full_file_path);
            exit(EXIT_FAILURE);
        }
        fwrite(snapshot_buf, 1, snapshot_len, snapshot_fd);
        fclose(snapshot_fd);
        // * Write filename to /dev/shm
        snapshot_name_fd = fopen(LAST_SNAPSHOT_PATH, "wb");
        if (snapshot_name_fd == NULL) {
            log_error("Failed to open file %s", LAST_SNAPSHOT_PATH);
            exit(EXIT_FAILURE);
        }
        fputs(full_file_path, snapshot_name_fd);
        fclose(snapshot_name_fd);
        free(snapshot_buf);
        snapshot_buf = NULL;
    }
	else {
        if (snapshot_len == -1) {
            log_error("Failed to retrieve snapshot data");
        } else if (snapshot_len == -2) {
            log_error("Buffer too small to store snapshot data");
        } else if (snapshot_len == -4) {
            log_error("Timeout while waiting for snapshot data");
        }
        if (snapshot_buf) {
            free(snapshot_buf);
            snapshot_buf = NULL;
        }
	}
}

static int do_queue_alloc(int type)
{
    int rc;
    do {
        rc = stream_queue_alloc(type);
    } while MUTEX_FAILED(rc);
    return rc;
}

static unsigned int get_tick_gm(unsigned int tv_ms)
{
    sys_tick = tv_ms*(RTP_HZ / 1000);
    return sys_tick;
}

static int convert_gmss_media_type(int type)
{
    int media_type;

    switch(type) {
        case ENC_TYPE_H264:
            media_type = GM_SS_TYPE_H264;
            break;
        case ENC_TYPE_MPEG4:
            media_type = GM_SS_TYPE_MP4;
            break;
        case ENC_TYPE_MJPEG:
            media_type = GM_SS_TYPE_MJPEG;
            break;
        default:
            media_type  = -1;
            log_error("convert_gmss_media_type: type=%d, error!", type);
            break;
    }
    return media_type;
}

/* Map gmlib audio encoder type to librtsp stream audio type */
static int convert_gmss_audio_type(int enc_type)
{
    switch (enc_type) {
        case GM_PCM:
            return GM_SS_TYPE_PCM;
        case GM_AAC:
            return GM_SS_TYPE_AAC;
        case GM_ADPCM:
            return GM_SS_TYPE_G726;
        case GM_G711_ALAW:
            return GM_SS_TYPE_G711A;
        case GM_G711_ULAW:
            return GM_SS_TYPE_G711U;
        default:
            log_error("convert_gmss_audio_type: enc_type=%d not supported!", enc_type);
            return -1;
    }
}

static const char *audio_encode_type_name(int enc_type)
{
    switch (enc_type) {
        case GM_PCM:        return "PCM";
        case GM_AAC:        return "AAC";
        case GM_ADPCM:      return "G726/ADPCM";
        case GM_G711_ALAW:  return "G711A";
        case GM_G711_ULAW:  return "G711U";
        default:            return "UNKNOWN";
    }
}

/* RTP timestamp clock (Hz) for the audio payload, per RFC 3551/3640 */
static int audio_rtp_clock(int enc_type, int sample_rate)
{
    switch (enc_type) {
        case GM_AAC:
        case GM_PCM:
            return sample_rate;
        case GM_ADPCM:
        case GM_G711_ALAW:
        case GM_G711_ULAW:
            return 8000;
        default:
            return sample_rate;
    }
}

/* Sampling frequency index of the AAC AudioSpecificConfig (ISO/IEC 14496-3) */
static int aac_sampling_freq_index(int sample_rate)
{
    switch (sample_rate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000:  return 11;
        case 7350:  return 12;
        default:    return 11;
    }
}

/* Build the audio SDP parameter string that librtsp embeds into a=fmtp: */
static void build_audio_sdp(int enc_type, int sample_rate, int channels, char *sdp, int sdp_len)
{
    switch (enc_type) {
        case GM_AAC: {
            /* AudioSpecificConfig (2 bytes): audioObjectType(5) | samplingFrequencyIndex(4) | channelConfiguration(4) */
            unsigned int config = (2 << 11) | (aac_sampling_freq_index(sample_rate) << 7) | ((channels & 0x0f) << 3);
            snprintf(sdp, sdp_len, "%X", config);
            break;
        }
        case GM_PCM:
        case GM_ADPCM:
        case GM_G711_ALAW:
        case GM_G711_ULAW:
            /* No extra config parameter required by librtsp for these codecs */
            sdp[0] = '\0';
            break;
        default:
            sdp[0] = '\0';
            break;
    }
}

static int open_live_streaming(int ch_num, int sub_num)
{
    int media_type;
    avbs_t *b;
    priv_avbs_t *pb;
    char livename[64];

    CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num);
    b = &enc[ch_num].bs[sub_num];
    pb = &enc[ch_num].priv_bs[sub_num];
    //log_info("OPEN STREAM sub=%d pb=%p vsdp='%s'",sub_num,pb,pb->video.sdpstr);
    media_type = convert_gmss_media_type(b->video.enc_type);
    pb->video.qno = do_queue_alloc(media_type);

    if (cliArgs.audio_enabled) {
        pb->audio.qno = do_queue_alloc(convert_gmss_audio_type(cliArgs.audio_encode_type));
        build_audio_sdp(cliArgs.audio_encode_type, cliArgs.audio_sample_rate,
                        cliArgs.audio_channel_type, pb->audio.sdpstr, SDPSTR_MAX);
    } else {
        pb->audio.qno = 0;
        pb->audio.sdpstr[0] = '\0';
    }

    sprintf(livename, "live/ch%02d_%d", ch_num, sub_num);
    pb->sr = stream_reg(livename, pb->video.qno, pb->video.sdpstr,pb->audio.qno, pb->audio.sdpstr, 1, 0, 0, 0, 0, 0, 0);

    if (pb->sr < 0){
        log_error("open_live_streaming: ch_num=%d, sub_num=%d setup error", ch_num, sub_num);
	}
    // * Enable authentication for the stream if the username and password are set
    if (rtsp_use_auth == 1) {
        stream_authorization(pb->sr, rtsp_username, rtsp_password);
    }
    strcpy(pb->name, livename);
    return 0;
}

#define TIMEVAL_DIFF(start, end) (((end.tv_sec)-(start.tv_sec))*1000000+((end.tv_usec)-(start.tv_usec)))
static int write_rtp_frame_ext(int ch_num, int sub_num, void *data, int data_len, unsigned int tv_ms)
{
    int ret, media_type;
    avbs_t *b;
    priv_avbs_t *pb;
    gm_ss_entity entity;
    struct timeval curr_tval;
    static struct timeval err_print_tval;

    pb = &enc[ch_num].priv_bs[sub_num];
    b  = &enc[ch_num].bs[sub_num];

    if ( pb->play == 0 || (b->event != NONE_BS_EVENT) ) {
        ret = 1;
        goto exit_free_as_buf;
    }

    entity.data = (char *) data;
    entity.size = data_len;
    entity.timestamp = get_tick_gm(tv_ms);
    media_type = convert_gmss_media_type(b->video.enc_type);
    pthread_mutex_lock(&stream_queue_mutex);
    ret = stream_media_enqueue(media_type, pb->video.qno, &entity);
    pthread_mutex_unlock(&stream_queue_mutex);

    if ( ret < 0 ) {
        gettimeofday(&curr_tval, NULL );
        if ( ret == ERR_FULL) {
            pb->congest = 1;
            if ( TIMEVAL_DIFF(err_print_tval, curr_tval) > 5000000 ) {
                log_error("ext enqueue queue ch_num=%d, sub_num=%d full", ch_num, sub_num);
			}
		}
        else if ((ret != ERR_NOTINIT)&& (ret != ERR_MUTEX) && (ret != ERR_NOTRUN)) {
            if (TIMEVAL_DIFF(err_print_tval, curr_tval) > 5000000) {
                log_error("ext enqueue queue ch_num=%d, sub_num=%d error %d", ch_num, sub_num, ret);
			}
		}
        if ( TIMEVAL_DIFF(err_print_tval, curr_tval) > 5000000) {
            log_error("ext enqueue queue ch_num=%d, sub_num=%d error %d", ch_num, sub_num, ret);
            gettimeofday(&err_print_tval, NULL );
        }
        goto exit_free_audio_buf;
    }
    return 0;

exit_free_audio_buf:
    //put_video_frame(pb->video.qno, fs);
exit_free_as_buf:
    //free_bs_data(ch_num, sub_num, q);
    return 1;
}

static int close_live_streaming(int ch_num, int sub_num)
{
    avbs_t *b;
    priv_avbs_t *pb;
    int ret = 0;

    CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num);
    b = &enc[ch_num].bs[sub_num];
    pb = &enc[ch_num].priv_bs[sub_num];
    if (pb->sr >= 0) {
        ret = stream_dereg(pb->sr, 1);
        if (ret < 0)
            goto err_exit;
        pb->sr = -1;
        pb->video.qno = -1;
        pb->play = 0;
    }

err_exit:
    if (ret < 0)
        log_error("%s: stream_dereg(%d) err %d", __func__, pb->sr, ret);
    return ret;
}

int open_bs(int ch_num, int sub_num)
{
    avbs_t *b;
    priv_avbs_t *pb;

    CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num);
    pb = &enc[ch_num].priv_bs[sub_num];
    b = &enc[ch_num].bs[sub_num];

    enc[ch_num].enabled = DVR_ENC_EBST_ENABLE;
    enc[ch_num].bs[sub_num].enabled = DVR_ENC_EBST_ENABLE;
    enc[ch_num].bs[sub_num].video.enabled = DVR_ENC_EBST_ENABLE;

    switch (b->opt_type) {
        case RTSP_LIVE_STREAMING:
            pb->open = open_live_streaming;
            pb->close = close_live_streaming;
            break;
        case OPT_NONE:
        default:
            break;
    }
    return 0;
}

int close_bs(int ch_num, int sub_num)
{
    av_t *e;
    int sub, is_close_channel = 1;

    CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num);
    e = &enc[ch_num];

    e->bs[sub_num].video.enabled = DVR_ENC_EBST_DISABLE;
    e->bs[sub_num].enabled = DVR_ENC_EBST_DISABLE;
    for (sub = 0; sub < RTSP_NUM_PER_CAP; sub++) {
        if (e->bs[sub].video.enabled == DVR_ENC_EBST_ENABLE) {
            is_close_channel = 0;
            break;
        }
    }
    if (is_close_channel == 1)
        enc[ch_num].enabled = DVR_ENC_EBST_DISABLE;
    return 0;
}

static int bs_check_event(void)
{
    int ch_num, sub_num, ret = 0;
    avbs_t *b;

    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        for (sub_num = 0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            b = &enc[ch_num].bs[sub_num];
            if (b->event != NONE_BS_EVENT) {
                ret = 1;
                break;
            }
        }
    }
    return ret;
}

void bs_new_event(void)
{
    int ch_num, sub_num;
    avbs_t *b;
    priv_avbs_t *pb;

    if (bs_check_event() == 0) {
        rtspd_set_event = 0;
        return;
    }

    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        pthread_mutex_lock(&enc[ch_num].ubs_mutex);
        for (sub_num = 0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            b = &enc[ch_num].bs[sub_num];
            pb = &enc[ch_num].priv_bs[sub_num];
            switch (b->event) {
                case START_BS_EVENT:
                    open_bs(ch_num, sub_num);
                    if (pb->open) pb->open(ch_num, sub_num);
                    b->event = NONE_BS_EVENT;
                    break;
                case STOP_BS_EVENT:
                    pb->open = NULL;
                    if (pb->close) {
                        pb->close(ch_num, sub_num);
                        pb->close = NULL;
                        close_bs(ch_num, sub_num);
                    }
                    b->event = NONE_BS_EVENT;
                    break;
                default:
                    break;
            }
        }
        pthread_mutex_unlock(&enc[ch_num].ubs_mutex);
    }
}

int env_set_bs_new_event(int ch_num, int sub_num, int event)
{
    avbs_t *b;
    int ret = 0;

    CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num);
    b = &enc[ch_num].bs[sub_num];

    switch (event) {
        case START_BS_EVENT:
            if (b->opt_type == OPT_NONE)
                goto err_exit;
            if (b->enabled == DVR_ENC_EBST_ENABLE) {
                log_error("Already enabled: ch_num=%d, sub_num=%d", ch_num, sub_num);
                ret = -1;
                goto err_exit;
            }
            break;
        case STOP_BS_EVENT:
            if (b->enabled != DVR_ENC_EBST_ENABLE) {
                log_error("Already disabled: ch_num=%d, sub_num=%d", ch_num, sub_num);
                ret = -1;
                goto err_exit;
            }
            break;
        default:
            log_error("env_set_bs_new_event: ch_num=%d, sub_num=%d, event=%d, error", ch_num, sub_num, event);
            ret = -1;
            goto err_exit;
    }
    b->event = event;
    rtspd_set_event = 1;

err_exit:
    return ret;
}

int set_poll_event(void)
{
    int ch_num, sub_num, ret = -1;
    av_t *e;
    avbs_t *b;

    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        e = &enc[ch_num];
        if (e->enabled != DVR_ENC_EBST_ENABLE)
            continue;
        for (sub_num = 0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            b = &e->bs[sub_num];
            if (b->video.enabled == DVR_ENC_EBST_ENABLE) {
                ret = 0;
			}
		}
    }
    return ret;
}

void get_enc_res(gm_enc_info_t *enc, int *enc_type, int *width, int *height)
{
    int w = 0; 
    int h = 0;
    gm_h264e_attr_t *h264e_attr;
    gm_mpeg4e_attr_t *mpeg4e_attr;
    gm_mjpege_attr_t *mjpege_attr;

    switch (enc->enc_type) {
        case ENC_TYPE_H264:
            h264e_attr  = &enc->codec.h264e_attr;
            w           = h264e_attr->dim.width;
            h           = h264e_attr->dim.height;
            break;
        case ENC_TYPE_MPEG4:
            mpeg4e_attr = &enc->codec.mpeg4e_attr;
            w           = mpeg4e_attr->dim.width;
            h           = mpeg4e_attr->dim.height;
            break;
        case ENC_TYPE_MJPEG:
            mjpege_attr = &enc->codec.mjpege_attr;
            w           = mjpege_attr->dim.width;
            h           = mjpege_attr->dim.height;
            break;
    }
    if (enc_type)
        *enc_type = enc->enc_type;
    if (width)
        *width = w;
    if (height)
        *height = h;
}

#define PRINT_INTERVAL_MS 30000
static unsigned int frame_counts[CAP_CH_NUM][RTSP_NUM_PER_CAP] = {{0}};
static unsigned int rec_bs_len[CAP_CH_NUM][RTSP_NUM_PER_CAP]   = {{0}};
static void print_enc_average(int ch_num, int sub_num, int bs_len, struct timeval *cur_timeval)
{
    int enc_type, w, h;
    static struct timeval last_timeval;
    static unsigned int total_ms, print_init = 0;
    unsigned int diff_ms, i, j;
    char res_str[20];
    priv_avbs_t *pb;
    gm_enc_info_t *gm_enc;

    frame_counts[ch_num][sub_num]++;
    rec_bs_len[ch_num][sub_num] += bs_len;

    if (print_init == 0) {
        last_timeval.tv_sec = cur_timeval->tv_sec;
        last_timeval.tv_usec = cur_timeval->tv_usec;
        print_init = 1;
        total_ms = 0;
        return;
    }

    // * Get diff time
    if (cur_timeval->tv_sec > last_timeval.tv_sec) {
        diff_ms = 1000 + (cur_timeval->tv_usec / 1000) - (last_timeval.tv_usec / 1000);
        diff_ms += (cur_timeval->tv_sec - last_timeval.tv_sec - 1) * 1000;
    } else {
        diff_ms = (cur_timeval->tv_usec - last_timeval.tv_usec) / 1000;
    }
    total_ms += diff_ms;

    // * Show statistic
    if (total_ms >= PRINT_INTERVAL_MS) {
        for (i = 0; i < CAP_CH_NUM; i++) {
            for (j = 0; j < RTSP_NUM_PER_CAP; j++) {
                if (frame_counts[i][j] == 0)
                    continue;
                pb = &enc[i].priv_bs[j];
                gm_enc = &enc_param[pb->video.cap_ch][pb->video.cap_path].enc[pb->video.rec_track];
                get_enc_res(gm_enc, &enc_type, &w, &h);
                sprintf(res_str, "%dx%d", w, h);
                log_info("path=/live/ch%02d_%d cap=%d_%d size=%s enc=%s fps=%d.%d kbps=%d record=%d motion=%d",
                        i,
                        j,
                        pb->video.cap_ch,
                        pb->video.cap_path,
                        res_str,
                        rtsp_enc_type_str[enc_type],
                        (frame_counts[i][j] * 1000 / total_ms),
                        (frame_counts[i][j] * 100000 / total_ms) % 100,
                        (rec_bs_len[i][j] * 8 / 1024) * 1000 / total_ms,
                        VideoRecorder.recording,
                        motion_detected);

                if (VideoRecorder.recording == 1) {
                    log_info("Recording to %s", VideoRecorder.file_path);
                }
                frame_counts[i][j] = 0;
                rec_bs_len[i][j] = 0;
            }
        }
        total_ms = 0;
    }
    last_timeval.tv_sec = cur_timeval->tv_sec;
    last_timeval.tv_usec = cur_timeval->tv_usec;
}

#define POLL_WAIT_TIME 15000
static unsigned int poll_wait_time = 0;
static void env_release_resources(void)
{
    int ret, ch_num;
    av_t *e;

    if ((ret = stream_server_stop()))
        log_error("stream_server_stop() error %d", ret);
    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        e = &enc[ch_num];
        pthread_mutex_destroy(&e->ubs_mutex);
    }
}

static int frm_cb(int type, int qno, gm_ss_entity *entity)
{
    priv_avbs_t *pb;
    int ch_num, sub_num;
    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        for (sub_num = 0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            pb = &enc[ch_num].priv_bs[sub_num];
            if (pb->video.offs == (int)(entity->data) && pb->video.len == entity->size && pb->video.qno==qno) {
                pthread_mutex_lock(&pb->video.priv_vbs_mutex);
                pb->video.offs = 0;
                pb->video.len = 0;
                pthread_mutex_unlock(&pb->video.priv_vbs_mutex);
            }
        }
    }
    return 0;
}

priv_avbs_t *find_file_sr(char *name, int srno)
{
    int ch_num, sub_num, hit=0;
    priv_avbs_t *pb;

    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        for (sub_num = 0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            pb = &enc[ch_num].priv_bs[sub_num];
            if ((pb->sr == srno) && (pb->name) && (strcmp(pb->name, name) == 0)) {
                hit = 1;
                break;
            }
        }
        if (hit)
            break;
    }
    return (hit ? pb : NULL);
}

static int cmd_cb(char *name, int sno, int cmd, void *p)
{
    int ret = -1;
    priv_avbs_t *pb;
    
    switch(cmd) {
        case GM_STREAM_CMD_OPTION:
            ret = 0;
            break;
        case GM_STREAM_CMD_DESCRIBE:
            ret = 0;
            break;
        case GM_STREAM_CMD_OPEN:
            log_error("%s:%d <GM_STREAM_CMD_OPEN>", __FUNCTION__, __LINE__);
            ERR_GOTO(-10, cmd_cb_err);
            break;
        case GM_STREAM_CMD_SETUP:
            ret = 0;
            break;
        case GM_STREAM_CMD_PLAY:
            if ( strncmp(name, "live/", 5) == 0 ) {
                if ((pb = find_file_sr(name, sno)) == NULL)
                    ERR_GOTO(-1, cmd_cb_err);
                if (pb->video.qno >= 0)
                    pb->play = 1;
            }
            ret = 0;
            break;
        case GM_STREAM_CMD_PAUSE:
            log_info("%s:%d <GM_STREAM_CMD_PAUSE>", __FUNCTION__, __LINE__);
            ret = 0;
            break;
        case GM_STREAM_CMD_TEARDOWN:
            if ( strncmp(name, "live/", 5) == 0 ) {
                if ((pb = find_file_sr(name, sno)) == NULL)
                    ERR_GOTO(-1, cmd_cb_err);
                pb->play = 0;
            }
            ret = 0;
            break;
        default:
            log_error("%s: not support cmd %d", __func__, cmd);
            break;
    }

cmd_cb_err:
    if ( ret < 0 ) {
        log_error("%s: cmd %d error %d", __func__, cmd, ret);
    }
    return ret;
}

static void *media_thread(void *arg)
{
    struct timeval now;

    // * Reset all to zero before entering loop
    VideoRecorder.recording    = 0;
    VideoRecorder.file_path[0] = '\0';
    VideoRecorder.audio_file_path[0] = '\0';
    VideoRecorder.fh           = NULL;
    VideoRecorder.fh_aac       = NULL;

    // * Inititialize recording
    if (init_recording() < 0)
        log_error("Failed to initialize recording");
    // * Inititialize snapshot
    if (init_snapshot() < 0)
        log_error("Failed to initialize snapshot");
    while (rtspd_sysinit) {
        // * Check for external snapshot trigger
        if (access(CREATE_SNAPSHOT_FILE, F_OK ) != -1 ) {
            unlink(CREATE_SNAPSHOT_FILE);
            snapshot_create = 1;
        }
        // * Check for internal snapshot trigger
        if (snapshot_create == 1) {
            log_info("Creating a snapshot of the current data stream");
            snapshot_create = 0;
            take_snapshot();
        }
        // * Check for external video record trigger
        if (access(CREATE_VIDEO_FILE, F_OK ) != -1 ) {
            unlink(CREATE_VIDEO_FILE);
            video_create = 1;
        }
        // * Check for internal video record trigger
        if (video_create == 1) {
            video_create = 0;
            if (VideoRecorder.recording != 1) {
                log_info("Creating a video of the next %d seconds of the data stream", RECORDING_DURATION);
                if (start_recording() < 0)
                    log_error("Failed to start recording in media thread");
            }
        }
        // * Check if recordings duration is over 30 seconds
        if (VideoRecorder.recording == 1) {
            gettimeofday(&now, NULL);
            if (now.tv_sec - VideoRecorder.record_start.tv_sec > (long)RECORDING_MAX_DURATION) {
                if (stop_recording() < 0)
                    log_error("Failed to stop recording in media thread");
            }
        }
        usleep(1000);
    }
    return 0;
}

static void *motion_thread(void *arg)
{
    int ch;
    int ret;
    struct timeval now;

    gm_enc_t *param;
    param = &enc_param[0][0];

    gm_multi_cap_md_t *cap_md = NULL;
    cap_md = (gm_multi_cap_md_t *) malloc(sizeof(gm_multi_cap_md_t) * 1);
    if (cap_md == NULL) {
        log_fatal("Failed to allocate capture motion info!");
        goto thread_exit;
    }
    memset((void *) cap_md, 0, (sizeof(gm_multi_cap_md_t) * 1));

    cap_md[0].bindfd = param->bindfd[0];
    cap_md[0].cap_md_info.md_buf_len = CAP_MOTION_SIZE;
    cap_md[0].cap_md_info.md_buf = (char *) malloc(CAP_MOTION_SIZE);

    if (cap_md[0].cap_md_info.md_buf == NULL) {
        log_fatal("Failed to allocate capture motion buffer!");
        goto thread_exit;
    }

    int training_detected = 0;

    while (rtspd_sysinit) {
        ret = gm_recv_multi_cap_md(cap_md, 1);
        if (ret < 0) {                  // * -1: Error, 0: Success
            log_error("Failed to retrieve motion data (gm_recv_multi_cap_md)");
            continue;
        }
        ret = motion_detection_handling(cap_md, &mdt_result[0], 1);
        if (ret < 0) {                  // * -1: Error, 0: Success
            log_fatal("Failed to handle motion data (motion_detection_handling)");
            goto thread_exit;
        }
        for (ch = 0; ch < 1; ch++) {
            if (mdt_result[ch].result == MOTION_PARSING_ERROR)
                log_error("Failed parsing motion data.");
            else if (mdt_result[ch].result == MOTION_INIT_ERROR)
                log_error("Motion init error.");
            else if (mdt_result[ch].result == MOTION_ALGO_ERROR)
                log_error("Motion algorithm failed.");
            else if (mdt_result[ch].result == MOTION_DATA_ERROR)
                log_error("Motion data retrieval failed.");
            // * Motion Training
            else if (mdt_result[ch].result == MOTION_IS_TRAINING) {
                if (training_detected == 0) {
                    training_detected = 1;
                    log_info("Motion detection training running.");
                }
            }

            // * Motion ON
            else if (mdt_result[ch].result == MOTION_DETECTED) {
                if (motion_detected == 0) {
                    gettimeofday(&last_motion, NULL);
                    motion_detected = 1;
                    if (cliArgs.snapshot == 1)
                        snapshot_create = 1;
                    if (cliArgs.record == 1)
                        video_create = 1;
                    log_info("Motion ON - executing motion on script");
                    system(MOTION_ON_SCRIPT);
                }
            }

            // * Motion OFF
            else if (mdt_result[ch].result == NO_MOTION) {
                if (motion_detected == 1) {
                    motion_detected = 0;
                    // * Turn recording off after 20 seconds
                    if (VideoRecorder.recording == 1) {
                        gettimeofday(&now, NULL);
                        if (now.tv_sec - last_motion.tv_sec >= (long)RECORDING_DURATION) {
                            if (stop_recording() < 0)
                                log_error("Failed to stop recording in motion thread");
                        }
                    }
                    log_info("Motion OFF - executing motion off script");
                    system(MOTION_OFF_SCRIPT);
                }
            }

            else {
                log_error("Undefined Motion Event");
            }
        }
        usleep(200000);   // * Use a two second period to detect motion
    }

thread_exit:

    if (cap_md) {
        for (ch = 0 ; ch < 1; ch++) {
            if (cap_md[ch].cap_md_info.md_buf)
                free(cap_md[ch].cap_md_info.md_buf);
        }
    }
    if (cap_md)
        free(cap_md);

    motion_detection_end();
    pthread_exit(NULL);
    motion_thread_id = (pthread_t)NULL;
    return 0;
}

void *enqueue_thread(void *ptr)
{
    while (rtspd_sysinit) {
        if (rtspd_set_event)
            bs_new_event();
        if (set_poll_event() < 0) {
            sleep(1);
            continue;
        }
        usleep(1000);
    }
    env_release_resources();
    pthread_exit(NULL);
    return NULL;
}

void gm_update_bs_info(void)
{
    int cap_ch,cap_path,rec_track;
    int ch=0;
    avbs_t *avbs;
    gm_enc_t *param;

    for (cap_ch = 0; cap_ch < CAP_CH_NUM; cap_ch++) {
        for (cap_path = 0; cap_path < CAP_PATH_NUM; cap_path++) {
            param = &enc_param[cap_ch][cap_path];
            for (rec_track = 0; rec_track < ENC_TRACK_NUM; rec_track++) {
                if (param->bindfd[rec_track]) {
                    avbs = &enc[cap_ch].bs[ch];
                    avbs->video.enc_type = param->enc[rec_track].enc_type;
                    ch++;
                }
            }
        }
    }
}

int env_init(void)
{
    int ret = 0;
    int ch_num, sub_num;
    av_t *e;
    avbs_t *b;
    priv_avbs_t *pb;

    memset(enc,0,sizeof(enc));

    // * Initial private data
    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        e = &enc[ch_num];
        if (pthread_mutex_init(&e->ubs_mutex, NULL)) {
            log_error("env_init: mutex init failed");
            exit(-1);
        }
        for (sub_num = 0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            b                 = &e->bs[sub_num];
            b->opt_type       = RTSP_LIVE_STREAMING;
            b->video.enc_type = ENC_TYPE_H264;
            b->event          = NONE_BS_EVENT;
            b->enabled        = DVR_ENC_EBST_DISABLE;
            b->video.enabled  = DVR_ENC_EBST_DISABLE;

            pb                = &e->priv_bs[sub_num];
            pb->video.qno     = -1;
            pb->video.offs    = 0;
            pb->video.len     = 0;
            pb->sr            = -1;
            if (pthread_mutex_init(&pb->video.priv_vbs_mutex, NULL)) {
                log_error("env_enc_init: priv_vbs mutex init failed");
                exit(-1);
            }
        }
    }
    // * Update bs info from decoder
    gm_update_bs_info();

    srand((unsigned int)time(NULL));						
    if ((ret = stream_server_init(ipptr, (int) sys_port, 0, 1200, 256, SR_MAX, VQ_MAX, VQ_LEN, AQ_MAX, AQ_LEN, frm_cb, cmd_cb)) < 0)
								//socket, 	port, 		QoS, MTU, clients
        log_error("stream_server_init, ret %d", ret);
    if ((ret = stream_server_start()) < 0)
        log_error("stream_server_start, ret %d", ret);
    return ret;
}

static unsigned chipid;
void gm_enc_init(int cap_ch, int cap_path, int rec_track, int enc_type, int mode, int framerate, int bitrate, int width, int height)
{
    gm_enc_t *param;
    DECLARE_ATTR(cap_attr, gm_cap_attr_t);
    DECLARE_ATTR(h264e_attr, gm_h264e_attr_t);
    DECLARE_ATTR(mpeg4e_attr, gm_mpeg4e_attr_t);
    DECLARE_ATTR(dnr_attr, gm_3dnr_attr_t);
    DECLARE_ATTR(mjpege_attr, gm_mjpege_attr_t);

    param = &enc_param[cap_ch][cap_path];

    if (param->cap.obj == NULL) {
        param->cap.obj = gm_new_obj(GM_CAP_OBJECT);            // * New capture object
        cap_attr.cap_vch = cap_ch;

        // * GM813x capture path 0(liveview), 1(substream), 2(substream), 3(mainstream)
        cap_attr.path = cap_path;
        cap_attr.enable_mv_data = 1;
        gm_set_attr(param->cap.obj, &cap_attr);                // * Set capture attribute

		//enable 3dnr if resolution > capture dim / 2 
        if ((width >= (gm_system.cap[0].dim.width / 2)) &&
            (height >= (gm_system.cap[0].dim.height / 2))) {
            dnr_attr.enabled = 1;
            gm_set_attr(param->cap.obj, &dnr_attr);
        }

        /* Apply capture flip if configured */
        if (cliArgs.h_flip || cliArgs.v_flip) {
            gm_cap_flip_t flip_attr;
            memset(&flip_attr, 0, sizeof(flip_attr));
            flip_attr.h_flip_enabled = cliArgs.h_flip;
            flip_attr.v_flip_enabled = cliArgs.v_flip;
            gm_set_cap_flip(cap_ch, &flip_attr);
            log_info("Capture flip: h=%d v=%d", cliArgs.h_flip, cliArgs.v_flip);
        }

        /* Apply capture rotation if configured */
        if (cliArgs.rotation != 0) {
            DECLARE_ATTR(rotation_attr, gm_rotation_attr_t);
            rotation_attr.enabled = 1;
            rotation_attr.clockwise = cliArgs.rotation;
            gm_set_attr(param->cap.obj, &rotation_attr);
            log_info("Capture rotation: %d degrees", cliArgs.rotation);
        }

        memcpy(&param->cap.cap_attr, &cap_attr, sizeof(gm_cap_attr_t));
        memcpy(&param->cap.dnr_attr, &dnr_attr, sizeof(gm_3dnr_attr_t));
    }
	
    param->enc[rec_track].obj = gm_new_obj(GM_ENCODER_OBJECT); // * New encoder object
    param->enc[rec_track].enc_type = enc_type;
    switch (enc_type) {
        case ENC_TYPE_H264:
            h264e_attr.dim.width             = width;
            h264e_attr.dim.height            = height;
            h264e_attr.frame_info.framerate  = framerate;
            h264e_attr.ratectl.mode          = mode;
            h264e_attr.ratectl.gop           = cliArgs.gop;	   // * I frame per second
            h264e_attr.ratectl.bitrate       = bitrate;
            h264e_attr.ratectl.bitrate_max   = 16384;          // * Max bitrate ceiling (VBR upper bound)
            h264e_attr.b_frame_num           = 0;              // * B-frames per GOP (H.264 high profile)
            h264e_attr.enable_mv_data        = 0;              // * Disable H.264 motion data output
            h264e_attr.ratectl.init_quant    = 25;
            h264e_attr.ratectl.min_quant     = 20;
            h264e_attr.ratectl.max_quant     = 51;

            /* Apply H264 profile/level/config/coding if configured */
            if (cliArgs.h264_profile)
                h264e_attr.profile_setting.profile = (gm_h264e_profile_t) cliArgs.h264_profile;
            if (cliArgs.h264_level)
                h264e_attr.profile_setting.level = (gm_h264e_level_t) cliArgs.h264_level;
            if (cliArgs.h264_config)
                h264e_attr.profile_setting.config = (gm_h264e_config_t) cliArgs.h264_config;
            if (cliArgs.h264_coding)
                h264e_attr.profile_setting.coding = (gm_h264e_coding_t) cliArgs.h264_coding;

            /* Apply fractional framerate if configured */
            if (cliArgs.fps_ratio_num > 0 && cliArgs.fps_ratio_den > 0) {
                h264e_attr.frame_info.fps_ratio.numerator   = cliArgs.fps_ratio_num;
                h264e_attr.frame_info.fps_ratio.denominator = cliArgs.fps_ratio_den;
            }

            gm_set_attr(param->enc[rec_track].obj, &h264e_attr);

            /* H264 advanced */
			DECLARE_ATTR(h264_adv, gm_h264_advanced_attr_t);
			h264_adv.multi_slice = 4;
			h264_adv.field_coding = 0;
			h264_adv.gray_scale = 0;
			gm_set_attr(param->enc[rec_track].obj, &h264_adv);

            /* Always apply VUI color info and SAR */
            {
                DECLARE_ATTR(vui_attr, gm_h264_vui_attr_t);
                vui_attr.param_info.param.video_format = 5;   /* component */
                vui_attr.param_info.param.colour_primaries = 1;   /* BT.709 */
                vui_attr.param_info.param.transfer_characteristics = 1;   /* BT.709 */
                vui_attr.param_info.param.matrix_coefficient = (char) cliArgs.vui_colorspace;
                vui_attr.param_info.param.full_range = cliArgs.vui_full_range & 1;
                vui_attr.param_info.param.timing_info_present_flag = 0;
                vui_attr.sar_info.sar.sar_width = cliArgs.sar_width;
                vui_attr.sar_info.sar.sar_height = cliArgs.sar_height;
                gm_set_attr(param->enc[rec_track].obj, &vui_attr);
            }

            /* Apply ROI encoding if configured */
            if (cliArgs.roi_enabled) {
                DECLARE_ATTR(roi_attr, gm_enc_roi_attr_t);
                roi_attr.enabled = 1;
                roi_attr.rect.x = cliArgs.roi_x;
                roi_attr.rect.y = cliArgs.roi_y;
                roi_attr.rect.width = cliArgs.roi_w;
                roi_attr.rect.height = cliArgs.roi_h;
                gm_set_attr(param->enc[rec_track].obj, &roi_attr);
            }

            /* Apply ROI QP 8-region mode if configured */
            if (cliArgs.roiqp_enabled) {
                DECLARE_ATTR(roiqp_attr, gm_h264_roiqp_attr_t);
                roiqp_attr.enabled = 1;
                /* Default: center 50% region gets lower QP (better quality) */
                memset(roiqp_attr.rect, 0, sizeof(roiqp_attr.rect));
                roiqp_attr.rect[0].x = width / 4;
                roiqp_attr.rect[0].y = height / 4;
                roiqp_attr.rect[0].width = width / 2;
                roiqp_attr.rect[0].height = height / 2;
                gm_set_attr(param->enc[rec_track].obj, &roiqp_attr);
            }

            memcpy(&param->enc[rec_track].codec.h264e_attr, &h264e_attr, sizeof(gm_h264e_attr_t));
            break;
        case ENC_TYPE_MPEG4:
            mpeg4e_attr.dim.width            = width;
            mpeg4e_attr.dim.height           = height;
            mpeg4e_attr.frame_info.framerate = framerate;
            mpeg4e_attr.ratectl.mode         = mode;
            mpeg4e_attr.ratectl.gop          = 60;
            mpeg4e_attr.ratectl.bitrate      = bitrate;
            mpeg4e_attr.ratectl.bitrate_max  = bitrate;
            gm_set_attr(param->enc[rec_track].obj, &mpeg4e_attr);
            memcpy(&param->enc[rec_track].codec.mpeg4e_attr, &mpeg4e_attr, sizeof(gm_mpeg4e_attr_t));
            break;
        case ENC_TYPE_MJPEG:
            mjpege_attr.dim.width            = width;
            mjpege_attr.dim.height           = height;
            mjpege_attr.frame_info.framerate = framerate;
            mjpege_attr.quality              = 30;
            gm_set_attr(param->enc[rec_track].obj, &mjpege_attr);
            memcpy(&param->enc[rec_track].codec.mjpege_attr, &mjpege_attr, sizeof(gm_mjpege_attr_t));
            break;
        default:
            log_error("Encoder type not supported: %s", rtsp_enc_type_str[enc_type]);
            break;
    }
    // * Bind channel recording
    param->bindfd[rec_track] = gm_bind(enc_groupfd, param->cap.obj, param->enc[rec_track].obj);
    if (cliArgs.osd) {
    	rtspd_enable_osd_font(param->cap.obj, cliArgs.osd_text[0] != '\0' ? cliArgs.osd_text : "chuangmi");
    }
    // * Set motion detection
    if (cliArgs.motion == 1) {
        motion_detection_init();				// * Enable motion detection
        int ret = 0;
        ret = set_interesting_area(rec_track);	// * Set area for motion detection
        if (ret != 0) {
            log_error("Failed running set_interesting_area!");
        }
    }

    // * Enable Scaler Encoder if downscaling is required (only for H264)
    if (enc_type == ENC_TYPE_H264 && (width < 1280 || height < 720)) {
        h264e_attr.ratectl.bitrate      = bitrate;
        h264e_attr.frame_info.framerate = framerate;
        h264e_attr.dim.width            = width;
        h264e_attr.dim.height           = height;
        gm_set_attr(sub_enc_object, &h264e_attr);
        sub_bindfd                      = gm_bind(enc_groupfd, param->cap.obj, sub_enc_object);
    }
    rtspd_avail_ch++;
}

int gm_get_chipinfo(void)
{
    FILE *fp;
    char buffer[256];
    int i;
    int chipid;
    char *match;

    fp = fopen("/proc/pmu/chipver","r");
    i = fread(buffer,1,sizeof(buffer),fp);
    fclose(fp);
    if (i == 0)
        return 0;
    buffer[i] = '\0';
    match = strstr(buffer, "81");
    if (match == NULL)
        return 0;
    sscanf(match,"%X",&chipid);
    return chipid;
}

static int gm_get_max_bandwidth(char *list)
{
    int bandwidth=0;
    int i;
    int ch;
    int tmp;
    char token[] = "\n \t";
    int sensor_res[] = {800,500,400,300,200,130,100,34,30,7};
    char *str;
    str = strtok(list,token);

    for (i=0;i<5;i++) {
        if (str == NULL)
            break;
        sscanf(str,"%02d",&ch);
        str += 3;
        sscanf(str,"%03d",&tmp);
        str = strtok(NULL,token);
        if (ch == 0 || tmp == 0)
             continue;
        tmp = tmp * sensor_res[i];
        tmp = tmp / ch;
        if (i <= 3) {
            bandwidth = 8*30;
            break;
        }
        if (bandwidth <= (tmp /100))
            bandwidth = tmp/100;
    }
    return bandwidth;
}

int gm_get_bandwidth_info(void)
{
    FILE *fp;
    char buffer[2048];
    int i;
    char *match;

    fp = fopen("/proc/videograph/vpd/spec_info","r");
    i = fread(buffer,1,sizeof(buffer),fp);
    fclose(fp);
    if (i == 0)
        buffer[i] = '\0';
    match = strstr(buffer,"[ENC CAPTURE]");
    if (match == NULL)
        return 0;
    match = strstr(match, "CH_0");
    if (match == NULL)
        return 0;
    match += 4;
    sscanf(match,"%X",&i);
    if (i == 0)
        return 0;
	
    if (gm_get_max_bandwidth((match+1)) > 62)
        return 1;
    else
        return 0;
}

static void audio_init() {
    DECLARE_ATTR(audio_grab_attr, gm_audio_grab_attr_t);
    DECLARE_ATTR(audio_encode_attr, gm_audio_enc_attr_t);

    if (!cliArgs.audio_enabled) {
        log_info("Audio disabled");
        audio_bindfd = NULL;
        return;
    }

    enc_audio_groupfd = gm_new_groupfd();

    audio_grab_object = gm_new_obj(GM_AUDIO_GRAB_OBJECT);
    audio_encode_object = gm_new_obj(GM_AUDIO_ENCODER_OBJECT);

    audio_grab_attr.vch = 0;
    audio_grab_attr.sample_rate = cliArgs.audio_sample_rate;
    audio_grab_attr.sample_size = cliArgs.audio_sample_size;
    audio_grab_attr.channel_type = (gm_audio_channel_type_t) cliArgs.audio_channel_type;
    gm_set_attr(audio_grab_object, &audio_grab_attr);

    audio_encode_attr.encode_type = (gm_audio_encode_type_t) cliArgs.audio_encode_type;
    audio_encode_attr.bitrate = cliArgs.audio_bitrate;
    audio_encode_attr.frame_samples = cliArgs.audio_frame_samples;
    gm_set_attr(audio_encode_object, &audio_encode_attr);

    audio_bindfd = gm_bind(enc_audio_groupfd, audio_grab_object, audio_encode_object);

    log_info("Audio: type=%s rate=%dHz size=%dbit ch=%s bitrate=%d frame_samples=%d",
             audio_encode_type_name(cliArgs.audio_encode_type),
             cliArgs.audio_sample_rate,
             cliArgs.audio_sample_size,
             cliArgs.audio_channel_type == GM_STEREO ? "stereo" : "mono",
             cliArgs.audio_bitrate,
             cliArgs.audio_frame_samples);
    if (gm_apply(enc_audio_groupfd) < 0) {
        printf("Error! gm_apply fail");
        exit(-1);
    }
}

void gm_graph_init(void)
{
    int cap_fps;
    int cap_h;
    int cap_w;
    int cap_resolution;
    int cap_bandwidth;

    gm_init();
    gm_get_sysinfo(&gm_system);

    if (cliArgs.osd)
        rtspd_set_osd_palette();
    if (cliArgs.framerate > 0)
        poll_wait_time = 1000000 / (cliArgs.framerate + 2);
    else
        poll_wait_time = 15000;
    cap_fps        = cliArgs.framerate;
    cap_h          = cliArgs.height;
    cap_w          = cliArgs.width;
    cap_bandwidth  = cap_fps * cap_h * cap_w;
    cap_resolution = cap_h * cap_w;

    memset(enc_param, 0, sizeof(enc_param));
    enc_groupfd = gm_new_groupfd();
    chipid = gm_get_chipinfo();
    chipid = (chipid >> 16) & 0x0000ffff;

    rtspd_avail_ch = 0;
    gm_enc_init(0, 0, 0, cliArgs.encoderType, cliArgs.bitrateMode, cliArgs.framerate, cliArgs.bitrate, cliArgs.width, cliArgs.height);
    gm_apply(enc_groupfd); // * Activate settings
	audio_init();		   // * Activate Audio
}

void gm_graph_release(void)
{
    gm_enc_t *param;
    int cap_ch, cap_path, rec_track;

    for (cap_ch = 0; cap_ch < CAP_CH_NUM; cap_ch++) {
        for (cap_path = 0; cap_path < CAP_PATH_NUM; cap_path++) {
            param = &enc_param[cap_ch][cap_path];
            for (rec_track = 0; rec_track < ENC_TRACK_NUM; rec_track++) {
                if (param->bindfd[rec_track])
                    gm_unbind(param->bindfd[rec_track]);
            }
        }
    }
    if (audio_bindfd)
        gm_unbind(audio_bindfd);
    gm_apply(enc_groupfd);
    if (audio_bindfd)
        gm_apply(enc_audio_groupfd);
	
    for (cap_ch = 0; cap_ch < CAP_CH_NUM; cap_ch++) {
        for (cap_path = 0; cap_path < CAP_PATH_NUM; cap_path++) {
            param = &enc_param[cap_ch][cap_path];
            for (rec_track = 0; rec_track < ENC_TRACK_NUM; rec_track++) {
                if (param->enc[rec_track].obj)
                    gm_delete_obj(param->enc[rec_track].obj);
            }
            if (param->cap.obj)
                gm_delete_obj(param->cap.obj);
        }
    }
    if (audio_grab_object)
        gm_delete_obj(audio_grab_object);
    if (audio_encode_object)
        gm_delete_obj(audio_encode_object);
    gm_delete_groupfd(enc_groupfd);
    if (audio_bindfd)
        gm_delete_groupfd(enc_audio_groupfd);
    gm_release();
}

/* Apply the current ePTZ crop window to the encoder (digital zoom / pan / tilt).
 * The crop rectangle is taken from the capture source and scaled up by the
 * encoder to the configured output resolution. */
static int rtspd_apply_eptz(float factor, float pan, float tilt)
{
    DECLARE_ATTR(eptz_attr, gm_enc_eptz_attr_t);
    gm_enc_t *param;
    void *bindfd;
    int src_w, src_h;
    int crop_w, crop_h, crop_x, crop_y;
    static int eptz_unsupported = 0;

    /* GM8136S driver does not support the EPTZ attribute; detect it once and
     * keep the digital zoom feature silently disabled instead of spamming
     * "GM_ENC_EPTZ_ATTR not support!" on every update. */
    if (eptz_unsupported)
        return 0;

    param = &enc_param[0][0];
    bindfd = param->bindfd[0];
    if (bindfd == NULL)
        return -1;

    src_w = gm_system.cap[0].dim.width;
    src_h = gm_system.cap[0].dim.height;
    if (src_w <= 0 || src_h <= 0)
        return -1;

    if (factor < 1.0f)
        factor = 1.0f;
    if (factor > MAX_ZOOM_FACTOR)
        factor = MAX_ZOOM_FACTOR;
    if (pan < 0.0f)  pan  = 0.0f;
    if (pan > 1.0f)  pan  = 1.0f;
    if (tilt < 0.0f) tilt = 0.0f;
    if (tilt > 1.0f) tilt = 1.0f;

    if (factor <= 1.001f) {
        /* No zoom: disable ePTZ and encode the full frame */
        eptz_attr.enabled = 0;
        eptz_attr.src_dim.width  = src_w;
        eptz_attr.src_dim.height = src_h;
        eptz_attr.src_crop_rect.x = 0;
        eptz_attr.src_crop_rect.y = 0;
        eptz_attr.src_crop_rect.width  = src_w;
        eptz_attr.src_crop_rect.height = src_h;
    } else {
        /* Crop a window of src_w/factor x src_h/factor, scaled to output */
        crop_w = (int)((float)src_w / factor + 0.5f);
        crop_h = (int)((float)src_h / factor + 0.5f);
        /* Keep crop dims aligned to a macroblock (16) for the hardware encoder */
        crop_w &= ~15;
        crop_h &= ~15;
        if (crop_w < 16)
            crop_w = 16;
        if (crop_h < 16)
            crop_h = 16;
        crop_x = (int)((float)(src_w - crop_w) * pan + 0.5f);
        crop_y = (int)((float)(src_h - crop_h) * tilt + 0.5f);
        crop_x &= ~1;
        crop_y &= ~1;
        if (crop_x + crop_w > src_w)
            crop_x = src_w - crop_w;
        if (crop_y + crop_h > src_h)
            crop_y = src_h - crop_h;

        eptz_attr.enabled = 1;
        eptz_attr.src_dim.width  = src_w;
        eptz_attr.src_dim.height = src_h;
        eptz_attr.src_crop_rect.x = (unsigned int)crop_x;
        eptz_attr.src_crop_rect.y = (unsigned int)crop_y;
        eptz_attr.src_crop_rect.width  = (unsigned int)crop_w;
        eptz_attr.src_crop_rect.height = (unsigned int)crop_h;
    }

    if (gm_set_attr(param->enc[0].obj, &eptz_attr) < 0) {
        eptz_unsupported = 1;
        log_info("ePTZ not supported by hardware, digital zoom disabled");
        return 0;
    }
    if (gm_apply_attr(bindfd, &eptz_attr) < 0) {
        eptz_unsupported = 1;
        log_info("ePTZ not supported by hardware, digital zoom disabled");
        if (gm_apply(enc_groupfd) < 0) {
            log_error("rtspd_apply_eptz: gm_apply failed");
            return -1;
        }
    }
    return 0;
}

/* Zoom thread: poll the zoom control file written by the ONVIF server and
 * smoothly move the ePTZ crop window to the requested zoom/pan/tilt. */
static void *rtspd_zoom_thread(void *arg)
{
    float new_zoom, new_pan, new_tilt;
    float last_zoom = -1.0f, last_pan = -1.0f, last_tilt = -1.0f;

    (void)arg;
    while (rtspd_sysinit) {
        FILE *f = fopen(RTSPD_ZOOM_FILE, "r");
        if (f) {
            char buf[64];
            if (fgets(buf, sizeof(buf), f) != NULL) {
                new_zoom = 0.0f;
                new_pan = 0.5f;
                new_tilt = 0.5f;
                if (sscanf(buf, "%f %f %f", &new_zoom, &new_pan, &new_tilt) >= 1) {
                    if (new_zoom < 0.0f) new_zoom = 0.0f;
                    if (new_zoom > 1.0f) new_zoom = 1.0f;
                    if (new_pan < 0.0f)  new_pan  = 0.0f;
                    if (new_pan > 1.0f)  new_pan  = 1.0f;
                    if (new_tilt < 0.0f) new_tilt = 0.0f;
                    if (new_tilt > 1.0f) new_tilt = 1.0f;
                    pthread_mutex_lock(&zoom_mutex);
                    zoom_target   = 1.0f + new_zoom * (MAX_ZOOM_FACTOR - 1.0f);
                    zoom_tgt_pan  = new_pan;
                    zoom_tgt_tilt = new_tilt;
                    pthread_mutex_unlock(&zoom_mutex);
                }
            }
            fclose(f);
        }

        /* Smoothly step zoom/pan/tilt toward the target, then apply */
        pthread_mutex_lock(&zoom_mutex);
        zoom_factor += (zoom_target - zoom_factor) * ZOOM_SMOOTH_STEP;
        zoom_pan    += (zoom_tgt_pan - zoom_pan)    * ZOOM_SMOOTH_STEP;
        zoom_tilt   += (zoom_tgt_tilt - zoom_tilt)  * ZOOM_SMOOTH_STEP;
        new_zoom  = zoom_factor;
        new_pan   = zoom_pan;
        new_tilt  = zoom_tilt;
        pthread_mutex_unlock(&zoom_mutex);

        /* Skip redundant updates */
        if (fabsf(new_zoom - last_zoom) < 0.001f &&
            fabsf(new_pan  - last_pan)  < 0.001f &&
            fabsf(new_tilt - last_tilt) < 0.001f) {
            usleep(20000);
            continue;
        }

        if (rtspd_apply_eptz(new_zoom, new_pan, new_tilt) == 0) {
            last_zoom = new_zoom;
            last_pan  = new_pan;
            last_tilt = new_tilt;
        }
        usleep(20000);   // * 50 Hz zoom update
    }
    return NULL;
}

/* Ctrl thread: polls /tmp/rtspd.ctrl for commands from codec_ctrl.
 * keyframe is applied live; bitrate/mode/fps/gop are staged in
 * /tmp/rtspd_pending_args and applied via a self-restart, because the
 * GM driver ignores gm_set_attr() changes after the encoder is running. */
#define RTSPD_CTRL_FILE    "/tmp/rtspd.ctrl"
#define RTSPD_ARGS_FILE    "/tmp/rtspd_pending_args"
#define RTSPD_ARGS_FILE_TMP "/tmp/rtspd_pending_args.tmp"
#define RTSPD_RESTART_FILE "/tmp/rtspd_restart_oldpid"
static pthread_t ctrl_thread_id = 0;

static int  saved_argc = 0;
static char *saved_argv[64];

/* Merge key=val into RTSPD_ARGS_FILE, preserving other keys. */
static void write_pending_arg(const char *key, int val)
{
    char line[32], buf[128];
    FILE *af, *tmp;
    int replaced = 0;

    snprintf(line, sizeof(line), "%s=%d\n", key, val);
    tmp = fopen(RTSPD_ARGS_FILE_TMP, "w");
    if (!tmp)
        return;
    af = fopen(RTSPD_ARGS_FILE, "r");
    if (af) {
        while (fgets(buf, sizeof(buf), af)) {
            size_t klen = strlen(key);
            if (strncmp(buf, key, klen) == 0 && buf[klen] == '=') {
                fputs(line, tmp);
                replaced = 1;
            } else {
                fputs(buf, tmp);
            }
        }
        fclose(af);
    }
    if (!replaced)
        fputs(line, tmp);
    fclose(tmp);
    rename(RTSPD_ARGS_FILE_TMP, RTSPD_ARGS_FILE);
}

/* Fork a detached child that re-execs the same binary with the same argv.
 * The child must only use async-signal-safe calls (no fopen/opendir/malloc)
 * because other threads may hold internal locks at fork time, which would
 * deadlock the child. The parent then kills itself so the encoder is torn
 * down cleanly; the new process picks up the restart marker and waits for
 * this one to exit before re-initializing. */
static void rtspd_reboot(void)
{
    pid_t parent = getpid();
    FILE *mf = fopen(RTSPD_RESTART_FILE, "w");
    if (mf) {
        fprintf(mf, "%d", (int)parent);
        fclose(mf);
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("Ctrl: fork failed, cannot restart (%s)", strerror(errno));
        remove(RTSPD_RESTART_FILE);
        return;
    }
    if (pid > 0) {
        log_info("Ctrl: restarting rtspd...");
        kill(parent, SIGTERM);
        return;
    }

    /* child: async-signal-safe only */
    setsid();
    {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
        char exe[256];
        int len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len > 0) {
            exe[len] = '\0';
            execv(exe, saved_argv);
        }
    }
    _exit(127);
}

/* True if /proc/<pid>/cmdline mentions rtspd (guards against PID reuse
 * making us wait on an unrelated process). */
static int pid_is_rtspd(int pid)
{
    char path[64], buf[64];
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strstr(buf, "rtspd") != NULL;
}

/* Called at startup: if a restart marker exists we were relaunched by
 * rtspd_reboot(). Wait for the old process to exit (so the encoder device
 * is released), then drop the inherited device fds before re-initializing. */
static void restart_handoff(void)
{
    FILE *f = fopen(RTSPD_RESTART_FILE, "r");
    int oldpid = 0, valid = 0;

    if (!f)
        return;
    if (fscanf(f, "%d", &oldpid) != 1)
        oldpid = 0;
    fclose(f);

    if (oldpid > 0 && pid_is_rtspd(oldpid)) {
        int i;
        valid = 1;
        log_info("Restart: inherited from old rtspd (pid %d), waiting for it to exit", oldpid);
        for (i = 0; i < 100; i++) {
            usleep(100000);
            if (kill(oldpid, 0) < 0 && errno == ESRCH)
                break;
        }
    } else if (oldpid > 0) {
        log_info("Restart: ignoring stale marker (pid %d not rtspd)", oldpid);
    }

    if (valid) {
        int fds[128], n = 0, i;
        int logfd = logfile ? fileno(logfile) : -1;
        DIR *d = opendir("/proc/self/fd");
        if (d) {
            struct dirent *de;
            while (n < 128 && (de = readdir(d)) != NULL) {
                int fdnum = atoi(de->d_name);
                if (fdnum > 2 && fdnum != logfd)
                    fds[n++] = fdnum;
            }
            closedir(d);
        }
        for (i = 0; i < n; i++)
            close(fds[i]);
        log_info("Restart: handoff complete");
    }

    remove(RTSPD_RESTART_FILE);
}

/* Apply key=value overrides stored by the ctrl thread (from codec_ctrl)
 * into cliArgs. Called at startup so changes survive a restart. The file is
 * NOT deleted: it is the persistent desired config, so consecutive changes
 * (e.g. bitrate then mode) stack instead of losing earlier values. */
static void apply_pending_args(void)
{
    FILE *f = fopen(RTSPD_ARGS_FILE, "r");
    int bitrate = -1, mode = -1, fps = -1, gop = -1;
    char buf[128];

    if (!f)
        return;
    while (fgets(buf, sizeof(buf), f)) {
        if (sscanf(buf, "bitrate=%d", &bitrate) == 1) {}
        else if (sscanf(buf, "mode=%d", &mode) == 1) {}
        else if (sscanf(buf, "fps=%d", &fps) == 1) {}
        else if (sscanf(buf, "gop=%d", &gop) == 1) {}
    }
    fclose(f);

    if (bitrate > 0 && bitrate <= 16384) { cliArgs.bitrate = bitrate; log_info("Pending args: bitrate=%d", bitrate); }
    if (mode >= 1 && mode <= 4)           { cliArgs.bitrateMode = mode; log_info("Pending args: mode=%d", mode); }
    if (fps > 0 && fps <= 30)             { cliArgs.framerate = fps;   log_info("Pending args: fps=%d", fps); }
    if (gop > 0 && gop <= 120)            { cliArgs.gop = gop;         log_info("Pending args: gop=%d", gop); }
}

static void *rtspd_ctrl_thread(void *arg)
{
    char buf[128];
    int need_reboot = 0;
    (void)arg;

    while (rtspd_sysinit) {
        FILE *f = fopen(RTSPD_CTRL_FILE, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                buf[strcspn(buf, "\r\n")] = '\0';
                need_reboot = 0;

                if (strcmp(buf, "keyframe") == 0) {
                    if (bindfd) {
                        int ret = gm_request_keyframe(bindfd);
                        log_info("Ctrl: keyframe requested (ret=%d)", ret);
                    }
                }
                else if (strncmp(buf, "bitrate ", 8) == 0) {
                    int val = atoi(buf + 8);
                    if (val > 0 && val <= 16384) {
                        write_pending_arg("bitrate", val);
                        log_info("Ctrl: bitrate=%d pending restart", val);
                        need_reboot = 1;
                    }
                }
                else if (strncmp(buf, "mode ", 5) == 0) {
                    int val = atoi(buf + 5);
                    if (val >= 1 && val <= 4) {
                        write_pending_arg("mode", val);
                        log_info("Ctrl: mode=%d pending restart", val);
                        need_reboot = 1;
                    }
                }
                else if (strncmp(buf, "fps ", 4) == 0) {
                    int val = atoi(buf + 4);
                    if (val > 0 && val <= 30) {
                        write_pending_arg("fps", val);
                        log_info("Ctrl: fps=%d pending restart", val);
                        need_reboot = 1;
                    }
                }
                else if (strncmp(buf, "gop ", 4) == 0) {
                    int val = atoi(buf + 4);
                    if (val > 0 && val <= 120) {
                        write_pending_arg("gop", val);
                        log_info("Ctrl: gop=%d pending restart", val);
                        need_reboot = 1;
                    }
                }
            }
            fclose(f);
            remove(RTSPD_CTRL_FILE);
        }
        if (need_reboot) {
            log_info("Ctrl: restarting rtspd to apply changes");
            rtspd_reboot();
            usleep(200000);
        }
        usleep(500000);   // * poll every 500ms
    }
    return NULL;
}

void *encode_thread(void *ptr)
{
    int i, j, ch = 0, ret, cap_ch, cap_path, rec_track, rcv_nr, w, h;
    int first_play[CAP_CH_NUM][RTSP_NUM_PER_CAP];
    priv_avbs_t *pb;
    avbs_t *avbs;
    gm_enc_multi_bitstream_t bs[CAP_CH_NUM][RTSP_NUM_PER_CAP];
    gm_pollfd_t poll_fds[CAP_CH_NUM][RTSP_NUM_PER_CAP];
    gm_enc_t *param;
    static struct timeval prev;
    struct timeval cur, tout;
    static int timeval_init = 0;
    int diff;

    memset(poll_fds, 0, sizeof(poll_fds));
    memset(first_play, -1, sizeof(first_play));

    for (cap_ch = 0; cap_ch < CAP_CH_NUM; cap_ch++) {
        for (cap_path = 0; cap_path < CAP_PATH_NUM; cap_path++) {
            param = &enc_param[cap_ch][cap_path];
            for (rec_track = 0; rec_track < ENC_TRACK_NUM; rec_track++) {
                if (param->bindfd[rec_track]) {
                    poll_fds[cap_ch][ch].bindfd = param->bindfd[rec_track];
                    poll_fds[cap_ch][ch].event  = GM_POLL_READ;

                    avbs = &enc[cap_ch].bs[ch];
                    get_enc_res(&param->enc[rec_track], NULL, &w, &h);
                    pb = &enc[cap_ch].priv_bs[ch];
                    pb->video.bs_buf_len = w * h * 3 / 2;
                    pb->video.bs_buf     = malloc(pb->video.bs_buf_len);
                    pb->video.cap_ch     = cap_ch;
                    pb->video.cap_path   = cap_path;
                    pb->video.rec_track  = rec_track;
                    ch++;
                }
            }
        }
    }

    int poll_timeout_count = 0;
    while(1) {
        if (rtspd_sysinit == 0)
            break;
        if (rtspd_set_event) {
            usleep(2000);
            continue;
        }
        if (set_poll_event() < 0) {
            usleep(2000);
            continue;
        }
        gettimeofday(&cur, NULL);

        if (timeval_init == 0) {
            timeval_init = 1;
            tout.tv_sec  = 0;
            tout.tv_usec = poll_wait_time;
        } else {
            diff         = (cur.tv_usec < prev.tv_usec) ? (cur.tv_usec+1000000-prev.tv_usec) : (cur.tv_usec-prev.tv_usec);
            tout.tv_usec = (diff > poll_wait_time) ? (tout.tv_usec = 0) : (poll_wait_time - diff);
        }
        usleep(tout.tv_usec);
        gettimeofday(&prev, NULL);
        ret = 0;
        if (rtspd_sysinit == 0)
            break;
        ret = gm_poll(&poll_fds[0][0], CAP_CH_NUM * RTSP_NUM_PER_CAP, 2000);
        if (ret == GM_TIMEOUT) {
            poll_timeout_count++;
            if (poll_timeout_count >= 30) {
                log_error("GM Poll timeout x%d — forcing rtspd restart", poll_timeout_count);
                kill(getpid(), SIGTERM);
                break;
            }
            if (poll_timeout_count % 5 == 1)
                log_error("GM Poll timeout (count=%d), backing off", poll_timeout_count);
            usleep(500000);
            continue;
        }
        poll_timeout_count = 0;
        rcv_nr = 0;
        memset(bs, 0, sizeof(bs));
        for (i = 0; i < CAP_CH_NUM; i++) {
            for (j = 0; j < RTSP_NUM_PER_CAP; j++) {
                pb = &enc[i].priv_bs[j];
                if (pb->video.offs || pb->video.len)
                    continue;
                if (poll_fds[i][j].revent.event != GM_POLL_READ)
                    continue;
                if (poll_fds[i][j].revent.bs_len > pb->video.bs_buf_len) {
                    log_error("%d_%d: bindfd(%p) bitstream buffer length is not enough! (%d_bytes vs %d_bytes)", i, j, poll_fds[i][j].bindfd, poll_fds[i][j].revent.bs_len, pb->video.bs_buf_len);
                    continue;
                }
                rcv_nr++;
                bs[i][j].bindfd = poll_fds[i][j].bindfd;                
                bs[i][j].bs.bs_buf = pb->video.bs_buf; // * Set buffer point
                bs[i][j].bs.bs_buf_len = pb->video.bs_buf_len;// * Set buffer length
                bs[i][j].bs.mv_buf = 0;// * Turn receiving MV data off
                bs[i][j].bs.mv_buf_len = 0;// * Not to receive MV data
                if (pb->play == 0)
                    first_play[i][j] = -1;
            }
        }
        if (rcv_nr == 0)
            continue;
        if (rtspd_sysinit == 0)
            break;
        if ( (ret = gm_recv_multi_bitstreams(&bs[0][0], CAP_CH_NUM * RTSP_NUM_PER_CAP)) < 0 ) {
            // <= -1: fail, 0: success
            log_error("Failed to receive bitstream (gm_recv_multi_bitstreams).");
            continue;
        }

        for (i = 0; i < CAP_CH_NUM; i++) {
            for (j = 0; j < RTSP_NUM_PER_CAP; j++) {
                if (rtspd_sysinit == 0)
                    continue;
                pb = &enc[i].priv_bs[j];
                avbs = &enc[i].bs[j];
                if ((bs[i][j].retval < 0) && bs[i][j].bindfd)
                    log_error("Failed to receive bitstream.");
                else if (bs[i][j].retval == GM_SUCCESS) {
                    if (bs[i][j].bs.keyframe == 1)
                        VideoRecorder.waiting_for_keyframe = 0;
                    // * Write buffer to file in case recording is enabled
                    if (VideoRecorder.recording == 1 && VideoRecorder.fh != NULL && VideoRecorder.waiting_for_keyframe == 0) {
                        fwrite(bs[i][j].bs.bs_buf, 1, bs[i][j].bs.bs_len, VideoRecorder.fh);
                        fflush(VideoRecorder.fh);
                    }
                    if (avbs->video.enc_type != ENC_TYPE_MJPEG) {
                        if ((pb->play == 1) && (bs[i][j].bs.keyframe == 1))
                            first_play[i][j] = 1;
                    }
                    else
                        first_play[i][j] = 1;
                    if (first_play[i][j] == 1) {
                        pthread_mutex_lock(&pb->video.priv_vbs_mutex);
                        pb->video.offs  = (int) (bs[i][j].bs.bs_buf);
                        pb->video.len   = bs[i][j].bs.bs_len;
                        pb->video.tv_ms = bs[i][j].bs.timestamp;
                        pthread_mutex_unlock(&pb->video.priv_vbs_mutex);
                        // * Write buffer to the rtsp service and empty buffers
                        if (write_rtp_frame_ext(i, j, (void *)pb->video.offs, pb->video.len, bs[i][j].bs.timestamp) == 1) {
                            pb->video.offs = (int)NULL;
                            pb->video.len  = 0;
                        }
                    }
                    print_enc_average(i, j, bs[i][j].bs.bs_len, &prev);
                }
            }
        }
    }

    pthread_exit(NULL);
    encode_thread_id = (pthread_t)NULL;
    return NULL;
}

#define AU_BITSTREAM_LEN         12800
static void *audio_encode_thread(void *arg)
{
    int ret;
    int enc_type;
    int rtp_clock;
    gm_pollfd_t poll_fds;
    gm_enc_multi_bitstream_t multi_bs;
    char *bitstream_data;
    char *payload;
    int payload_len;
    priv_avbs_t *pb;
    gm_ss_entity entity;

    if (!cliArgs.audio_enabled)
        return 0;

    bitstream_data = (char *)malloc(AU_BITSTREAM_LEN + 4);
    if (bitstream_data == 0)
        return 0;
    memset(bitstream_data, 0, AU_BITSTREAM_LEN + 4);
    memset(&poll_fds, 0, sizeof(poll_fds));
    poll_fds.bindfd = audio_bindfd;
    poll_fds.event = GM_POLL_READ;
    pb = &enc[0].priv_bs[0];

    enc_type  = cliArgs.audio_encode_type;
    rtp_clock = audio_rtp_clock(enc_type, cliArgs.audio_sample_rate);

    while (rtspd_sysinit) {
        if (pb->play == 0 && VideoRecorder.recording != 1) {
            usleep(2000);
            continue;
        }
        ret = gm_poll(&poll_fds, 1, 2000);
        if (ret == GM_TIMEOUT) {
            usleep(500000);
            continue;
        }
        memset(&multi_bs, 0, sizeof(multi_bs));
        if (poll_fds.revent.event != GM_POLL_READ) {
            continue;
        }
        if (poll_fds.revent.bs_len > AU_BITSTREAM_LEN) {
            log_error("buffer length is not enough! %d, %d\n",
                    poll_fds.revent.bs_len, AU_BITSTREAM_LEN);
            continue;
        }
        multi_bs.bindfd = audio_bindfd;
        multi_bs.bs.bs_buf = bitstream_data + 4;
        multi_bs.bs.bs_buf_len = AU_BITSTREAM_LEN;
        multi_bs.bs.mv_buf = NULL;
        multi_bs.bs.mv_buf_len = 0;
        if ((ret = gm_recv_multi_bitstreams(&multi_bs, 1)) < 0) {
            log_error("audio error return value %d", ret);
            continue;
        }
        if (!multi_bs.bindfd || multi_bs.retval < 0)
            continue;

        if (enc_type == GM_AAC) {
            /* The AAC encoder outputs ADTS. The driver may deliver one or
             * more ADTS frames per receive (block_count), so walk the buffer
             * and emit one RFC 3640 MPEG4-GENERIC AU (4-byte AU header + one
             * ADTS-stripped frame) per ADTS frame. */
            char *aac_data = multi_bs.bs.bs_buf;
            int aac_data_len = multi_bs.bs.bs_len;
            int consumed = 0;
            int aac_frame_no = 0;
            static struct timeval aq_err_tval;

            while (consumed + 7 <= aac_data_len) {
                unsigned char *hdr = (unsigned char *)(aac_data + consumed);
                int adts_header, frame_len, frame_payload_len;

                if (hdr[0] != 0xff || (hdr[1] & 0xf6) != 0xf0)
                    break;                              // * Not ADTS, stop
                frame_len = ((hdr[3] & 0x03) << 11) | ((int)hdr[4] << 3) | ((hdr[5] >> 5) & 0x07);
                if (frame_len < 7 || consumed + frame_len > aac_data_len)
                    break;                              // * Incomplete tail frame

                /* ADTS header size: bit0 of byte1 is protection_absent; when
                 * set (no CRC) the header is 7 bytes, otherwise 9 bytes. */
                adts_header = (hdr[1] & 1) ? 7 : 9;
                frame_payload_len = frame_len - adts_header;

                /* Write the raw ADTS frame to the recording file (if any).
                 * Done before the RTP enqueue so recorded audio stays complete
                 * even when the RTSP audio queue is full. */
                if (VideoRecorder.recording == 1 && VideoRecorder.fh_aac != NULL) {
                    fwrite(aac_data + consumed, 1, frame_len, VideoRecorder.fh_aac);
                    fflush(VideoRecorder.fh_aac);
                }

                if (pb->play == 1) {
                    payload = aac_data + consumed + adts_header - 4;
                    payload_len = frame_payload_len + 4;
                    payload[0] = 0;
                    payload[1] = 0x10;
                    payload[2] = frame_payload_len >> 5;
                    payload[3] = frame_payload_len << 3;

                    memset(&entity, 0, sizeof(entity));
                    entity.data = payload;
                    entity.size = payload_len;
                    /* The driver may deliver several ADTS frames per receive
                     * (block_count); give each one its own RTP timestamp so
                     * the client sees consecutive AUs at the right instants. */
                    entity.timestamp = multi_bs.bs.timestamp * (rtp_clock / 1000)
                                       + aac_frame_no * cliArgs.audio_frame_samples;
                    pthread_mutex_lock(&stream_queue_mutex);
                    ret = stream_media_enqueue(convert_gmss_audio_type(enc_type), pb->audio.qno, &entity);
                    pthread_mutex_unlock(&stream_queue_mutex);
                    if (ret < 0) {
                        /* ERR_FULL: drop the frame; log at most once per 5s */
                        if (ret != ERR_FULL) {
                            log_error("audio enqueue failed! ret = %d", ret);
                        } else {
                            struct timeval aq_now;
                            gettimeofday(&aq_now, NULL);
                            if (TIMEVAL_DIFF(aq_err_tval, aq_now) > 5000000) {
                                log_error("audio queue full, dropping AAC frames");
                                aq_err_tval = aq_now;
                            }
                        }
                    }
                }
                consumed += frame_len;
                aac_frame_no++;
            }
        } else {
            /* PCM / G711 / G726 (ADPCM): raw payload, no headers */
            payload = multi_bs.bs.bs_buf;
            payload_len = multi_bs.bs.bs_len;

            memset(&entity, 0, sizeof(entity));
            entity.data = payload;
            entity.size = payload_len;
            entity.timestamp = multi_bs.bs.timestamp * (rtp_clock / 1000);
            pthread_mutex_lock(&stream_queue_mutex);
            ret = stream_media_enqueue(convert_gmss_audio_type(enc_type), pb->audio.qno, &entity);
            pthread_mutex_unlock(&stream_queue_mutex);
            if (ret < 0 && ret != ERR_FULL) {
                log_error("audio enqueue failed! ret = %d", ret);
            }
        }
    }
    pthread_exit(NULL);
    audio_encode_thread_id = (pthread_t)NULL;

    return 0;
}

void update_video_sdp(int cap_ch, int cap_path, int rec_track)
{
    char *bitstream_data = NULL;
    unsigned int bitstream_data_len;
    gm_enc_multi_bitstream_t bs;
    gm_pollfd_t poll_fds;
    int ret, cnt=0, w, h;
    gm_enc_t *param;
    priv_avbs_t *pb;

    memset(&poll_fds, 0, sizeof(poll_fds));
    param = &enc_param[cap_ch][cap_path];

    if (param->bindfd[rec_track]) {
        poll_fds.bindfd = param->bindfd[rec_track];
        poll_fds.event = GM_POLL_READ;
        get_enc_res(&param->enc[rec_track], NULL, &w, &h);
        pb = &enc[cap_ch].priv_bs[cap_path];
        bitstream_data_len = w * h * 3 / 2;
        bitstream_data = malloc(bitstream_data_len);
    }
    else return;

    while(1) {
        ret = gm_poll(&poll_fds, 1, 2000);
        if ( ret == GM_TIMEOUT ) {
            log_error("GM Poll timeout");
            continue;
        }
        memset(&bs, 0, sizeof(bs));
        if ( poll_fds.revent.event != GM_POLL_READ )
            continue;
        if ( poll_fds.revent.bs_len > bitstream_data_len) {
            log_error("bitstream buffer length is too small! %d, %d", poll_fds.revent.bs_len, bitstream_data_len);
            continue;
        }
        bs.bindfd = poll_fds.bindfd;        		
        bs.bs.bs_buf = bitstream_data;				// * Set buffer point
        bs.bs.bs_buf_len = bitstream_data_len;      // * Set buffer lengt
        bs.bs.mv_buf = 0;        					// * Turn off receiving motion data
        bs.bs.mv_buf_len = 0;						// * Not to receive MV data
        ret = gm_recv_multi_bitstreams(&bs, 1);     // * -1: Fail 0: Success
        if ( ret < 0 )
            log_error("Failed to receive bitstream (gm_recv_multi_bitstreams).");
        else if ( (bs.retval < 0) && bs.bindfd )
            log_error("Failed to receive bitstream.");
        else if ( ret == 0 && bs.retval == GM_SUCCESS ) {
            if (bs.bs.keyframe == 1 ) {
                switch (cliArgs.encoderType) {
                    case 0:
                        stream_sdp_parameter_encoder("H264", (unsigned char *) bs.bs.bs_buf, bs.bs.bs_len, pb->video.sdpstr, SDPSTR_MAX);
                        break;
                    case 1:
                        stream_sdp_parameter_encoder("MPEG4", (unsigned char *) bs.bs.bs_buf, bs.bs.bs_len, pb->video.sdpstr, SDPSTR_MAX);
                        break;
                    case 2:
                        stream_sdp_parameter_encoder("MJPEG", (unsigned char *) bs.bs.bs_buf, bs.bs.bs_len, pb->video.sdpstr, SDPSTR_MAX);
                        break;
                    }
                	break;
            }
            else {
                if (++cnt > 100) {
                    log_error("Timeout reached while waiting for keyframe");
                    break;
                }
            }
        }
    }
    if (bitstream_data)
        free(bitstream_data);
}

static int rtspd_start(int port)
{
    int ret, ch_num, stream;
    pthread_attr_t attr;

    if (rtspd_sysinit == 1)
        return -1;
    if ((0 < port) && (port < 0x10000))
        sys_port = port;
    if ((ret = env_init()) < 0)
        return ret;
    if (pthread_mutex_init(&stream_queue_mutex, NULL)) {
        log_error("rtspd_start: mutex init failed");
        exit(-1);
    }

    rtspd_sysinit = 1;

    // * Encode Thread
    if (encode_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&encode_thread_id, &attr, &encode_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    // * Audio Thread
    if (cliArgs.audio_enabled && audio_encode_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&audio_encode_thread_id, &attr, &audio_encode_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    // * OSD Thread
    if (cliArgs.osd && osd_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&osd_thread_id, &attr, &rtspd_osd_thread, enc_param[0][0].cap.obj);
        pthread_attr_destroy(&attr);
    }

    // * Snapshot Thread
    if (media_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&media_thread_id, &attr, &media_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    // * Motion Thread
    if (cliArgs.motion == 1) {
        if (motion_thread_id == (pthread_t)NULL) {
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            ret = pthread_create(&motion_thread_id, &attr, &motion_thread, NULL);
            pthread_attr_destroy(&attr);
        }
    }

    // * Enqueue Thread
    if (enqueue_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&enqueue_thread_id, &attr, &enqueue_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    // * ePTZ Zoom Thread (digital zoom, controlled by the ONVIF server)
    if (zoom_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&zoom_thread_id, &attr, &rtspd_zoom_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    // * Ctrl Thread (reads /tmp/rtspd.ctrl for codec_ctrl commands)
    if (ctrl_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&ctrl_thread_id, &attr, &rtspd_ctrl_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    for (ch_num = 0; ch_num < CAP_CH_NUM; ch_num++) {
        pthread_mutex_lock(&enc[ch_num].ubs_mutex);
        for (stream = 0; stream < RTSP_NUM_PER_CAP; stream++)
            env_set_bs_new_event(ch_num, stream, START_BS_EVENT);
        pthread_mutex_unlock(&enc[ch_num].ubs_mutex);
    }
    return 0;
}

int is_bs_all_disable(void)
{
    av_t *e;
    int ch_num, sub_num;

    for (ch_num=0; ch_num < CAP_CH_NUM; ch_num++) {
        e = &enc[ch_num];
        for(sub_num=0; sub_num < RTSP_NUM_PER_CAP; sub_num++) {
            if (e->bs[sub_num].enabled == DVR_ENC_EBST_ENABLE)
                return 0;
        }
    }
    return 1;
}

static void rtspd_stop(void)
{
    if (cliArgs.motion == 1)
        motion_detection_end();
    if (cliArgs.record == 1 && VideoRecorder.recording == 1) {
        if (stop_recording() < 0)
            log_error("Failed to stop recording in rtspd_stop");
    }
    if (snapshot_buf) {
        free(snapshot_buf);
        snapshot_buf = NULL;
    }
    pthread_mutex_destroy(&stream_queue_mutex);
    rtspd_sysinit = 0;
}

char *get_local_ip(void)
{
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
    ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    //return inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    struct sockaddr_in sin;
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    return inet_ntoa(sin.sin_addr);
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf(" ./rtspd [-bfwhm] [-j|-4]\n");
    printf(
        "\nAvailable options:\n"
        "-b [1-16384]   - Set the bitrate         (default: 8192)\n"
        "-f [1-15]      - Set the framerate       (default: 15)\n"
        "-w [1-1280]    - Set the image width     (default: 1280 pixels)\n"
        "-h [1-720]     - Set the image height    (default: 720 pixels)\n"
        "-m [1-4]       - Set the bitrate mode    (default: 1, CBR)\n"
        "-o [1 or 0]    - Enable OSD overlay      (default: on, timestamp updated every second)\n"
        "-t [text]      - Set OSD overlay text    (default: 'hostname')\n"
        "-z [0-4]       - Set OSD font zoom (0=none,1=2x,2=3x,3=4x,4=1/2) (default: 1)\n"
		"-B [0-15]      - Set OSD background palette index (default: 1=Black)\n\n"
        
        "-j (optional)  - Use MJPEG encoding      (default: off)\n"
        "-4 (optional)  - Use MPEG4 encoding      (default: off)\n"
        "-d (optional)  - Enable motion detection (default: off)\n"
        "-s (optional)  - Take a snapshot when motion detected (default: off)\n"
        "-r (optional)  - Record a 10 second clip on motion    (default: off)\n\n"

        "Audio options:\n"
        "-X [type]      - Audio encode type: aac|pcm|g726|adpcm|g711a|alaw|g711u|ulaw (default: aac)\n"
        "-A [8000-48000]- Audio sample rate in Hz: 8000/16000/32000/44100/48000 (default: 16000)\n"
        "-R [1-192000]  - Audio bitrate in bits/sec (AAC: 14500~192000)      (default: 16000)\n"
        "-S [samples]   - Samples per frame (AAC: 1024*n, PCM: 250~2048, ADPCM: 505*n, G711: 320*n)\n"
        "-C [8|16]      - Audio sample size in bits                          (default: 16)\n"
        "-P [1|2]       - Audio channel type: 1=mono 2=stereo                (default: 1)\n"
        "-q             - Disable audio on the RTSP stream                   (default: audio on)\n\n"

        "Capture / Encoder options:\n"
        "-F [mode]      - Flip capture: h, v, hv, or 0 (default: none)\n"
        "-G [degrees]   - Rotate capture: 0, 90, 180, 270  (default: 0)\n"
        "-V [profile]   - H264 profile: baseline|main|high|default  (default: default)\n"
        "-L [level]     - H264 level: 10-51 (e.g. 31=3.1, 40=4.0, 41=4.1)  (default: 0)\n"
        "-E [coding]    - H264 entropy coding: cavlc|cabac|default  (default: default)\n"
        "-I [preset]    - H264 config: perf|light|quality|default  (default: default)\n"
        "-U [0|1]       - VUI full-range color (0=limited, 1=full)  (default: 1)\n"
        "-N [WxH]       - Sample aspect ratio (e.g. 1x1, 4:3)      (default: 1x1)\n"
        "-Z [x,y,w,h]   - ROI encoding region in pixels (default: off)\n"
        "-Q [on|off]    - Enable 8-region ROI QP (default: off)\n"
        "-Y [num:den]   - Fractional framerate (e.g. 30000:1001 for 29.97)\n"
    );

	exit(EXIT_FAILURE);
}

void signal_handler(int sig)
{
    log_fatal("Exiting rtspd: CTRL+C pressed, or exit requested");
    rtspd_stop();
    gm_graph_release();
    exit(EXIT_SUCCESS);
}

void setup_logging(void)
{
    logfile = fopen(RTSPD_LOGFILE, "a");
    if(logfile)
        log_set_fp(logfile);
}

int main(int argc, char *argv[])
{
    int i;
    int cap_ch, cap_path, rec_track;

    setup_logging();    // * Setup logging

    /* If we were relaunched after a codec change, wait for the old process
     * to release the encoder and drop inherited device fds. */
    restart_handoff();

    saved_argc = argc;
    for (i = 0; i < argc && i < 64; i++)
        saved_argv[i] = argv[i];
    if (argc < 64)
        saved_argv[argc] = NULL;

    cliArgs.bitrate     = 8192;
    cliArgs.framerate   = 15;
    cliArgs.width       = 1280;
    cliArgs.height      = 720;
    cliArgs.bitrateMode = GM_EVBR;
    cliArgs.gop         = 20;
    cliArgs.encoderType = ENC_TYPE_H264;

    cliArgs.snapshot    = 0;    // * disable by default
    cliArgs.record      = 0;    // * disable by default
    cliArgs.motion      = 0;    // * disable by default
    cliArgs.osd         = 1;    // * enabled by default
    cliArgs.font_zoom   = 1;    // * small=GM_OSD_FONT_ZOOM_NONE, 1=minimum, 2=normal
    cliArgs.osd_bg_color= 1;    // * default is black
    cliArgs.osd_text[0] = '\0';

    /* Audio defaults: AAC-LC 16000 Hz 16-bit mono (same as original firmware) */
    cliArgs.audio_enabled       = 1;
    cliArgs.audio_sample_rate   = 16000;
    cliArgs.audio_sample_size   = 16;
    cliArgs.audio_bitrate       = 16000;
    cliArgs.audio_frame_samples = 1024;
    cliArgs.audio_channel_type  = GM_MONO;
    cliArgs.audio_encode_type   = GM_AAC;

    /* Capture defaults: no flip, no rotation */
    cliArgs.h_flip       = 0;
    cliArgs.v_flip       = 0;
    cliArgs.rotation     = 0;

    /* H264 encoder defaults */
    cliArgs.h264_profile = 0;  /* default (let gmlib decide) */
    cliArgs.h264_level   = 0;
    cliArgs.h264_config  = 0;
    cliArgs.h264_coding  = 0;

    /* VUI defaults */
    cliArgs.vui_colorspace = 1;  /* BT.709 */
    cliArgs.vui_full_range = 1;  /* full range 0-255 for better color */

    /* SAR defaults: 1:1 */
    cliArgs.sar_width    = 1;
    cliArgs.sar_height   = 1;

    /* ROI defaults: disabled */
    cliArgs.roi_enabled  = 0;
    cliArgs.roi_x = cliArgs.roi_y = cliArgs.roi_w = cliArgs.roi_h = 0;

    /* ROI QP defaults: disabled */
    cliArgs.roiqp_enabled = 0;

    /* Fractional framerate defaults: 0 = use integer framerate */
    cliArgs.fps_ratio_num = 0;
    cliArgs.fps_ratio_den = 0;

    if (argc > 1) {
        for (i = 1; i < argc; i++) {
            if (argv[i][0] != '-' ) {
                log_error("Invalid input: %s!", argv[i]);
                print_usage();
                return 1;
            } else {
                switch (argv[i][1]) {
                    case 'd':
                        cliArgs.motion      = 1;    				// * Enable motion detection
                        break;
                    case 's':
                        cliArgs.snapshot    = 1;    				// * Enable snapshot when motion detected
                        break;
                    case 'r':
                        cliArgs.record      = 1;    				// * Enable record stream when motion detected
                        break;
                    case 'b':
                        cliArgs.bitrate     = atoi(&argv[i][2]);	// * Set bitrate higher for smooth
                        break;
                    case 'f':
                        cliArgs.framerate   = atoi(&argv[i][2]);	// * Set framerate only accept scaling down
                        break;
                    case 'w':
                        cliArgs.width       = atoi(&argv[i][2]);	// * Set width
                        break;
                    case 'h':
                        cliArgs.height      = atoi(&argv[i][2]);	// * Set height
                        break;
                    case 'm':
                        cliArgs.bitrateMode = atoi(&argv[i][2]);	// * Set biterateMode 
                        break;
                    case 'j':
                        cliArgs.encoderType = ENC_TYPE_MJPEG;
                        break;
                    case '4':
                        cliArgs.encoderType = ENC_TYPE_MPEG4;
                        break;
                    case 'o':
                        cliArgs.osd = 1;							// Set OSD costumize
                        if (argv[i][2] != '\0') {
                            strncpy(cliArgs.osd_text, &argv[i][2], sizeof(cliArgs.osd_text) - 1);
                            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
                        } else if ((i + 1) < argc && argv[i + 1][0] != '-') {
                            strncpy(cliArgs.osd_text, argv[++i], sizeof(cliArgs.osd_text) - 1);
                            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
                        }
                        break;
                    case 'B':
                        cliArgs.osd = 1;							// * Set background color osd
                        if (argv[i][2] != '\0') {
                            cliArgs.osd_bg_color = atoi(&argv[i][2]);
                        } else if ((i + 1) < argc && argv[i + 1][0] != '-') {
                            cliArgs.osd_bg_color = atoi(argv[++i]);
                        }
                        if (cliArgs.osd_bg_color < 0 || cliArgs.osd_bg_color > 15)
                            cliArgs.osd_bg_color = 1;
                        break;
                    case 'z':										// * Set zoom font osd
                        /* expect a digit after -z or -zN */
                        if (argv[i][2] != '\0')
                            cliArgs.font_zoom = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.font_zoom = atoi(argv[++i]);
                        else
                            cliArgs.font_zoom = GM_OSD_FONT_ZOOM_NONE;
                        if (cliArgs.font_zoom < 0 || cliArgs.font_zoom > 12)
                            cliArgs.font_zoom = GM_OSD_FONT_ZOOM_NONE;
                        break;
                    case 'X':										// * Set audio encode type
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (strcasecmp(v, "aac") == 0)          cliArgs.audio_encode_type = GM_AAC;
                                else if (strcasecmp(v, "pcm") == 0)     cliArgs.audio_encode_type = GM_PCM;
                                else if (strcasecmp(v, "g726") == 0)    cliArgs.audio_encode_type = GM_ADPCM;
                                else if (strcasecmp(v, "adpcm") == 0)   cliArgs.audio_encode_type = GM_ADPCM;
                                else if (strcasecmp(v, "g711a") == 0)   cliArgs.audio_encode_type = GM_G711_ALAW;
                                else if (strcasecmp(v, "alaw") == 0)    cliArgs.audio_encode_type = GM_G711_ALAW;
                                else if (strcasecmp(v, "g711u") == 0)   cliArgs.audio_encode_type = GM_G711_ULAW;
                                else if (strcasecmp(v, "ulaw") == 0)    cliArgs.audio_encode_type = GM_G711_ULAW;
                                else {
                                    log_error("Unknown audio encode type: %s", v);
                                    print_usage();
                                    return 1;
                                }
                                cliArgs.audio_enabled = 1;
                            }
                        }
                        break;
                    case 'A':										// * Set audio sample rate
                        if (argv[i][2] != '\0')
                            cliArgs.audio_sample_rate = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_sample_rate = atoi(argv[++i]);
                        else
                            cliArgs.audio_sample_rate = 0;
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'R':										// * Set audio bitrate
                        if (argv[i][2] != '\0')
                            cliArgs.audio_bitrate = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_bitrate = atoi(argv[++i]);
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'S':										// * Set audio frame samples
                        if (argv[i][2] != '\0')
                            cliArgs.audio_frame_samples = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_frame_samples = atoi(argv[++i]);
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'C':										// * Set audio sample size (bits)
                        if (argv[i][2] != '\0')
                            cliArgs.audio_sample_size = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_sample_size = atoi(argv[++i]);
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'P':										// * Set audio channel type
                        if (argv[i][2] != '\0')
                            cliArgs.audio_channel_type = (atoi(&argv[i][2]) == 2) ? GM_STEREO : GM_MONO;
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_channel_type = (atoi(argv[++i]) == 2) ? GM_STEREO : GM_MONO;
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'q':										// * Disable audio
                        cliArgs.audio_enabled = 0;
                        break;
                    case 't':
                        if (argv[i][2] != '\0') {
                            strncpy(cliArgs.osd_text, &argv[i][2], sizeof(cliArgs.osd_text) - 1);
                            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
                        } else if ((i + 1) < argc && argv[i + 1][0] != '-') {
                            strncpy(cliArgs.osd_text, argv[++i], sizeof(cliArgs.osd_text) - 1);
                            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
                        }
                        cliArgs.osd = 1;
                        break;

                    /* --- Capture flip (gm_cap_flip_t) --- */
                    case 'F':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (strcasecmp(v, "h") == 0)         { cliArgs.h_flip = 1; cliArgs.v_flip = 0; }
                                else if (strcasecmp(v, "v") == 0)    { cliArgs.h_flip = 0; cliArgs.v_flip = 1; }
                                else if (strcasecmp(v, "hv") == 0)   { cliArgs.h_flip = 1; cliArgs.v_flip = 1; }
                                else if (strcmp(v, "0") == 0)         { cliArgs.h_flip = 0; cliArgs.v_flip = 0; }
                                else {
                                    log_error("Invalid flip mode: %s (use h, v, hv, or 0)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    /* --- Capture rotation (gm_rotation_attr_t) --- */
                    case 'G':
                        cliArgs.rotation = atoi(&argv[i][2]);
                        if (argv[i][2] == '\0' && (i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.rotation = atoi(argv[++i]);
                        if (cliArgs.rotation != 0 && cliArgs.rotation != 90 &&
                            cliArgs.rotation != 180 && cliArgs.rotation != 270) {
                            log_error("Rotation must be 0, 90, 180, or 270 (got %d)", cliArgs.rotation);
                            return 1;
                        }
                        break;

                    /* --- H264 profile (gm_h264e_profile_t) --- */
                    case 'V':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (strcasecmp(v, "baseline") == 0)      cliArgs.h264_profile = GM_H264E_BASELINE_PROFILE;
                                else if (strcasecmp(v, "main") == 0)     cliArgs.h264_profile = GM_H264E_MAIN_PROFILE;
                                else if (strcasecmp(v, "high") == 0)     cliArgs.h264_profile = GM_H264E_HIGH_PROFILE;
                                else if (strcasecmp(v, "default") == 0)  cliArgs.h264_profile = GM_H264E_DEFAULT_PROFILE;
                                else {
                                    log_error("Invalid H264 profile: %s (use baseline, main, high, default)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    /* --- H264 level (gm_h264e_level_t) --- */
                    case 'L':
                        cliArgs.h264_level = atoi(&argv[i][2]);
                        if (argv[i][2] == '\0' && (i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.h264_level = atoi(argv[++i]);
                        break;

                    /* --- H264 coding: CABAC vs CAVLC --- */
                    case 'E':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (strcasecmp(v, "cavlc") == 0)        cliArgs.h264_coding = GM_H264E_CAVLC_CODING;
                                else if (strcasecmp(v, "cabac") == 0)   cliArgs.h264_coding = GM_H264E_CABAC_CODING;
                                else if (strcasecmp(v, "default") == 0) cliArgs.h264_coding = GM_H264E_DEFAULT_CODING;
                                else {
                                    log_error("Invalid H264 coding: %s (use cavlc, cabac, default)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    /* --- H264 config preset --- */
                    case 'I':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (strcasecmp(v, "perf") == 0)         cliArgs.h264_config = GM_H264E_PERFORMANCE_CONFIG;
                                else if (strcasecmp(v, "light") == 0)   cliArgs.h264_config = GM_H264E_LIGHT_QUALITY_CONFIG;
                                else if (strcasecmp(v, "quality") == 0) cliArgs.h264_config = GM_H264E_QUALITY_CONFIG;
                                else if (strcasecmp(v, "default") == 0) cliArgs.h264_config = GM_H264E_DEFAULT_CONFIG;
                                else {
                                    log_error("Invalid H264 config: %s (use perf, light, quality, default)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    /* --- VUI color: full-range flag --- */
                    case 'U':
                        cliArgs.vui_full_range = atoi(&argv[i][2]);
                        if (argv[i][2] == '\0' && (i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.vui_full_range = atoi(argv[++i]);
                        break;

                    /* --- Sample aspect ratio (SAR): WxH --- */
                    case 'N':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (sscanf(v, "%dx%d", &cliArgs.sar_width, &cliArgs.sar_height) != 2 &&
                                    sscanf(v, "%d:%d", &cliArgs.sar_width, &cliArgs.sar_height) != 2) {
                                    log_error("Invalid SAR format: %s (use WxH e.g. 1x1 or 4:3)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    /* --- ROI encoding region (gm_enc_roi_attr_t) --- */
                    case 'Z':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v && strcmp(v, "off") != 0 && strcmp(v, "0") != 0) {
                                if (sscanf(v, "%u,%u,%u,%u", &cliArgs.roi_x, &cliArgs.roi_y, &cliArgs.roi_w, &cliArgs.roi_h) != 4) {
                                    log_error("Invalid ROI format: %s (use x,y,w,h or off)", v);
                                    return 1;
                                }
                                cliArgs.roi_enabled = 1;
                            } else {
                                cliArgs.roi_enabled = 0;
                            }
                        }
                        break;

                    /* --- ROI QP 8-region mode (gm_h264_roiqp_attr_t) --- */
                    case 'Q':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v && (strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0))
                                cliArgs.roiqp_enabled = 1;
                            else
                                cliArgs.roiqp_enabled = 0;
                        }
                        break;

                    /* --- Fractional framerate (fps_ratio) --- */
                    case 'Y':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (sscanf(v, "%d:%d", &cliArgs.fps_ratio_num, &cliArgs.fps_ratio_den) != 2 ||
                                    cliArgs.fps_ratio_num <= 0 || cliArgs.fps_ratio_den <= 0) {
                                    log_error("Invalid fps_ratio: %s (use num:den e.g. 30000:1001)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    default:
                        log_error("Unknown argument: %s", argv[i]);
                        print_usage();
                        return 1;
                }
            }
        }
    }

    if ((cliArgs.motion != 1) && (cliArgs.snapshot == 1 || cliArgs.record == 1)) {
        log_error("-d is required when using -s or -r");
        return 1;
    }

    /* Apply pending codec overrides (from codec_ctrl) before validation so
     * the encoder is created with the last requested bitrate/mode/fps/gop. */
    apply_pending_args();

    if ((cliArgs.bitrate < 1) || (cliArgs.bitrate > 16384)) {
        log_error("Use a maximum bitrate of 16384 and a minimum of 1");
        return 1;
    }

    if ((cliArgs.framerate < 1) || (cliArgs.framerate > 30)) {
        log_error("A framerate below 1 or higher than 30 fps is not supported.");
        return 1;
    }

    if ((cliArgs.height < 1) || (cliArgs.height > 720)) {
        log_error("A height bigger than 720p or below 1 is not supported.");
        return 1;
    }

    if ((cliArgs.width < 1) || (cliArgs.width > 1280)) {
        log_error("A width wider than 1280p is not supported.");
        return 1;
    }

    if ((cliArgs.bitrateMode < 1) || (cliArgs.bitrateMode > 4)) {
        log_error("Bitrate mode should be in between 1 and 4");
        return 1;
    }

    if (cliArgs.audio_enabled) {
        /* gmlib sample rate: 8000, 16000, 32000, 44100, 48000... */
        switch (cliArgs.audio_sample_rate) {
            case 8000:
            case 16000:
            case 32000:
            case 44100:
            case 48000:
                break;
            default:
                log_error("Audio sample rate %d not supported (use 8000/16000/32000/44100/48000)", cliArgs.audio_sample_rate);
                return 1;
        }
        if (cliArgs.audio_sample_size != 8 && cliArgs.audio_sample_size != 16) {
            log_error("Audio sample size must be 8 or 16 bits");
            return 1;
        }
        if (cliArgs.audio_channel_type != GM_MONO && cliArgs.audio_channel_type != GM_STEREO) {
            log_error("Audio channel type must be 1 (mono) or 2 (stereo)");
            return 1;
        }
        if (convert_gmss_audio_type(cliArgs.audio_encode_type) < 0) {
            log_error("Audio encode type not supported by librtsp");
            return 1;
        }
        if (cliArgs.audio_encode_type == GM_G711_ALAW || cliArgs.audio_encode_type == GM_G711_ULAW ||
            cliArgs.audio_encode_type == GM_ADPCM) {
            if (cliArgs.audio_sample_rate != 8000) {
                log_error("G711/G726 audio requires 8000 Hz sample rate");
                return 1;
            }
        }
    }

    /* If no custom OSD text provided, read HOSTNAME from config and use it */
    if (cliArgs.osd_text[0] == '\0') {
        char hostbuf[32];
        read_hostname(hostbuf, sizeof(hostbuf));
        if (hostbuf[0] != '\0') {
            strncpy(cliArgs.osd_text, hostbuf, sizeof(cliArgs.osd_text) - 1);
            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
        }
    }

    log_info("Starting the RTSP Daemon");

    rtsp_password = getenv("RTSP_PASS");
    rtsp_username = getenv("RTSP_USER");

    if (rtsp_username != NULL && strcmp(rtsp_username, "") != 0 && rtsp_password != NULL && strcmp(rtsp_password, "") != 0) {
        rtsp_use_auth = 1;
        log_info("Enabling stream authentication.");
        log_info("Stream username: %s", rtsp_username);
        log_info("Stream password: %s", rtsp_password);
    }

    // * Initializing gmlib
    gm_graph_init();

    log_info("Width        	: %d", cliArgs.width);
    log_info("Height       	: %d", cliArgs.height);
    log_info("Encoder      	: %s", cliArgs.encoderType == ENC_TYPE_H264 ? "H264" : cliArgs.encoderType == ENC_TYPE_MJPEG ? "MJPEG" : "MPEG4");
    log_info("Framerate    	: %d", cliArgs.framerate);
    log_info("Bitrate      	: %d", cliArgs.bitrate);
    log_info("Bitrate Mode 	: %d", cliArgs.bitrateMode);
	log_info("IP Local		: %s", get_local_ip());	

    // * Use our handler for the signals so we can do some cleanup at quit
    signal(SIGINT,  signal_handler);
    signal(SIGHUP,  signal_handler);
    signal(SIGTERM, signal_handler);

    for (cap_ch = 0; cap_ch < CAP_CH_NUM; cap_ch++) {
        for (cap_path = 0; cap_path < CAP_PATH_NUM; cap_path++) {
            for (rec_track = 0; rec_track < ENC_TRACK_NUM; rec_track++) {
                update_video_sdp(cap_ch, cap_path, rec_track);
            }
        }
    }
	// * Start the rtsp threads
    rtspd_start(554);
    
	while(1) {
        usleep(10000);
    }
    return 0;
}
