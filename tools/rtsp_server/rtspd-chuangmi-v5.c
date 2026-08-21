/* @file rtspd-chuangmi-v5.c
 *  RTSP server for Chuangmi v5 firmware (internal storage, syslog)
 *  Based on rtspd.c with v5-specific modifications:
 *  - Uses syslog instead of file-based logging
 *  - Runs on internal storage (/tmp/) instead of SD card (/tmp/sd/)
 *  - No motion detection, recording, or snapshot features
 *  - Configurable audio (AAC, PCM, G711, G726 via CLI options)
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
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <dirent.h>
#include <syslog.h>

#include "gmlib.h"
#include "librtsp.h"

#define DVR_ENC_EBST_ENABLE      0x55887799
#define DVR_ENC_EBST_DISABLE     0

#define ENC_TYPE_H264            0
#define ENC_TYPE_MPEG4           1
#define ENC_TYPE_MJPEG           2

#define CAP_CH_NUM               1
#define RTSP_NUM_PER_CAP         1
#define CAP_PATH_NUM             4
#define ENC_TRACK_NUM            4

#define SDPSTR_MAX               512
#define SR_MAX                   64
#define VQ_MAX                   (SR_MAX)
#define VQ_LEN                   100
#define AQ_MAX                   64
#define AQ_LEN                   32
#define AV_NAME_MAX              127

#define RTP_HZ                   90000

#define ERR_GOTO(x, y)           do { ret = x; goto y; } while(0)
#define MUTEX_FAILED(x)          (x == ERR_MUTEX)
#define VIDEO_FRAME_NUMBER       VQ_LEN+1

#define NONE_BS_EVENT            0
#define START_BS_EVENT           1
#define STOP_BS_EVENT            2

#define OSD_PALETTE_COLOR_BLACK             0x10801080
#define OSD_PALETTE_COLOR_BLUE              0x296e29f0
#define OSD_PALETTE_COLOR_GREEN             0x5151515B
#define OSD_PALETTE_COLOR_RED               0x52F0525A
#define OSD_PALETTE_COLOR_WHITE             0xEB80EB80

#define CHECK_CHANNUM_AND_SUBNUM(ch_num, sub_num)    \
    do {    \
        if ((ch_num >= CAP_CH_NUM || ch_num < 0) || \
            (sub_num >= RTSP_NUM_PER_CAP || sub_num < 0)) {    \
            log_error("%s: ch_num=%d, sub_num=%d error!",__FUNCTION__, ch_num, sub_num);    \
            return -1; \
        }    \
    } while(0)    \

#define log_info(...) syslog(LOG_INFO, __VA_ARGS__)
#define log_error(...) syslog(LOG_ERR, __VA_ARGS__)
#define log_fatal(...) syslog(LOG_CRIT, __VA_ARGS__)

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
    int enabled;
    int enc_type;
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
    int event;
    int enabled;
    opt_type_t opt_type;
    vbs_t video;
} avbs_t;

typedef struct st_priv_bs {
    int play;
    int congest;
    int sr;
    char name[AV_NAME_MAX];
    open_container_fn open;
    close_container_fn close;
    priv_vbs_t video;
    priv_vbs_t audio;
} priv_avbs_t;

typedef struct st_av {
    avbs_t bs[RTSP_NUM_PER_CAP];
    pthread_mutex_t ubs_mutex;
    int enabled;
    priv_avbs_t priv_bs[RTSP_NUM_PER_CAP];
} av_t;

pthread_t enqueue_thread_id        = 0;
pthread_t encode_thread_id         = 0;
pthread_t audio_encode_thread_id   = 0;
pthread_t osd_thread_id            = 0;

unsigned int sys_tick         = 0;
struct timeval sys_sec        = {-1, -1};
int sys_port                  = 554;
char *ipptr                   = NULL;

static int rtspd_sysinit      = 0;
static int rtspd_set_event    = 0;
static int rtspd_avail_ch     = 0;

pthread_mutex_t stream_queue_mutex;
av_t enc[CAP_CH_NUM];
gm_system_t gm_system;

void *groupfd;
void *bindfd;
void *capture_object;
void *encode_object;
void *sub_enc_object;
void *sub_bindfd;

void *audio_bindfd;
void *audio_grab_object;
void *audio_encode_object;

char *audio_data;

static unsigned short rtspd_osd_font2_text[64];
static int rtspd_osd_font2_ready = 0;
static pthread_mutex_t rtspd_osd_mutex = PTHREAD_MUTEX_INITIALIZER;

struct CommandLineArguments {
    int framerate;
    int height;
    int width;
    int bitrate;
    int bitrateMode;
    int encoderType;
    int osd;
    int font_zoom;
    int osd_bg_color;
    char osd_text[32];
    char *user;
    char *password;

    int audio_sample_rate;
    int audio_sample_size;
    int audio_bitrate;
    int audio_frame_samples;
    int audio_channel_type;
    int audio_encode_type;
    int audio_enabled;

    int h_flip;
    int v_flip;
    int rotation;
    int h264_profile;
    int h264_level;
    int h264_config;
    int h264_coding;
    int vui_colorspace;
    int vui_full_range;
    int sar_width;
    int sar_height;

    int roi_enabled;
    unsigned int roi_x, roi_y, roi_w, roi_h;
    int roiqp_enabled;
    int fps_ratio_num;
    int fps_ratio_den;
} cliArgs;

/* Read HOSTNAME from config file. Try common locations. */
static void read_hostname(char *out, size_t outlen)
{
    FILE *f;
    char line[256];
    out[0] = '\0';
    f = fopen("/tmp/hostname", "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = '\0';
        }
        strncpy(out, line, outlen - 1);
        out[outlen - 1] = '\0';
    }
    fclose(f);
}

static gm_palette_table_t rtspd_osd_palette = {
    palette_table: {
        OSD_PALETTE_COLOR_BLACK,
        OSD_PALETTE_COLOR_BLUE,
        OSD_PALETTE_COLOR_GREEN,
        OSD_PALETTE_COLOR_RED,
        OSD_PALETTE_COLOR_WHITE,
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
    osd_font2.font_palette_idx = 4;
    osd_font2.priority = GM_OSD_PRIORITY_MARK_ON_OSD;
    osd_font2.smooth.enabled = 1;
    osd_font2.smooth.level = GM_OSD_FONT_SMOOTH_LEVEL_WEAK;
    osd_font2.marquee.mode = GM_OSD_MARQUEE_MODE_NONE;
    osd_font2.win_palette_idx  = cliArgs.osd_bg_color;
    osd_font2.border.enabled = 0;
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

static int rtsp_use_auth = 0;

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

static void build_audio_sdp(int enc_type, int sample_rate, int channels, char *sdp, int sdp_len)
{
    switch (enc_type) {
        case GM_AAC: {
            unsigned int config = (2 << 11) | (aac_sampling_freq_index(sample_rate) << 7) | ((channels & 0x0f) << 3);
            snprintf(sdp, sdp_len, "%X", config);
            break;
        }
        case GM_PCM:
        case GM_ADPCM:
        case GM_G711_ALAW:
        case GM_G711_ULAW:
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

    if (pb->sr < 0)
        log_error("open_live_streaming: ch_num=%d, sub_num=%d setup error", ch_num, sub_num);

    if (rtsp_use_auth == 1) {
        stream_authorization(pb->sr, cliArgs.user, cliArgs.password);
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
            if ( TIMEVAL_DIFF(err_print_tval, curr_tval) > 5000000 )
                log_error("ext enqueue queue ch_num=%d, sub_num=%d full", ch_num, sub_num);
        }
        else if ((ret != ERR_NOTINIT)&& (ret != ERR_MUTEX) && (ret != ERR_NOTRUN)) {
            if (TIMEVAL_DIFF(err_print_tval, curr_tval) > 5000000)
                log_error("ext enqueue queue ch_num=%d, sub_num=%d error %d", ch_num, sub_num, ret);
        }
        if ( TIMEVAL_DIFF(err_print_tval, curr_tval) > 5000000) {
            log_error("ext enqueue queue ch_num=%d, sub_num=%d error %d", ch_num, sub_num, ret);
            gettimeofday(&err_print_tval, NULL );
        }
        goto exit_free_audio_buf;
    }
    return 0;

exit_free_audio_buf:
exit_free_as_buf:
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
    int w = 0, h = 0;
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

#define PRINT_INTERVAL_MS 5000
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
    if (cur_timeval->tv_sec > last_timeval.tv_sec) {
        diff_ms = 1000 + (cur_timeval->tv_usec / 1000) - (last_timeval.tv_usec / 1000);
        diff_ms += (cur_timeval->tv_sec - last_timeval.tv_sec - 1) * 1000;
    } else {
        diff_ms = (cur_timeval->tv_usec - last_timeval.tv_usec) / 1000;
    }
    total_ms += diff_ms;
    if (total_ms >= PRINT_INTERVAL_MS) {
        for (i = 0; i < CAP_CH_NUM; i++) {
            for (j = 0; j < RTSP_NUM_PER_CAP; j++) {
                if (frame_counts[i][j] == 0)
                    continue;
                pb = &enc[i].priv_bs[j];
                gm_enc = &enc_param[pb->video.cap_ch][pb->video.cap_path].enc[pb->video.rec_track];
                get_enc_res(gm_enc, &enc_type, &w, &h);
                sprintf(res_str, "%dx%d", w, h);
                log_info("path=/live/ch%02d_%d cap=%d_%d size=%s enc=%s fps=%d.%d kbps=%d",
                        i,
                        j,
                        pb->video.cap_ch,
                        pb->video.cap_path,
                        res_str,
                        rtsp_enc_type_str[enc_type],
                        (frame_counts[i][j] * 1000 / total_ms),
                        (frame_counts[i][j] * 100000 / total_ms) % 100,
                        (rec_bs_len[i][j] * 8 / 1024) * 1000 / total_ms
                );
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

    gm_update_bs_info();

    srand((unsigned int)time(NULL));

    if ((ret = stream_server_init(ipptr, (int) sys_port, 0, 1200, 256, SR_MAX, VQ_MAX, VQ_LEN, AQ_MAX, AQ_LEN, frm_cb, cmd_cb)) < 0)
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
        param->cap.obj = gm_new_obj(GM_CAP_OBJECT);
        cap_attr.cap_vch = cap_ch;

        cap_attr.path = cap_path;
        cap_attr.enable_mv_data = 0;
        gm_set_attr(param->cap.obj, &cap_attr);

        if ((width >= (gm_system.cap[cap_ch].dim.width / 2)) &&
            (height >= (gm_system.cap[cap_ch].dim.height / 2))) {
            dnr_attr.enabled = 1;
            gm_set_attr(param->cap.obj, &dnr_attr);
        }

        if (cliArgs.h_flip || cliArgs.v_flip) {
            gm_cap_flip_t flip_attr;
            memset(&flip_attr, 0, sizeof(flip_attr));
            flip_attr.h_flip_enabled = cliArgs.h_flip;
            flip_attr.v_flip_enabled = cliArgs.v_flip;
            gm_set_cap_flip(cap_ch, &flip_attr);
            syslog(LOG_DAEMON | LOG_INFO, "Capture flip: h=%d v=%d", cliArgs.h_flip, cliArgs.v_flip);
        }

        if (cliArgs.rotation != 0) {
            DECLARE_ATTR(rotation_attr, gm_rotation_attr_t);
            rotation_attr.enabled = 1;
            rotation_attr.clockwise = cliArgs.rotation;
            gm_set_attr(param->cap.obj, &rotation_attr);
            syslog(LOG_DAEMON | LOG_INFO, "Capture rotation: %d degrees", cliArgs.rotation);
        }

        memcpy(&param->cap.cap_attr, &cap_attr, sizeof(gm_cap_attr_t));
        memcpy(&param->cap.dnr_attr, &dnr_attr, sizeof(gm_3dnr_attr_t));
    }

    param->enc[rec_track].obj = gm_new_obj(GM_ENCODER_OBJECT);
    param->enc[rec_track].enc_type = enc_type;
    switch (enc_type) {
        case ENC_TYPE_H264:
            h264e_attr.dim.width             = width;
            h264e_attr.dim.height            = height;
            h264e_attr.frame_info.framerate  = framerate;
            h264e_attr.ratectl.mode          = mode;
            h264e_attr.ratectl.gop           = 20;
            h264e_attr.ratectl.bitrate       = bitrate;
            h264e_attr.ratectl.bitrate_max   = bitrate;
            h264e_attr.b_frame_num           = 0;
            h264e_attr.enable_mv_data        = 0;
            h264e_attr.ratectl.init_quant    = 25;
            h264e_attr.ratectl.min_quant     = 20;
            h264e_attr.ratectl.max_quant     = 51;

            if (cliArgs.h264_profile)
                h264e_attr.profile_setting.profile = (gm_h264e_profile_t) cliArgs.h264_profile;
            if (cliArgs.h264_level)
                h264e_attr.profile_setting.level = (gm_h264e_level_t) cliArgs.h264_level;
            if (cliArgs.h264_config)
                h264e_attr.profile_setting.config = (gm_h264e_config_t) cliArgs.h264_config;
            if (cliArgs.h264_coding)
                h264e_attr.profile_setting.coding = (gm_h264e_coding_t) cliArgs.h264_coding;

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

            {
                DECLARE_ATTR(vui_attr, gm_h264_vui_attr_t);
                vui_attr.param_info.param.video_format = 5;
                vui_attr.param_info.param.colour_primaries = 1;
                vui_attr.param_info.param.transfer_characteristics = 1;
                vui_attr.param_info.param.matrix_coefficient = (char) cliArgs.vui_colorspace;
                vui_attr.param_info.param.full_range = cliArgs.vui_full_range & 1;
                vui_attr.param_info.param.timing_info_present_flag = 0;
                vui_attr.sar_info.sar.sar_width = cliArgs.sar_width;
                vui_attr.sar_info.sar.sar_height = cliArgs.sar_height;
                gm_set_attr(param->enc[rec_track].obj, &vui_attr);
            }

            if (cliArgs.roi_enabled) {
                DECLARE_ATTR(roi_attr, gm_enc_roi_attr_t);
                roi_attr.enabled = 1;
                roi_attr.rect.x = cliArgs.roi_x;
                roi_attr.rect.y = cliArgs.roi_y;
                roi_attr.rect.width = cliArgs.roi_w;
                roi_attr.rect.height = cliArgs.roi_h;
                gm_set_attr(param->enc[rec_track].obj, &roi_attr);
            }

            if (cliArgs.roiqp_enabled) {
                DECLARE_ATTR(roiqp_attr, gm_h264_roiqp_attr_t);
                roiqp_attr.enabled = 1;
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
    param->bindfd[rec_track] = gm_bind(enc_groupfd, param->cap.obj, param->enc[rec_track].obj);
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
        log_error("gm_apply fail for audio");
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
    gm_apply(enc_groupfd);
    audio_init();
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
                log_error("GM Poll timeout x%d - forcing rtspd restart", poll_timeout_count);
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
                bs[i][j].bs.bs_buf = pb->video.bs_buf;
                bs[i][j].bs.bs_buf_len = pb->video.bs_buf_len;
                bs[i][j].bs.mv_buf = 0;
                bs[i][j].bs.mv_buf_len = 0;
                if (pb->play == 0)
                    first_play[i][j] = -1;
            }
        }
        if (rcv_nr == 0)
            continue;
        if (rtspd_sysinit == 0)
            break;
        if ( (ret = gm_recv_multi_bitstreams(&bs[0][0], CAP_CH_NUM * RTSP_NUM_PER_CAP)) < 0 ) {
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
        if (pb->play == 0) {
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
            log_error("buffer length is not enough! %d, %d",
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
            char *aac_data = multi_bs.bs.bs_buf;
            int aac_data_len = multi_bs.bs.bs_len;
            int consumed = 0;
            int aac_frame_no = 0;
            static struct timeval aq_err_tval;

            while (consumed + 7 <= aac_data_len) {
                unsigned char *hdr = (unsigned char *)(aac_data + consumed);
                int adts_header, frame_len, frame_payload_len;

                if (hdr[0] != 0xff || (hdr[1] & 0xf6) != 0xf0)
                    break;
                frame_len = ((hdr[3] & 0x03) << 11) | ((int)hdr[4] << 3) | ((hdr[5] >> 5) & 0x07);
                if (frame_len < 7 || consumed + frame_len > aac_data_len)
                    break;

                adts_header = (hdr[1] & 1) ? 7 : 9;
                frame_payload_len = frame_len - adts_header;

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
                    entity.timestamp = multi_bs.bs.timestamp * (rtp_clock / 1000)
                                       + aac_frame_no * cliArgs.audio_frame_samples;
                    pthread_mutex_lock(&stream_queue_mutex);
                    ret = stream_media_enqueue(convert_gmss_audio_type(enc_type), pb->audio.qno, &entity);
                    pthread_mutex_unlock(&stream_queue_mutex);
                    if (ret < 0) {
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
        bs.bs.bs_buf = bitstream_data;
        bs.bs.bs_buf_len = bitstream_data_len;
        bs.bs.mv_buf = 0;
        bs.bs.mv_buf_len = 0;
        ret = gm_recv_multi_bitstreams(&bs, 1);
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

    if (encode_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&encode_thread_id, &attr, &encode_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    if (cliArgs.audio_enabled && audio_encode_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&audio_encode_thread_id, &attr, &audio_encode_thread, NULL);
        pthread_attr_destroy(&attr);
    }

    if (cliArgs.osd && osd_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&osd_thread_id, &attr, &rtspd_osd_thread, enc_param[0][0].cap.obj);
        pthread_attr_destroy(&attr);
    }

    if (enqueue_thread_id == (pthread_t)NULL) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&enqueue_thread_id, &attr, &enqueue_thread, NULL);
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

char *get_local_ip(void)
{
    int fd;
    struct ifreq ifr;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
    ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    struct sockaddr_in sin;
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    return inet_ntoa(sin.sin_addr);
}

static void rtspd_stop(void)
{
    pthread_mutex_destroy(&stream_queue_mutex);
    rtspd_sysinit = 0;
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf(" ./rtspd-v5 [-bfwhmup] [-j|-4]\n");
    printf(
        "\nAvailable options:\n"
        "-b [1-8192]    - Set the bitrate         (default: 8192)\n"
        "-f [1-15]      - Set the framerate       (default: 15)\n"
        "-w [1-1280]    - Set the image width     (default: 1280 pixels)\n"
        "-h [1-720]     - Set the image height    (default: 720 pixels)\n"
        "-m [1-4]       - Set the bitrate mode    (default: 1, CBR)\n"
        "-o [1 or 0]    - Enable OSD overlay      (default: on, timestamp updated every second)\n"
        "-t [text]      - Set OSD overlay text    (default: 'hostname')\n"
        "-z [0-4]       - Set OSD font zoom (0=none,1=2x,2=3x,3=4x,4=1/2) (default: 1)\n"
        "-B [0-5]       - Set OSD background palette index (default: 1=Black)\n"
        "-u string      - Set the user name       (default: none)\n"
        "-p string      - Set the user password   (default: none)\n\n"
        "-j (optional)  - Use MJPEG encoding      (default: off)\n"
        "-4 (optional)  - Use MPEG4 encoding      (default: off)\n\n"

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
        "-U [0|1]       - VUI full-range color (0=limited, 1=full)  (default: 0)\n"
        "-N [WxH]       - Sample aspect ratio (e.g. 1x1, 4:3)      (default: 1x1)\n"
        "-Z [x,y,w,h]  - ROI encoding region in pixels (default: off)\n"
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
    closelog();
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    int i;
    int cap_ch, cap_path, rec_track;

    openlog("rtspd", LOG_PID, LOG_DAEMON);

    cliArgs.bitrate     = 8192;
    cliArgs.framerate   = 15;
    cliArgs.width       = 1280;
    cliArgs.height      = 720;
    cliArgs.bitrateMode = GM_CBR;
    cliArgs.encoderType = ENC_TYPE_H264;
    cliArgs.osd         = 1;
    cliArgs.font_zoom   = 1;
    cliArgs.osd_bg_color= 0;
    cliArgs.osd_text[0] = '\0';
    cliArgs.user        = NULL;
    cliArgs.password    = NULL;

    cliArgs.audio_enabled       = 1;
    cliArgs.audio_sample_rate   = 16000;
    cliArgs.audio_sample_size   = 16;
    cliArgs.audio_bitrate       = 16000;
    cliArgs.audio_frame_samples = 1024;
    cliArgs.audio_channel_type  = GM_MONO;
    cliArgs.audio_encode_type   = GM_AAC;

    cliArgs.h_flip       = 0;
    cliArgs.v_flip       = 0;
    cliArgs.rotation     = 0;
    cliArgs.h264_profile = 0;
    cliArgs.h264_level   = 0;
    cliArgs.h264_config  = 0;
    cliArgs.h264_coding  = 0;
    cliArgs.vui_colorspace = 0;
    cliArgs.vui_full_range = 0;
    cliArgs.sar_width    = 1;
    cliArgs.sar_height   = 1;

    cliArgs.roi_enabled  = 0;
    cliArgs.roi_x = cliArgs.roi_y = cliArgs.roi_w = cliArgs.roi_h = 0;
    cliArgs.roiqp_enabled = 0;
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
                    case 'b':
                        cliArgs.bitrate     = atoi(&argv[i][2]);
                        break;
                    case 'f':
                        cliArgs.framerate   = atoi(&argv[i][2]);
                        break;
                    case 'w':
                        cliArgs.width       = atoi(&argv[i][2]);
                        break;
                    case 'h':
                        cliArgs.height      = atoi(&argv[i][2]);
                        break;
                    case 'm':
                        cliArgs.bitrateMode = atoi(&argv[i][2]);
                        break;
                    case 'j':
                        cliArgs.encoderType = ENC_TYPE_MJPEG;
                        break;
                    case '4':
                        cliArgs.encoderType = ENC_TYPE_MPEG4;
                        break;
                    case 'u':
                        cliArgs.user = &argv[i][2];
                        break;
                    case 'p':
                        cliArgs.password = &argv[i][2];
                        break;
                    case 'o':
                        cliArgs.osd = 1;
                        if (argv[i][2] != '\0') {
                            strncpy(cliArgs.osd_text, &argv[i][2], sizeof(cliArgs.osd_text) - 1);
                            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
                        } else if ((i + 1) < argc && argv[i + 1][0] != '-') {
                            strncpy(cliArgs.osd_text, argv[++i], sizeof(cliArgs.osd_text) - 1);
                            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
                        }
                        break;
                    case 'B':
                        cliArgs.osd = 1;
                        if (argv[i][2] != '\0') {
                            cliArgs.osd_bg_color = atoi(&argv[i][2]);
                        } else if ((i + 1) < argc && argv[i + 1][0] != '-') {
                            cliArgs.osd_bg_color = atoi(argv[++i]);
                        }
                        if (cliArgs.osd_bg_color < 0 || cliArgs.osd_bg_color > 15)
                            cliArgs.osd_bg_color = 1;
                        break;
                    case 'z':
                        if (argv[i][2] != '\0')
                            cliArgs.font_zoom = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.font_zoom = atoi(argv[++i]);
                        else
                            cliArgs.font_zoom = GM_OSD_FONT_ZOOM_NONE;
                        if (cliArgs.font_zoom < 0 || cliArgs.font_zoom > 12)
                            cliArgs.font_zoom = GM_OSD_FONT_ZOOM_NONE;
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
                    case 'X':
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
                    case 'A':
                        if (argv[i][2] != '\0')
                            cliArgs.audio_sample_rate = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_sample_rate = atoi(argv[++i]);
                        else
                            cliArgs.audio_sample_rate = 0;
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'R':
                        if (argv[i][2] != '\0')
                            cliArgs.audio_bitrate = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_bitrate = atoi(argv[++i]);
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'S':
                        if (argv[i][2] != '\0')
                            cliArgs.audio_frame_samples = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_frame_samples = atoi(argv[++i]);
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'C':
                        if (argv[i][2] != '\0')
                            cliArgs.audio_sample_size = atoi(&argv[i][2]);
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_sample_size = atoi(argv[++i]);
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'P':
                        if (argv[i][2] != '\0')
                            cliArgs.audio_channel_type = (atoi(&argv[i][2]) == 2) ? GM_STEREO : GM_MONO;
                        else if ((i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.audio_channel_type = (atoi(argv[++i]) == 2) ? GM_STEREO : GM_MONO;
                        cliArgs.audio_enabled = 1;
                        break;
                    case 'q':
                        cliArgs.audio_enabled = 0;
                        break;

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
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid flip mode: %s (use h, v, hv, or 0)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    case 'G':
                        cliArgs.rotation = atoi(&argv[i][2]);
                        if (argv[i][2] == '\0' && (i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.rotation = atoi(argv[++i]);
                        if (cliArgs.rotation != 0 && cliArgs.rotation != 90 &&
                            cliArgs.rotation != 180 && cliArgs.rotation != 270) {
                            syslog(LOG_DAEMON | LOG_ERR, "Rotation must be 0, 90, 180, or 270 (got %d)", cliArgs.rotation);
                            return 1;
                        }
                        break;

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
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid H264 profile: %s", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    case 'L':
                        cliArgs.h264_level = atoi(&argv[i][2]);
                        if (argv[i][2] == '\0' && (i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.h264_level = atoi(argv[++i]);
                        break;

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
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid H264 coding: %s", v);
                                    return 1;
                                }
                            }
                        }
                        break;

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
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid H264 config: %s", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    case 'U':
                        cliArgs.vui_full_range = atoi(&argv[i][2]);
                        if (argv[i][2] == '\0' && (i + 1) < argc && argv[i + 1][0] != '-')
                            cliArgs.vui_full_range = atoi(argv[++i]);
                        break;

                    case 'N':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (sscanf(v, "%dx%d", &cliArgs.sar_width, &cliArgs.sar_height) != 2 &&
                                    sscanf(v, "%d:%d", &cliArgs.sar_width, &cliArgs.sar_height) != 2) {
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid SAR format: %s (use WxH e.g. 1x1 or 4:3)", v);
                                    return 1;
                                }
                            }
                        }
                        break;

                    case 'Z':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v && strcmp(v, "off") != 0 && strcmp(v, "0") != 0) {
                                if (sscanf(v, "%u,%u,%u,%u", &cliArgs.roi_x, &cliArgs.roi_y, &cliArgs.roi_w, &cliArgs.roi_h) != 4) {
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid ROI format: %s (use x,y,w,h or off)", v);
                                    return 1;
                                }
                                cliArgs.roi_enabled = 1;
                            } else {
                                cliArgs.roi_enabled = 0;
                            }
                        }
                        break;

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

                    case 'Y':
                        {
                            const char *v = NULL;
                            if (argv[i][2] != '\0') v = &argv[i][2];
                            else if ((i + 1) < argc && argv[i + 1][0] != '-') v = argv[++i];
                            if (v) {
                                if (sscanf(v, "%d:%d", &cliArgs.fps_ratio_num, &cliArgs.fps_ratio_den) != 2 ||
                                    cliArgs.fps_ratio_num <= 0 || cliArgs.fps_ratio_den <= 0) {
                                    syslog(LOG_DAEMON | LOG_ERR, "Invalid fps_ratio: %s (use num:den e.g. 30000:1001)", v);
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
    if ((cliArgs.bitrate < 1) || (cliArgs.bitrate > 8192)) {
        log_error("Use a maximum bitrate of 8192 and a minimum of 1");
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

    if (cliArgs.osd_text[0] == '\0') {
        char hostbuf[32];
        read_hostname(hostbuf, sizeof(hostbuf));
        if (hostbuf[0] != '\0') {
            strncpy(cliArgs.osd_text, hostbuf, sizeof(cliArgs.osd_text) - 1);
            cliArgs.osd_text[sizeof(cliArgs.osd_text) - 1] = '\0';
        }
    }

    log_info("Starting the RTSP Daemon");

    if (cliArgs.user != NULL && strcmp(cliArgs.user, "") != 0 && cliArgs.password != NULL && strcmp(cliArgs.password, "") != 0) {
        rtsp_use_auth = 1;
        log_info("Enabling stream authentication.");
        log_info("Stream username: %s", cliArgs.user);
        log_info("Stream password: %s", cliArgs.password);
    }

    gm_graph_init();

    if (cliArgs.osd) {
        rtspd_set_osd_palette();
        rtspd_enable_osd_font(enc_param[0][0].cap.obj,cliArgs.osd_text);
    }

    log_info("Width        : %d", cliArgs.width);
    log_info("Height       : %d", cliArgs.height);
    log_info("Encoder      : %s", cliArgs.encoderType == ENC_TYPE_H264 ? "H264" : cliArgs.encoderType == ENC_TYPE_MJPEG ? "MJPEG" : "MPEG4");
    log_info("Framerate    : %d", cliArgs.framerate);
    log_info("Bitrate      : %d", cliArgs.bitrate);
    log_info("Bitrate Mode : %d", cliArgs.bitrateMode);
    log_info("IP Local     : %s", get_local_ip());

    for (cap_ch = 0; cap_ch < CAP_CH_NUM; cap_ch++) {
        for (cap_path = 0; cap_path < CAP_PATH_NUM; cap_path++) {
            for (rec_track = 0; rec_track < ENC_TRACK_NUM; rec_track++) {
                update_video_sdp(cap_ch, cap_path, rec_track);
            }
        }
    }

    signal(SIGINT,  signal_handler);
    signal(SIGHUP,  signal_handler);
    signal(SIGTERM, signal_handler);

    rtspd_start(554);

    while(1) {
        usleep(10000);
    }

    return 0;
}
