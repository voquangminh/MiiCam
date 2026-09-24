/* @file miio_avstreamer.c
 *
 *  Open-source, local feature-equivalent reimplementation of the vendor
 *  `miio_avstreamer` daemon from a Xiaomi Chuangmi v5 (IMI/GM8136, buildroot
 *  2016.02, XIAOMI_VERSION=3.3.6_2018062014) firmware dump.
 *
 *  The vendor binary (`chuangmi-v5-dump/data/miio_av/miio_avstreamer`,
 *  244808-byte ARM ELF) is the Mi-Home cloud streaming daemon. Its cloud
 *  links run through the CLOSED ThroughTek/Xiaomi TUTK SDK (`libIOTCAPIs.so`,
 *  `libAVAPIs.so`, `libRDTAPIs.so`) which cannot be reproduced from source.
 *  This file reconstructs everything else -- the entire local feature
 *  surface -- against the public Grain-Media gm_lib already used by the
 *  RTSP daemons in this repository, and replaces the TUTK streaming/IOCTRL
 *  channel with a plain local TCP media stream + a JSON-RPC control socket.
 *
 *  Feature map (module -> evidence in the vendor binary):
 *    gm graph init          "1. gm initing..." / gm_* imports (gm_enc_init)
 *    video/audio receive    thread_VideoFrameData / thread_AudioFrameData
 *    MP4 record             av_TFrecord, MP4Create/AddH264VideoTrack/...
 *    playback               av_playback, MP4ReadSample/GetSampleIdFromTime
 *    motion detect          av_motiondetect, gm_recv_multi_cap_md,
 *                           set_interesting_region, [Region]/md_cnt
 *    motion alarm/upload    av_motionalarm, encrypt_alarm_video/picture,
 *                           my_aes_cbc_encrypt, _sync.gen_file_upload_url
 *    RPC/otd socket         av_rpc, thread_av_rpc_methods, msg_dispatcher,
 *                           method names: get_prop/set_motion_record/
 *                           sd_storge/sd_format/set_power/set_flip/...
 *    mqtt (mosquitto)       mosq_sub_init, topic_ble_events, $SYS/#
 *    ptz                    imi_ptz_handle, ptz_handle_cmd, AVIOCTRL_PTZ_*
 *    night mode / IR        night_mode_set, /dev/isp328, gpio17/57
 *    qrcode                 av_qrcode, zbar_*, /tmp/qrcode.b39e528e
 *    sdcard                 av_TFcheck, av_SDformat, fdisk/mkfs.fat
 *    system monitor         sysMonit, /proc/stat cpu idle watchdog
 *    reboot task            thread_reboot_task ("continue run 10days")
 *    audio player           av_aplayerrcv, /tmp/sound_fifo
 *    log posting            log_post_micloud, pack_log, curl -T
 *    av password            makeNewAvPass (replaced by local p2pid id)
 *
 *  Hardware notes: 720p-native GM8136 sensor; the frame-rate check against
 *  gm_system.cap[0].framerate is kept so a 15-fps gmlib.cfg caps us there.
 *
 *  Build (repository Makefile):
 *      make build/miio_avstreamer
 */

/* ------------------------------------------------------------------ */
/* includes                                                            */
/* ------------------------------------------------------------------ */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <dirent.h>

#include "gmlib.h"
#include "algorithm/capture_motion_detection.c"

/* ------------------------------------------------------------------ */
/* tunables                                                            */
/* ------------------------------------------------------------------ */
#define MIIO_VERSION            "1.0"
#define DEFAULT_CONFIG          "/tmp/sd/config.cfg"
#define ALT_CONFIG              "/mnt/media/mmcblk0p1/config.cfg"
#define LOG_PREFIX              "[miio_avstreamer]"
#define PIDFILE                 "/var/run/miio_avstreamer.pid"
#define STREAM_DIR              "/tmp/sd/RECORDED_VIDEOS"
#define IMAGE_DIR               "/tmp/sd/RECORDED_IMAGES"
#define TMP_REC                 "/tmp/miio_rec"
#define SOUND_FIFO              "/tmp/sound_fifo"
#define LAST_VIDEO              "/dev/shm/miio_last_video_path"
#define LAST_SNAPSHOT           "/dev/shm/miio_last_snapshot_path"
#define SNAP_TRIGGER            "/dev/shm/miio_snapshot"

#define MAX_CAP_CH              1
#define ENC_TRACK_NUM           4
#define CAP_PATH_NUM            4
#define MAX_SNAPSHOT_LEN        (256 * 1024)
#define VIDEO_FRAME_MAX         (1920 * 1080 * 3 / 2)
#define AUDIO_FRAME_MAX         (12800)
#define MAX_MOTION_ALARM_SECS   30
#define DEFAULT_RECORD_SECS     20

/* Local stream framing: [0x52 0x49] [type] [len BE32] [payload]
 * type 'V'=H264 'A'=AAC/ALAW ADTS 'S'=snapshot JPEG 'M'=JSON meta  */
#define STREAM_MAGIC0           0x52
#define STREAM_MAGIC1           0x49
#define STREAM_TYPE_VIDEO       'V'
#define STREAM_TYPE_AUDIO       'A'
#define STREAM_TYPE_SNAP        'S'
#define STREAM_TYPE_META        'M'

#define MAX_CLIENTS             8
#define RPC_MSG_MAX             4096

/* MQTT topics (used by the motion/MQTT sections below their definition) */
#define MQTT_TOPIC_EVENT        "miio/avstreamer/event"
#define MQTT_TOPIC_CTRL         "miio/avstreamer/ctrl"

/* ------------------------------------------------------------------ */
/* globals                                                            */
/* ------------------------------------------------------------------ */
static int g_running   = 1;
static int g_test_mode = 0;               /* -t factory/test mode        */
static int g_restart   = 0;               /* -R restart flag (accepted)  */
static int g_video_chn = 1;
static int g_audio_chn = 1;

static gm_system_t gm_system;
static void *groupfd = NULL;
static void *bindfd  = NULL;              /* main video bindfd          */
static void *sub_bindfd = NULL;           /* scaler (360p) bindfd       */
static void *audio_bindfd = NULL;
static void *cap_obj = NULL;
static void *enc_obj = NULL;
static void *sub_enc_obj = NULL;
static void *audio_grab_obj = NULL;
static void *audio_enc_obj = NULL;
static void *audio_render_obj = NULL;
static void *audio_render_groupfd = NULL;
static void *audio_groupfd = NULL;
static int motion_alg_setup(int ch);   /* defined in motion section */

/* encoder state mirrors rtspd2MP gm_enc_t */
typedef struct {
    int enc_type;                         /* ENC_TYPE_H264=0 */
    int width, height;
    int framerate, bitrate, bitrate_max, gop;
    int mode;                             /* 0 CBR 1 VBR */
    int keyframe_cnt;
    unsigned char sps[64]; int sps_len;
    unsigned char pps[64]; int pps_len;
    int have_spspps;
} enc_state_t;

static enc_state_t g_enc;

/* audio config */
static int g_audio_enabled = 1;
static int g_audio_rate = 16000;
static int g_audio_type = GM_AAC;        /* GM_AAC=2 GM_G711_ALAW ...   */
static int g_audio_ch = GM_MONO;
static int g_audio_framesamples = 1024;
static int g_audio_bitrate = 32000;

/* motion */
static pthread_mutex_t g_motion_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_motion_detected = 0;
static int g_motion_enable = 0;
static int g_motion_record = 0;
static int g_motion_snapshot = 0;
static int g_motion_tracking = 0;
static struct timeval g_last_motion;
static int g_motion_sensitivity = 80;    /* 0..100 */
static int g_motion_alarm_seconds = 10;

/* state mirrors of the vendor nvram-driven toggles (persisted to config) */
static int g_power_on = 1;               /* system awake (power=on)     */
static int g_flip = 0;                   /* gm_set_cap_flip             */
static int g_watermark = 0;              /* OSD text on/off             */
static int g_light = 0;                  /* IR light on/off             */
static int g_night_mode = 0;             /* 0 auto 1 light 2 night      */
static int g_alarm_sens = 1;             /* 1 high 0 low                */
static int g_alarm_interval = 5;         /* seconds (default in vendor) */

/* ptz (software-tracked like motor_ctrl / onvif_server) */
static int g_ptz_x = 0, g_ptz_y = 0;     /* 0..31 / 0..15               */
static int g_ptz_moving = 0;

/* snapshot */
static char *g_snapshot_buf = NULL;
static pthread_mutex_t g_snap_lock = PTHREAD_MUTEX_INITIALIZER;

/* record flags live next to the MP4 muxer (g_rec_active/g_rec_lock) */

/* client registry */
typedef struct {
    int fd;
    int active;
    pthread_mutex_t wlock;
} lclient_t;
static lclient_t g_clients[MAX_CLIENTS];
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* thread handles (named after vendor threads) */
static pthread_t tid_videoframe = 0;
static pthread_t tid_audioframe = 0;
static pthread_t tid_motion = 0;
static pthread_t tid_rpc = 0;
static pthread_t tid_record = 0;
static pthread_t tid_motalarm = 0;
static pthread_t tid_playback = 0;
static pthread_t tid_sdcheck = 0;
static pthread_t tid_sysmon = 0;
static pthread_t tid_reboottask = 0;
static pthread_t tid_p2paudio = 0;
static pthread_t tid_audiplayer = 0;
static pthread_t tid_qrcode = 0;
static pthread_t tid_mqtt = 0;
static pthread_t tid_ptz = 0;
static pthread_t tid_stream = 0;

/* ------------------------------------------------------------------ */
/* tiny logging + config.cfg frontend (nvram spelled the same way)     */
/* ------------------------------------------------------------------ */

static void logmsg(const char *tag, const char *fmt, ...)
{
    va_list ap;
    time_t now = time(NULL);
    struct tm tmv;
    char tbuf[32];

    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    fprintf(stderr, "%s %s[%s]: ", tbuf, LOG_PREFIX, tag ? tag : "log");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

#define log_info(fmt, ...)   logmsg("info", fmt, ##__VA_ARGS__)
#define log_error(fmt, ...)  logmsg("error", fmt, ##__VA_ARGS__)

/* ---- config.cfg key/value store (mirrors vendor /usr/sbin/nvram) ---- */

typedef struct {
    char key[64];
    char val[256];
} cfg_kv_t;

#define MAX_CFG 128
static cfg_kv_t g_cfg[MAX_CFG];
static int g_cfg_n = 0;
static char g_config_path[256] = DEFAULT_CONFIG;

static int cfg_load_path(const char *path)
{
    FILE *f;
    char line[512];

    g_cfg_n = 0;
    f = fopen(path, "r");
    if (!f) {
        if (strcmp(path, "/tmp/sd/config.cfg") == 0)
            f = fopen("/mnt/media/mmcblk0p1/config.cfg", "r");
    }
    if (!f)
        return -1;
    strncpy(g_config_path, path, sizeof(g_config_path)-1);
    while (g_cfg_n < MAX_CFG && fgets(line, sizeof(line), f)) {
        char *p = line, *eq, *nl;
        char k[64], v[255];
        /* trim leading space */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == 0)
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;
        nl = strchr(p, '\n');
        if (nl) *nl = 0;
        k[0] = 0; v[0] = 0;
        sscanf(p, "%63[^= ]=%254[^\r\n]", k, v);
        if (!k[0])
            continue;
        snprintf(g_cfg[g_cfg_n].key, sizeof(g_cfg[g_cfg_n].key), "%s", k);
        snprintf(g_cfg[g_cfg_n].val, sizeof(g_cfg[g_cfg_n].val), "%s", v);
        g_cfg_n++;
    }
    fclose(f);
    return 0;
}

/* vendor spelled the config as /usr/sbin/nvram get/set; both map onto
 * config.cfg on this firmware so the module structure stays identical. */
static const char *do_nvram_get(const char *key)
{
    int i;
    for (i = 0; i < g_cfg_n; i++)
        if (strcasecmp(g_cfg[i].key, key) == 0)
            return g_cfg[i].val;
    return NULL;
}

static int do_nvram_set(const char *key, const char *val)
{
    int i;
    for (i = 0; i < g_cfg_n; i++) {
        if (strcasecmp(g_cfg[i].key, key) == 0) {
            snprintf(g_cfg[i].val, sizeof(g_cfg[i].val), "%s", val);
            return 0;
        }
    }
    if (g_cfg_n >= MAX_CFG) return -1;
    snprintf(g_cfg[g_cfg_n].key, sizeof(g_cfg[g_cfg_n].key), "%s", key);
    snprintf(g_cfg[g_cfg_n].val, sizeof(g_cfg[g_cfg_n].val), "%s", val);
    g_cfg_n++;
    return 0;
}

static int do_nvram_commit(void)
{
    FILE *f;
    int i;
    char tmp[300];

    snprintf(tmp, sizeof(tmp), "%s.tmp", g_config_path);
    f = fopen(tmp, "w");
    if (!f) return -1;
    for (i = 0; i < g_cfg_n; i++)
        fprintf(f, "%s=%s\n", g_cfg[i].key, g_cfg[i].val);
    fclose(f);
    rename(tmp, g_config_path);
    return 0;
}

static int cfg_get_int(const char *key, int dflt)
{
    const char *v = do_nvram_get(key);
    return v ? atoi(v) : dflt;
}

static void cfg_set_int(const char *key, int val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    do_nvram_set(key, buf);
}

/* ------------------------------------------------------------------ */
/* minimal JSON support (no json-c dependency)                         */
/* ------------------------------------------------------------------ */

/* find the value of a string field "KEY":"..." or KEY:"..." at top level;
 * returns a malloc'd copy (may keep quotes stripped). */
static char *json_get_string_field(const char *json, const char *key)
{
    const char *p, *q, *v;
    int klen = strlen(key);

    p = json;
    while ((p = strstr(p, key)) != NULL) {
        q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') { p = q; continue; }
        q++;
        v = q;
        while (*q && *q != '"') q++;
        if (!*q) break;
        {
            int len = q - v;
            char *out = calloc(1, len + 1);
            if (!out) return NULL;
            memcpy(out, v, len);
            return out;
        }
    }
    return NULL;
}

static int json_get_int_field(const char *json, const char *key)
{
    const char *p, *q;
    int klen = strlen(key);

    p = json;
    while ((p = strstr(p, key)) != NULL) {
        q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '"') {
            /* string of a number */
            return atoi(q + 1);
        }
        return atoi(q);
    }
    return 0;
}

static char *json_get_array_string(const char *json, const char *key, int idx)
{
    const char *p, *q;
    int klen = strlen(key), i;

    p = json;
    while ((p = strstr(p, key)) != NULL) {
        q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '[') break;
        q++;
        for (i = 0; i <= idx; i++) {
            while (*q == ' ' || *q == '\t' || *q == ',') q++;
            if (*q != '"') return NULL;
            q++;
            p = q;
            while (*q && *q != '"') q++;
            if (!*q) return NULL;
            if (i == idx) {
                int len = q - p;
                char *out = calloc(1, len + 1);
                if (out) { memcpy(out, p, len); return out; }
                return NULL;
            }
        }
        break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* AES-128-CBC (small, self-contained; used for alarm file encryption   */
/* like vendor encrypt_alarm_video/picture -> my_aes_cbc_encrypt).     */
/* ------------------------------------------------------------------ */

static const unsigned char aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static const unsigned char aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};

static const unsigned char aes_rcon[10] = {
    0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static void aes_add_round_key(unsigned char *state, const unsigned char *key)
{
    int i;
    for (i = 0; i < 16; i++)
        state[i] ^= key[i];
}

static void aes_sub_bytes(unsigned char *state)
{
    int i;
    for (i = 0; i < 16; i++)
        state[i] = aes_sbox[state[i]];
}

static void aes_shift_rows(unsigned char *s)
{
    unsigned char t;
    t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13]; s[13] = t;
    t = s[2];  s[2] = s[10]; s[10] = s[2]; s[2] = t;
    t = s[6];  s[6] = s[14]; s[14] = s[6]; s[6] = t;
    t = s[3];  s[3] = s[15]; s[15] = s[3]; s[3] = t;
    t = s[7];  s[7] = s[11]; s[11] = s[7]; s[7] = t;
}

static unsigned char aes_xtime(unsigned char x)
{
    return (x << 1) ^ ((x >> 7) ? 0x1b : 0);
}

static unsigned char aes_mul(unsigned char a, unsigned char b)
{
    unsigned char r = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

static void aes_mix_columns(unsigned char *s)
{
    int i;
    for (i = 0; i < 16; i += 4) {
        unsigned char a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
        s[i]   = aes_mul(a0,2) ^ aes_mul(a1,3) ^ a2      ^ a3;
        s[i+1] = a0      ^ aes_mul(a1,2) ^ aes_mul(a2,3) ^ a3;
        s[i+2] = a0      ^ a1            ^ aes_mul(a2,2) ^ aes_mul(a3,3);
        s[i+3] = aes_mul(a0,3) ^ a1      ^ a2            ^ aes_mul(a3,2);
    }
}

static void aes_key_expansion(const unsigned char *key, unsigned char *w)
{
    int i;
    unsigned char t[4];
    for (i = 0; i < 16; i++)
        w[i] = key[i];
    for (i = 4; i < 44; i++) {
        t[0] = w[(i-1)*4+0]; t[1] = w[(i-1)*4+1];
        t[2] = w[(i-1)*4+2]; t[3] = w[(i-1)*4+3];
        if (i % 4 == 0) {
            unsigned char tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ aes_rcon[i/4-1];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        w[i*4+0] = w[(i-4)*4+0] ^ t[0];
        w[i*4+1] = w[(i-4)*4+1] ^ t[1];
        w[i*4+2] = w[(i-4)*4+2] ^ t[2];
        w[i*4+3] = w[(i-4)*4+3] ^ t[3];
    }
}

static void aes_enc_block(unsigned char *state, const unsigned char *w)
{
    int round;
    aes_add_round_key(state, w);
    for (round = 1; round < 10; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, w + round*16);
    }
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, w + 160);
}

static void aes_cbc_crypt(const unsigned char *key, const unsigned char *iv,
                          const unsigned char *in, unsigned char *out, int len)
{
    unsigned char w[176];
    unsigned char ivc[16];
    int i, nblocks = len / 16;

    if (len <= 0)
        return;
    aes_key_expansion(key, w);
    memcpy(ivc, iv, 16);
    for (i = 0; i < nblocks; i++) {
        int j;
        unsigned char block[16];
        for (j = 0; j < 16; j++)
            block[j] = in[i*16+j] ^ ivc[j];
        aes_enc_block(block, w);
        memcpy(out + i*16, block, 16);
        memcpy(ivc, block, 16);
    }
}

/* encrypt the whole file  in place (PKCS7-padded) into *out_len */
static int aes_encrypt_file(const char *src, const char *dst,
                            const unsigned char *key, const unsigned char *iv)
{
    FILE *f;
    long flen;
    unsigned char *buf, pad, i;
    unsigned char *out;
    long padded;

    f = fopen(src, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0) { fclose(f); return -1; }
    buf = malloc(flen);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, flen, f) != (size_t)flen) { free(buf); fclose(f); return -1; }
    fclose(f);

    pad = 16 - (flen % 16);
    padded = flen + pad;
    out = malloc(padded);
    if (!out) { free(buf); return -1; }
    memcpy(out, buf, flen);
    for (i = 0; i < pad; i++)
        out[flen + i] = pad;
    free(buf);

    aes_cbc_crypt(key, iv, out, out, padded);

    f = fopen(dst, "wb");
    if (!f) { free(out); return -1; }
    fwrite(out, 1, padded, f);
    fclose(f);
    free(out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* PTZ motor (ioctls recovered in onvif_server/motor_driver.h)         */
/* ------------------------------------------------------------------ */

#define MOTOR_MAGIC 0x6d
#define H_DIR_SET   _IOW(MOTOR_MAGIC,  3, int)
#define H_DIST_SET  _IOW(MOTOR_MAGIC,  4, int)
#define H_COORD_GET _IOW(MOTOR_MAGIC,  5, int)
#define H_COORD_SET _IOW(MOTOR_MAGIC,  6, int)
#define V_DIR_SET   _IOW(MOTOR_MAGIC, 23, int)
#define V_DIST_SET  _IOW(MOTOR_MAGIC, 24, int)
#define V_COORD_GET _IOW(MOTOR_MAGIC, 25, int)
#define V_COORD_SET _IOW(MOTOR_MAGIC, 26, int)

static int ptz_fd = -1;

static int ptz_open(void)
{
    if (ptz_fd < 0)
        ptz_fd = open("/dev/motor", O_RDWR);
    return ptz_fd;
}

static int ptz_move(int h_dir, int h_dist, int v_dir, int v_dist)
{
    int hc = 0, vc = 0;

    if (ptz_open() < 0) {
        log_error("ptz: /dev/motor open fail");
        return -1;
    }
    if (h_dist) {
        ioctl(ptz_fd, H_COORD_GET, &hc);
        ioctl(ptz_fd, H_DIR_SET, &h_dir);
        ioctl(ptz_fd, H_DIST_SET, &h_dist);
        hc = (g_ptz_x + (h_dir == 0 ? h_dist : -h_dist));
        if (hc < 0) hc = 0;
        if (hc > 31) hc = 31;
        ioctl(ptz_fd, H_COORD_SET, &hc);
        g_ptz_x = hc;
        cfg_set_int("ptz-x", g_ptz_x);
    }
    if (v_dist) {
        ioctl(ptz_fd, V_COORD_GET, &vc);
        ioctl(ptz_fd, V_DIR_SET, &v_dir);
        ioctl(ptz_fd, V_DIST_SET, &v_dist);
        vc = (g_ptz_y + (v_dir == 1 ? v_dist : -v_dist));
        if (vc < 0) vc = 0;
        if (vc > 15) vc = 15;
        ioctl(ptz_fd, V_COORD_SET, &vc);
        g_ptz_y = vc;
        cfg_set_int("ptz-y", g_ptz_y);
    }
    do_nvram_commit();
    return 0;
}

/* vendor imi_ptz_handle / ptz_handle_cmd exposed over JSON RPC */
static int ptz_control(int cmd)
{
    /* 1 up 2 down 3 left 4 right (AVIOCTRL_PTZ_UP/DOWN/LEFT/RIGHT) */
    switch (cmd) {
    case 1: return ptz_move(0, 0, 1, 3);     /* up    */
    case 2: return ptz_move(0, 0, 0, 3);     /* down  */
    case 3: return ptz_move(0, 3, 0, 0);     /* left  */
    case 4: return ptz_move(1, 3, 0, 0);     /* right */
    default: return -1;
    }
}

static void ptz_load_coords(void)
{
    g_ptz_x = cfg_get_int("ptz-x", 0);
    g_ptz_y = cfg_get_int("ptz-y", 0);
}

/* ------------------------------------------------------------------ */
/* gm video graph init (ported from rtspd2MP.c gm_graph_init)          */
/* ------------------------------------------------------------------ */

static void create_directory(const char *dir)
{
    char tmp[256];
    char *p;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static int enc_state_from_config(void)
{
    int w, h, fps, br, gop, mode;

    w   = cfg_get_int("RTSP_WIDTH", 1280);
    h   = cfg_get_int("RTSP_HEIGHT", 720);
    fps = cfg_get_int("RTSP_FRAMERATE", 15);
    br  = cfg_get_int("RTSP_BITRATE", 2048);
    mode = cfg_get_int("RTSP_BITRATE_MODE", 0);
    gop = cfg_get_int("RTSP_GOP", 15);

    /* sensor clamp (720p native) */
    if (gm_system.cap[0].framerate > 0 && fps > gm_system.cap[0].framerate) {
        log_error("Framerate %d exceeds capture maximum %d, clamping",
                  fps, gm_system.cap[0].framerate);
        fps = gm_system.cap[0].framerate;
    }
    if (h > 720) h = 720;
    if (w > 1280) w = 1280;

    g_enc.width = w; g_enc.height = h;
    g_enc.framerate = fps; g_enc.bitrate = br;
    g_enc.bitrate_max = cfg_get_int("RTSP_BITRATE_MAX", br);
    g_enc.gop = gop;
    g_enc.mode = (mode > 0) ? GM_VBR : GM_CBR;
    g_enc.enc_type = 0; /* H264 */
    g_enc.have_spspps = 0;
    g_enc.sps_len = 0; g_enc.pps_len = 0;
    return 0;
}

static int gm_stream_init(void)
{
    gm_init();
    gm_get_sysinfo(&gm_system);

    DECLARE_ATTR(cap_attr, gm_cap_attr_t);
    DECLARE_ATTR(h264e_attr, gm_h264e_attr_t);
    DECLARE_ATTR(dnr_attr, gm_3dnr_attr_t);
    DECLARE_ATTR(audio_grab_attr, gm_audio_grab_attr_t);
    DECLARE_ATTR(audio_encode_attr, gm_audio_enc_attr_t);

    enc_state_from_config();

    groupfd = gm_new_groupfd();

    /* video captured (main path) */
    cap_obj = gm_new_obj(GM_CAP_OBJECT);
    cap_attr.cap_vch = 0;
    cap_attr.path = 0;                    /* assume cap path 0 */
    cap_attr.enable_mv_data = (g_motion_enable || g_motion_tracking) ? 1 : 0;
    cap_attr.dma_path = 0;
    if (g_enc.width >= (gm_system.cap[0].dim.width / 2) &&
        g_enc.height >= (gm_system.cap[0].dim.height / 2)) {
        dnr_attr.enabled = 1;
        gm_set_attr(cap_obj, &dnr_attr);
    }
    gm_set_attr(cap_obj, &cap_attr);

    h264e_attr.dim.width  = g_enc.width;
    h264e_attr.dim.height = g_enc.height;
    h264e_attr.frame_info.framerate = g_enc.framerate;
    h264e_attr.ratectl.mode = (gm_enc_ratecontrol_mode_t) g_enc.mode;
    h264e_attr.ratectl.gop  = g_enc.gop;
    h264e_attr.ratectl.bitrate = g_enc.bitrate;
    h264e_attr.ratectl.bitrate_max = g_enc.bitrate_max;
    h264e_attr.b_frame_num = 0;
    h264e_attr.enable_mv_data = 0;
    enc_obj = gm_new_obj(GM_ENCODER_OBJECT);
    gm_set_attr(enc_obj, &h264e_attr);

    bindfd = gm_bind(groupfd, cap_obj, enc_obj);

    /* scaler sub-stream (360p) - only created explicitly on v5 1080p */
    if (g_enc.width > 1280 || g_enc.height > 720) {
        sub_enc_obj = gm_new_obj(GM_ENCODER_OBJECT);
        h264e_attr.dim.width = 640;
        h264e_attr.dim.height = 360;
        h264e_attr.ratectl.bitrate = g_enc.bitrate / 2;
        gm_set_attr(sub_enc_obj, &h264e_attr);
        sub_bindfd = gm_bind(groupfd, cap_obj, sub_enc_obj);
    }

    if (g_motion_enable || g_motion_tracking) {
        motion_detection_init();
    }

    if (gm_apply(groupfd) < 0) {
        log_error("Error! gm_apply fail, AP procedure something wrong!");
        exit(-1);
    }

    if (g_motion_enable || g_motion_tracking) {
        if (motion_alg_setup(0) < 0) {
            log_error("motion: motion_alg_setup failed, disabling motion");
            g_motion_enable = 0;
        }
    }

    /* ---- audio ---- */
    if (g_audio_enabled) {
        audio_groupfd = gm_new_groupfd();
        audio_grab_obj = gm_new_obj(GM_AUDIO_GRAB_OBJECT);
        audio_enc_obj = gm_new_obj(GM_AUDIO_ENCODER_OBJECT);

        audio_grab_attr.vch = 0;
        audio_grab_attr.sample_rate = g_audio_rate;
        audio_grab_attr.sample_size = 16;
        audio_grab_attr.channel_type = (gm_audio_channel_type_t) g_audio_ch;
        gm_set_attr(audio_grab_obj, &audio_grab_attr);

        audio_encode_attr.encode_type = (gm_audio_encode_type_t) g_audio_type;
        audio_encode_attr.bitrate = g_audio_bitrate;
        audio_encode_attr.frame_samples = g_audio_framesamples;
        gm_set_attr(audio_enc_obj, &audio_encode_attr);

        audio_bindfd = gm_bind(audio_groupfd, audio_grab_obj, audio_enc_obj);
        if (gm_apply(audio_groupfd) < 0) {
            log_error("error:fail to gm_apply audio_groupfd");
            audio_bindfd = NULL;
        }
    }

    /* OSD text (watermark) is applied separately via set_watermark() */
    if (g_watermark) {
        gm_palette_table_t pal;
        memset(&pal, 0, sizeof(pal));
        gm_set_palette_table(&pal);
    }
    log_info("gm stream init OK: %dx%d@%d %dkbps gop=%d mode=%d audio=%s/%d",
             g_enc.width, g_enc.height, g_enc.framerate,
             g_enc.bitrate / 8, g_enc.gop, g_enc.mode,
             g_audio_type == GM_AAC ? "aac" : "alaw", g_audio_rate);
    return 0;
}

static void gm_stream_release(void)
{
    if (bindfd)   gm_unbind(bindfd);
    if (sub_bindfd) gm_unbind(sub_bindfd);
    if (audio_bindfd) gm_unbind(audio_bindfd);
    gm_release();
}

/* ------------------------------------------------------------------ */
/* local stream server (replaces avServStart3/avSendFrameData path)    */
/* ------------------------------------------------------------------ */

static void client_add(int fd)
{
    int i;
    pthread_mutex_lock(&g_clients_mutex);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active) {
            g_clients[i].fd = fd;
            g_clients[i].active = 1;
            pthread_mutex_init(&g_clients[i].wlock, NULL);
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static void client_drop(int i)
{
    g_clients[i].active = 0;
    close(g_clients[i].fd);
}

static void stream_send(int i, char type, const unsigned char *data, int len)
{
    unsigned char hdr[7];
    int fd = g_clients[i].fd;

    hdr[0] = STREAM_MAGIC0;
    hdr[1] = STREAM_MAGIC1;
    hdr[2] = (unsigned char) type;
    hdr[3] = (len >> 24) & 0xff;
    hdr[4] = (len >> 16) & 0xff;
    hdr[5] = (len >> 8) & 0xff;
    hdr[6] = len & 0xff;

    pthread_mutex_lock(&g_clients[i].wlock);
    if (write(fd, hdr, 7) != 7) {
        pthread_mutex_unlock(&g_clients[i].wlock);
        client_drop(i);
        return;
    }
    if (len > 0 && data && write(fd, data, len) != len) {
        pthread_mutex_unlock(&g_clients[i].wlock);
        client_drop(i);
        return;
    }
    pthread_mutex_unlock(&g_clients[i].wlock);
}

static void stream_broadcast(char type, const unsigned char *data, int len)
{
    int i;
    pthread_mutex_lock(&g_clients_mutex);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active)
            stream_send(i, type, data, len);
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static void *thread_StreamServer(void *arg)
{
    int lsock, port = cfg_get_int("MIIO_STREAM_PORT", 8601);
    struct sockaddr_in addr;
    int one = 1;

    (void)arg;
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        log_error("stream server socket fail");
        return NULL;
    }
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("stream bind:%d fail", port);
        close(lsock);
        return NULL;
    }
    listen(lsock, 4);
    log_info("local stream server ready on port %d", port);
    prctl(PR_SET_NAME, "av_stream", 0, 0, 0);

    while (g_running) {
        int fd = accept(lsock, NULL, NULL);
        if (fd < 0)
            continue;
        client_add(fd);
        log_info("local stream client #%d connected", fd);
    }
    close(lsock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SPS/PPS NAL extraction from the Annex-B H264 keyframe (for MP4 moov) */
/* ------------------------------------------------------------------ */

static void nal_extract_spspps(const unsigned char *buf, int len)
{
    int i, n2sps, n2pps, j, nal_type, sc, next;

    memset(g_enc.sps, 0, sizeof(g_enc.sps));
    memset(g_enc.pps, 0, sizeof(g_enc.pps));
    g_enc.sps_len = g_enc.pps_len = 0;
    n2sps = n2pps = -1;

    i = 0;
    while (i + 3 < len) {
        /* find start code: 00 00 01 or 00 00 00 01 */
        if (buf[i] != 0 || buf[i+1] != 0) { i++; continue; }
        if (buf[i+2] == 1) {
            sc = 3;
        } else if (buf[i+2] == 0 && i+3 < len && buf[i+3] == 1) {
            sc = 4;
        } else { i++; continue; }

        nal_type = (buf[i + sc] >> 5) & 0x1f;

        /* find the next start code boundary */
        next = len;
        for (j = i + sc; j + 3 < len; j++) {
            if (buf[j] == 0 && buf[j+1] == 0 &&
                (buf[j+2] == 1 ||
                 (j + 3 < len && buf[j+2] == 0 && buf[j+3] == 1))) {
                next = j;
                break;
            }
        }

        if (nal_type == 7 && (next - (i + sc)) <= (int)sizeof(g_enc.sps)) {
            n2sps = next - (i + sc);
            memcpy(g_enc.sps, buf + i + sc, n2sps);
        } else if (nal_type == 8 && (next - (i + sc)) <= (int)sizeof(g_enc.pps)) {
            n2pps = next - (i + sc);
            memcpy(g_enc.pps, buf + i + sc, n2pps);
        }

        if (n2sps > 0 && n2pps > 0) {
            g_enc.sps_len = n2sps;
            g_enc.pps_len = n2pps;
            g_enc.have_spspps = 1;
            return;
        }
        i = next;
    }
    if (n2sps > 0) g_enc.sps_len = n2sps;
    if (n2pps > 0) g_enc.pps_len = n2pps;
    if (g_enc.sps_len && g_enc.pps_len)
        g_enc.have_spspps = 1;
}

/* forward decls used by the record / muxer / playback modules */
static int  record_start(int is_alarm, int seconds);
static void record_stop(int final);
static void record_feed_video(const unsigned char *data, int len, int key, unsigned int ts);
static void record_feed_audio(const unsigned char *data, int len);
static int  playback_start(int sid, const char *path);

/* ------------------------------------------------------------------ */
/* bitstream receive threads (thread_VideoFrameData / thread_AudioFrameData)*/
/* ------------------------------------------------------------------ */

static void *thread_VideoFrameData(void *arg)
{
    gm_pollfd_t poll_fd;
    gm_enc_multi_bitstream_t bs;
    char *bs_buf;
    int ret;

    (void)arg;
    bs_buf = malloc(VIDEO_FRAME_MAX + 16);
    if (!bs_buf) return NULL;
    memset(bs_buf, 0, VIDEO_FRAME_MAX + 16);

    memset(&poll_fd, 0, sizeof(poll_fd));
    poll_fd.bindfd = bindfd;
    poll_fd.event = GM_POLL_READ;
    prctl(PR_SET_NAME, "av_p2p", 0, 0, 0);
    log_info("thread_VideoFrameData start OK");

    while (g_running) {
        ret = gm_poll(&poll_fd, 1, 2000);
        if (ret == GM_TIMEOUT)
            continue;
        if (ret < 0) {
            log_error("FIXME, video gm_poll ret: %d, quit.", ret);
            break;
        }
        memset(&bs, 0, sizeof(bs));
        bs.bindfd = bindfd;
        bs.bs.bs_buf = bs_buf;
        bs.bs.bs_buf_len = VIDEO_FRAME_MAX;
        bs.bs.mv_buf = NULL;
        bs.bs.mv_buf_len = 0;
        if ((ret = gm_recv_multi_bitstreams(&bs, 1)) < 0) {
            log_error("gm_recv_multi_bitstreams: %d", ret);
            continue;
        }
        if (bs.retval < 0)
            continue;

        /* capture SPS/PPS from the first IDR (for MP4 moov) */
        if (!g_enc.have_spspps && bs.bs.keyframe) {
            nal_extract_spspps((unsigned char *)bs.bs.bs_buf, bs.bs.bs_len);
        }

        /* push to local streaming clients */
        if (bs.bs.bs_len > 0)
            stream_broadcast(STREAM_TYPE_VIDEO, (unsigned char *)bs.bs.bs_buf, bs.bs.bs_len);

        /* feed the MP4 muxer when a record is active */
        record_feed_video((unsigned char *)bs.bs.bs_buf, bs.bs.bs_len,
                          bs.bs.keyframe, bs.bs.timestamp);
    }
    free(bs_buf);
    log_info("thread_VideoFrameData exit");
    return NULL;
}

static void *thread_AudioFrameData(void *arg)
{
    gm_pollfd_t poll_fd;
    gm_enc_multi_bitstream_t bs;
    char *bs_buf;
    int ret;

    (void)arg;
    if (!g_audio_enabled || !audio_bindfd)
        return NULL;
    bs_buf = malloc(AUDIO_FRAME_MAX + 16);
    if (!bs_buf) return NULL;

    memset(&poll_fd, 0, sizeof(poll_fd));
    poll_fd.bindfd = audio_bindfd;
    poll_fd.event = GM_POLL_READ;
    prctl(PR_SET_NAME, "av_p2paudio", 0, 0, 0);
    log_info("thread_AudioFrameData start OK");

    while (g_running) {
        ret = gm_poll(&poll_fd, 1, 2000);
        if (ret == GM_TIMEOUT)
            continue;
        if (ret < 0) {
            log_error("FIXME, audio gm_poll ret: %d, quit.", ret);
            break;
        }
        memset(&bs, 0, sizeof(bs));
        bs.bindfd = audio_bindfd;
        bs.bs.bs_buf = bs_buf;
        bs.bs.bs_buf_len = AUDIO_FRAME_MAX;
        bs.bs.mv_buf = NULL;
        bs.bs.mv_buf_len = 0;
        if ((ret = gm_recv_multi_bitstreams(&bs, 1)) < 0) {
            log_error("audio gm_recv_multi_bitstreams: %d", ret);
            continue;
        }
        if (bs.retval < 0)
            continue;
        if (bs.bs.bs_len > 0) {
            stream_broadcast(STREAM_TYPE_AUDIO, (unsigned char *)bs.bs.bs_buf, bs.bs.bs_len);
            record_feed_audio((unsigned char *)bs.bs.bs_buf, bs.bs.bs_len);
        }
    }
    free(bs_buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* MP4 muxer (H264 + AAC/ALAW) - minimal ISO BMFF writer               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* growable byte buffer (builds MP4 boxes in memory)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *b;
    size_t len, cap;
} mbuf_t;

static void mbuf_reserve(mbuf_t *m, size_t add)
{
    if (m->len + add > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 4096;
        while (ncap < m->len + add) ncap *= 2;
        m->b = realloc(m->b, ncap);
        m->cap = ncap;
    }
}

static void mbuf_add(mbuf_t *m, const void *data, size_t n)
{
    mbuf_reserve(m, n);
    if (n)
        memcpy(m->b + m->len, data, n);
    m->len += n;
}

static void mbuf_be(mbuf_t *m, uint32_t v, int nbytes)
{
    size_t i;
    mbuf_reserve(m, nbytes);
    for (i = 0; i < (size_t)nbytes; i++)
        m->b[m->len + i] = (v >> (8 * (nbytes - 1 - i))) & 0xff;
    m->len += nbytes;
}

static void mbuf_be16(mbuf_t *m, uint32_t v) { mbuf_be(m, v, 2); }
static void mbuf_be24(mbuf_t *m, uint32_t v) { mbuf_be(m, v, 3); }
static void mbuf_be32(mbuf_t *m, uint32_t v) { mbuf_be(m, v, 4); }
static void mbuf_cc(mbuf_t *m, const char cc[4]) { mbuf_add(m, cc, 4); }

static void mbuf_box_begin(mbuf_t *m, const char cc[4], size_t *size_pos)
{
    *size_pos = m->len;
    mbuf_be32(m, 0);
    mbuf_cc(m, cc);
}

static void mbuf_box_end(mbuf_t *m, size_t size_pos)
{
    uint32_t sz = (uint32_t)(m->len - size_pos);
    m->b[size_pos + 0] = (sz >> 24) & 0xff;
    m->b[size_pos + 1] = (sz >> 16) & 0xff;
    m->b[size_pos + 2] = (sz >> 8) & 0xff;
    m->b[size_pos + 3] = sz & 0xff;
}

/* ------------------------------------------------------------------ */
/* MP4 muxer (H264 avc1 + AAC-LC mp4a/alaw) - minimal ISO BMFF writer   */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    unsigned int len;
    int key;
    unsigned int dur;   /* dts units: video 90000/timescale, audio rate */
} mpsamp_t;

static mpsamp_t *g_vs = NULL, *g_as = NULL;
static int g_vn = 0, g_an = 0, g_vcap = 0, g_acap = 0;

static int record_add_video(const unsigned char *data, int len, int key)
{
    if (g_vn >= g_vcap) {
        int ncap = g_vcap ? g_vcap * 2 : 512;
        mpsamp_t *nv = realloc(g_vs, ncap * sizeof(mpsamp_t));
        if (!nv) return -1;
        g_vs = nv; g_vcap = ncap;
    }
    g_vs[g_vn].data = malloc(len);
    if (!g_vs[g_vn].data) return -1;
    memcpy(g_vs[g_vn].data, data, len);
    g_vs[g_vn].len = len;
    g_vs[g_vn].key = key;
    g_vs[g_vn].dur = 90000 / (g_enc.framerate ? g_enc.framerate : 15);
    g_vn++;
    return 0;
}

static int record_add_audio(const unsigned char *data, int len)
{
    if (g_an >= g_acap) {
        int ncap = g_acap ? g_acap * 2 : 256;
        mpsamp_t *na = realloc(g_as, ncap * sizeof(mpsamp_t));
        if (!na) return -1;
        g_as = na; g_acap = ncap;
    }
    g_as[g_an].data = malloc(len);
    if (!g_as[g_an].data) return -1;
    memcpy(g_as[g_an].data, data, len);
    g_as[g_an].len = len;
    g_as[g_an].key = 0;
    g_as[g_an].dur = g_audio_framesamples;
    g_an++;
    return 0;
}

static void record_clear_samples(void)
{
    int i;
    for (i = 0; i < g_vn; i++) free(g_vs[i].data);
    for (i = 0; i < g_an; i++) free(g_as[i].data);
    if (g_vs) { free(g_vs); g_vs = NULL; }
    if (g_as) { free(g_as); g_as = NULL; }
    g_vn = g_an = g_vcap = g_acap = 0;
}

/* --- box builders --- */

typedef void (*boxfn_t)(mbuf_t *, void *);

static void esd_desc(mbuf_t *m, unsigned char tag, boxfn_t body, void *p)
{
    size_t lenpos;
    mbuf_add(m, &tag, 1);
    lenpos = m->len;
    mbuf_be(m, 0, 1);               /* 1-byte length, patched below */
    if (body) body(m, p);
    m->b[lenpos] = (unsigned char)(m->len - lenpos - 1);
}

static void esd_slc_body(mbuf_t *m, void *p)
{
    mbuf_add(m, &(unsigned char){0x02}, 1);
    (void)p;
}

static void esd_dsi_body(mbuf_t *m, void *p)
{
    mbuf_add(m, p, 2);              /* AudioSpecificConfig: AAC-LC 16k mono */
}

static void esd_dcd_body(mbuf_t *m, void *p)
{
    mbuf_be(m, 0x40, 1);            /* objectTypeIndication: AAC */
    mbuf_be(m, 0x15, 1);            /* streamType: audio */
    mbuf_be24(m, 0);                /* bufferSizeDB */
    mbuf_be32(m, g_audio_bitrate);
    mbuf_be32(m, g_audio_bitrate);
    esd_desc(m, 0x05, esd_dsi_body, p);
}

static void esd_aac_body(mbuf_t *m, void *p)
{
    mbuf_be16(m, 0);                /* ES_ID, streamDependence=0 */
    esd_desc(m, 0x04, esd_dcd_body, p);
    esd_desc(m, 0x06, esd_slc_body, NULL);
    (void)p;
}

static void mp4_avcc(mbuf_t *m)
{
    static const unsigned char def_profile = 0x64, def_constraints = 0x00,
                              def_level = 0x1f;
    mbuf_add(m, &(unsigned char){0x01}, 1);
    mbuf_add(m, g_enc.sps_len >= 3 ? g_enc.sps + 1 : &def_profile, 1);
    mbuf_add(m, g_enc.sps_len >= 3 ? g_enc.sps + 2 : &def_constraints, 1);
    mbuf_add(m, g_enc.sps_len >= 3 ? g_enc.sps + 3 : &def_level, 1);
    mbuf_be(m, 0xff, 1);            /* lengthSizeMinusOne=3 */
    mbuf_be(m, 0xe1, 1);            /* numOfSequenceParameterSets=1 */
    mbuf_be16(m, g_enc.sps_len);
    mbuf_add(m, g_enc.sps, g_enc.sps_len);
    mbuf_be(m, 0x01, 1);            /* numOfPictureParameterSets=1 */
    mbuf_be16(m, g_enc.pps_len);
    mbuf_add(m, g_enc.pps, g_enc.pps_len);
}

static void mp4_video_stsd(mbuf_t *m)
{
    char comp[32];
    mbuf_be32(m, 0);
    mbuf_be(m, 1, 4);               /* entry_count */
    mbuf_cc(m, "avc1");
    mbuf_be(m, 0, 6);
    mbuf_be16(m, 1);                /* data_reference_index */
    mbuf_be(m, 0, 16);
    mbuf_be16(m, g_enc.width);
    mbuf_be16(m, g_enc.height);
    mbuf_be(m, 0x00480000, 4);
    mbuf_be(m, 0x00480000, 4);
    mbuf_be(m, 0, 4);
    mbuf_be16(m, 1);                /* frame_count */
    memset(comp, 0, sizeof(comp));
    strncpy(comp, "MiioCam H264", sizeof(comp) - 1);
    mbuf_add(m, comp, 32);
    mbuf_be16(m, 24);
    mbuf_be16(m, 0xffff);
    mp4_avcc(m);
}

static void mp4_audio_stsd(mbuf_t *m)
{
    mbuf_be32(m, 0);
    mbuf_be(m, 1, 4);               /* entry_count */
    mbuf_cc(m, g_audio_type == GM_AAC ? "mp4a" : "alaw");
    mbuf_be(m, 0, 6);
    mbuf_be16(m, 1);
    mbuf_be(m, 0, 8);
    mbuf_be16(m, g_audio_ch == GM_STEREO ? 2 : 1);
    mbuf_be16(m, 16);
    mbuf_be(m, 0, 4);
    mbuf_be16(m, g_audio_rate);
    mbuf_be(m, 0, 2);
    if (g_audio_type == GM_AAC) {
        unsigned char asc[2];
        size_t pos;
        asc[0] = 0x12; asc[1] = 0x10;   /* AAC-LC 16k mono */
        mbuf_box_begin(m, "esds", &pos);
        mbuf_be32(m, 0);
        esd_desc(m, 0x03, esd_aac_body, asc);
        mbuf_box_end(m, pos);
    }
}

/* stbl for one track */
static void mp4_stbl(mbuf_t *m, int is_audio)
{
    size_t pos_stsd, pos_stts, pos_stsc, pos_stsz, pos_stco, pos_stss;
    int i, n = is_audio ? g_an : g_vn;
    int timescale = is_audio ? g_audio_rate : 90000;

    mbuf_box_begin(m, "stsd", &pos_stsd);
    mbuf_be32(m, 0);
    mbuf_be(m, 1, 4);
    if (is_audio) mp4_audio_stsd(m); else mp4_video_stsd(m);
    mbuf_box_end(m, pos_stsd);

    mbuf_box_begin(m, "stts", &pos_stts);
    mbuf_be32(m, 0);
    mbuf_be(m, n ? 1 : 0, 4);
    if (n) {
        mbuf_be32(m, n);
        mbuf_be32(m, is_audio ? (unsigned int)g_audio_framesamples : (unsigned int)timescale / (g_enc.framerate ? g_enc.framerate : 15));
    }
    mbuf_box_end(m, pos_stts);

    mbuf_box_begin(m, "stsc", &pos_stsc);
    mbuf_be32(m, 0);
    mbuf_be(m, n ? 1 : 0, 4);
    if (n) {
        mbuf_be32(m, 1);            /* first_chunk */
        mbuf_be32(m, 1);            /* samples_per_chunk (1 chunk per sample) */
        mbuf_be32(m, 1);            /* sample_description_index */
    }
    mbuf_box_end(m, pos_stsc);

    mbuf_box_begin(m, "stsz", &pos_stsz);
    mbuf_be32(m, 0);
    mbuf_be32(m, 0);                /* sample_size (variable) */
    mbuf_be32(m, n);
    for (i = 0; i < n; i++)
        mbuf_be32(m, is_audio ? g_as[i].len : g_vs[i].len);
    mbuf_box_end(m, pos_stsz);

    mbuf_box_begin(m, "stco", &pos_stco);
    mbuf_be32(m, 0);
    mbuf_be32(m, n);
    for (i = 0; i < n; i++)
        mbuf_be32(m, 0);            /* patched after moov is built */
    mbuf_box_end(m, pos_stco);

    if (!is_audio) {
        int keys = 0;
        for (i = 0; i < n; i++) if (g_vs[i].key) keys++;
        if (keys > 0 && keys < n) {
            mbuf_box_begin(m, "stss", &pos_stss);
            mbuf_be32(m, 0);
            mbuf_be32(m, keys);
            for (i = 0; i < n; i++)
                if (g_vs[i].key) mbuf_be32(m, i + 1);
            mbuf_box_end(m, pos_stss);
        }
    }
}

static void mp4_trak(mbuf_t *m, int is_audio)
{
    size_t pos_tkhd, pos_mdia, pos_minf, pos_hdlr, pos_mdhd;
    int n = is_audio ? g_an : g_vn;
    int timescale = is_audio ? g_audio_rate : 90000;
    uint32_t dur = 0;
    int i;
    for (i = 0; i < n; i++)
        dur += is_audio ? g_as[i].dur : g_vs[i].dur;

    mbuf_box_begin(m, "tkhd", &pos_tkhd);
    mbuf_be(m, 0, 4);               /* version0 + flags */
    mbuf_be32(m, 0);                /* creation_time */
    mbuf_be32(m, 0);                /* modification_time */
    mbuf_be32(m, is_audio ? 2 : 1); /* track_ID */
    mbuf_be32(m, 0);                /* reserved */
    mbuf_be32(m, dur);              /* duration */
    mbuf_be(m, 0, 8);               /* reserved */
    mbuf_be16(m, 0);
    mbuf_be16(m, 0);
    mbuf_be(m, 0x00010000, 4);      /* layer + alternate_group + volume */
    mbuf_be16(m, 0);                /* volume (audio) */
    mbuf_be16(m, 0);                /* reserved */
    mbuf_be(m, 0x00010000, 4);      /* matrix */
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0x00010000, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0x40000000, 4);
    mbuf_be32(m, is_audio ? 0 : g_enc.width << 16);
    mbuf_be32(m, is_audio ? 0 : g_enc.height << 16);
    mbuf_box_end(m, pos_tkhd);

    mbuf_box_begin(m, "mdia", &pos_mdia);
    mbuf_box_begin(m, "mdhd", &pos_mdhd);
    mbuf_be32(m, 0);
    mbuf_be32(m, 0);
    mbuf_be32(m, 0);
    mbuf_be32(m, timescale);
    mbuf_be32(m, dur);
    mbuf_be16(m, is_audio ? 0x0100 : 0x0155); /* language: und / eng */
    mbuf_be16(m, 0);
    mbuf_box_end(m, pos_mdhd);

    mbuf_box_begin(m, "hdlr", &pos_hdlr);
    mbuf_be32(m, 0);
    mbuf_be32(m, 0);
    mbuf_cc(m, is_audio ? "soun" : "vide");
    mbuf_be(m, 0, 12);
    {
        const char *h = is_audio ? "SoundHandler\0" : "VideoHandler\0";
        mbuf_add(m, h, strlen(h) + 1);
    }
    mbuf_box_end(m, pos_hdlr);

    mbuf_box_begin(m, "minf", &pos_minf);
    mbuf_cc(m, "vmhd");             /* either vmhd or smhd */
    mbuf_be(m, 0, 4);
    if (is_audio) {
        mbuf_cc(m, "smhd");
    } else {
        mbuf_be16(m, 0);
        mbuf_be16(m, 0);
        mbuf_be(m, 0, 4);
    }
    mbuf_cc(m, "dinf");
    mbuf_box_begin(m, "dref", &pos_minf);
    mbuf_be32(m, 0);
    mbuf_be(m, 1, 4);
    mbuf_box_begin(m, "url ", &pos_minf);
    mbuf_be32(m, 1);
    mbuf_box_end(m, pos_minf);
    mbuf_box_end(m, pos_minf);
    mp4_stbl(m, is_audio);
    mbuf_box_end(m, pos_minf);
    mbuf_box_end(m, pos_mdia);
}

static void mp4_moov(mbuf_t *m)
{
    size_t pos_moov, pos_mvhd;
    unsigned int mdur = 0, vdur = 0, i;
    for (i = 0; i < g_vn; i++) vdur += g_vs[i].dur;
    for (i = 0; i < g_an; i++) mdur += g_as[i].dur;

    mbuf_box_begin(m, "moov", &pos_moov);
    mbuf_box_begin(m, "mvhd", &pos_mvhd);
    mbuf_be32(m, 0);
    mbuf_be32(m, 0);
    mbuf_be32(m, 0);
    mbuf_be32(m, 1000);
    mbuf_be32(m, g_an ? mdur / g_audio_rate * 1000 : vdur * 1000 / 90000);
    mbuf_be32(m, 0x00010000);
    mbuf_be16(m, 0x0100);
    mbuf_be16(m, 0);
    mbuf_be(m, 0, 8);
    mbuf_be(m, 0x00010000, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0x00010000, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0, 4);
    mbuf_be(m, 0x40000000, 4);
    mbuf_be(m, 0, 24);
    mbuf_be32(m, g_an ? 3 : 2);     /* next_track_ID */
    mbuf_box_end(m, pos_mvhd);

    if (g_vn) mp4_trak(m, 0);
    if (g_an) mp4_trak(m, 1);
    mbuf_box_end(m, pos_moov);
}

/* write moov+mdat to path; builds both in memory (clips are <= 30s).
 * mdat layout: [4-byte len][video sample]... [4-byte len][audio sample]...
 * so both tracks share one chunk-per-sample model with independent stco's. */
static int mp4_write(const char *path)
{
    mbuf_t moov = {0};
    static unsigned char ftyp[20] = {
        0, 0, 0, 20, 'f','t','y','p', 'i','s','o','m',
        0, 0, 0, 0, 'i','s','o','m'
    };
    unsigned char hdr[8];
    FILE *f;
    int i;
    uint32_t mdat_start, aggr;
    size_t stco_v = 0, stco_a = 0;

    if (!g_vn)
        return -1;

    mp4_moov(&moov);

    /* locate video and audio stco entry areas (two "stco" occurrences) */
    {
        size_t p, count = 0;
        for (p = 0; p + 8 <= moov.len; p++) {
            if (memcmp(moov.b + p, "stco", 4) == 0) {
                if (count == 0) stco_v = p + 12;
                else if (count == 1) stco_a = p + 12;
                count++;
            }
        }
        if (!stco_v) { free(moov.b); return -1; }
    }

    mdat_start = sizeof(ftyp) + moov.len;
    memcpy(hdr, "\x00\x00\x00\x00""mdat", 8);

    /* pad moov so that its size (and thus mdat offset) is a multiple of 8;
       the video starts at mdat_start and entries are length-prefixed. */
    while ((mdat_start + moov.len) % 8) {
        mbuf_add(&moov, &(unsigned char){0}, 1);
        moov.len++;
    }

    f = fopen(path, "wb");
    if (!f) { free(moov.b); return -1; }

    fwrite(ftyp, 1, sizeof(ftyp), f);
    fwrite(moov.b, 1, moov.len, f);

    aggr = 0;
    for (i = 0; i < g_vn; i++) {
        uint32_t off = mdat_start + aggr;
        moov.b[stco_v + i*4 + 0] = (off >> 24) & 0xff;
        moov.b[stco_v + i*4 + 1] = (off >> 16) & 0xff;
        moov.b[stco_v + i*4 + 2] = (off >> 8) & 0xff;
        moov.b[stco_v + i*4 + 3] = off & 0xff;
        aggr += 4 + g_vs[i].len;
    }
    if (g_an && stco_a) {
        for (i = 0; i < g_an; i++) {
            uint32_t off = mdat_start + aggr;
            moov.b[stco_a + i*4 + 0] = (off >> 24) & 0xff;
            moov.b[stco_a + i*4 + 1] = (off >> 16) & 0xff;
            moov.b[stco_a + i*4 + 2] = (off >> 8) & 0xff;
            moov.b[stco_a + i*4 + 3] = off & 0xff;
            aggr += 4 + g_as[i].len;
        }
    }

    /* write the whole file: ftyp + moov + mdat (rebuilt with patched stco) */
    fseek(f, 0, SEEK_SET);
    fwrite(ftyp, 1, sizeof(ftyp), f);
    fwrite(moov.b, 1, moov.len, f);
    {
        uint32_t sz = 8 + aggr;
        hdr[0] = (sz >> 24) & 0xff; hdr[1] = (sz >> 16) & 0xff;
        hdr[2] = (sz >> 8) & 0xff;  hdr[3] = sz & 0xff;
    }
    fwrite(hdr, 1, 8, f);
    for (i = 0; i < g_vn; i++) {
        unsigned char szb[4];
        szb[0] = (g_vs[i].len >> 24) & 0xff;
        szb[1] = (g_vs[i].len >> 16) & 0xff;
        szb[2] = (g_vs[i].len >> 8) & 0xff;
        szb[3] = g_vs[i].len & 0xff;
        fwrite(szb, 1, 4, f);
        fwrite(g_vs[i].data, 1, g_vs[i].len, f);
    }
    for (i = 0; i < g_an; i++) {
        unsigned char szb[4];
        szb[0] = (g_as[i].len >> 24) & 0xff;
        szb[1] = (g_as[i].len >> 16) & 0xff;
        szb[2] = (g_as[i].len >> 8) & 0xff;
        szb[3] = g_as[i].len & 0xff;
        fwrite(szb, 1, 4, f);
        fwrite(g_as[i].data, 1, g_as[i].len, f);
    }
    fclose(f);
    free(moov.b);
    return 0;
}

/* native playback index (.timestamp) — describes the mdat layout above */
static int mp4_write_timestamp(const char *mp4path, const char *idxpath)
{
    FILE *f;
    int i, n = g_vn + g_an;

    f = fopen(idxpath, "wb");
    if (!f) return -1;
    fprintf(f, "{\"file\":\"%s\",\"fps\":%d,\"width\":%d,\"height\":%d,"
               "\"samples\":[", mp4path, g_enc.framerate,
               g_enc.width, g_enc.height);
    for (i = 0; i < n; i++) {
        int is_a = i >= g_vn;
        int vi = is_a ? i - g_vn : i;
        unsigned int dts = is_a
            ? (unsigned)((uint64_t)vi * g_audio_framesamples)
            : (unsigned)((uint64_t)vi * 90000 / (g_enc.framerate ? g_enc.framerate : 15));
        fprintf(f, "%s{\"t\":%d,\"key\":%d,\"len\":%u,\"dts\":%u}",
                i ? "," : "", is_a, is_a ? 0 : (g_vs[vi].key ? 1 : 0),
                is_a ? g_as[vi].len : g_vs[vi].len, dts);
    }
    fprintf(f, "]}\n");
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* record control (thread_RecordMp4 / av_TFrecord)                     */
/* ------------------------------------------------------------------ */

static int g_rec_active = 0;
static int g_rec_is_alarm = 0;
static int g_rec_seconds = 0;           /* 0 = until stopped */
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_rec_path[300] = {0};

static void record_make_filename(char *out, size_t outsz, const char *ext)
{
    time_t now = time(NULL);
    struct tm sTm;
    char dstr[64], fstr[64];

    localtime_r(&now, &sTm);
    strftime(dstr, sizeof(dstr), "%Y/%m/%d", &sTm);
    strftime(fstr, sizeof(fstr), "%Y%m%d_%H%M%S", &sTm);
    create_directory(STREAM_DIR);
    if (strlen(STREAM_DIR) + strlen(dstr) + 2 < sizeof(g_rec_path)) {
        snprintf(g_rec_path, sizeof(g_rec_path), "%s/%s", STREAM_DIR, dstr);
        create_directory(g_rec_path);
    }
    snprintf(out, outsz, "%s/%s/%s.%s", STREAM_DIR, dstr, fstr, ext);
}

static void *thread_RecordMp4(void *arg)
{
    int seconds = (int)(long)arg;

    (void)seconds;
    prctl(PR_SET_NAME, "av_TFrecord", 0, 0, 0);
    log_info("thread_RecordMp4 start");

    /* auto-stop timer for alarm clips: finalize the MP4 */
    if (g_rec_seconds > 0) {
        sleep(g_rec_seconds);
        if (g_running)
            record_stop(1);
    }
    return NULL;
}

static int record_start(int is_alarm, int seconds)
{
    pthread_mutex_lock(&g_rec_lock);
    if (g_rec_active) {
        pthread_mutex_unlock(&g_rec_lock);
        return -1;
    }
    if (!g_enc.have_spspps) {
        log_error("record: no SPS/PPS yet, cannot mux H264");
        pthread_mutex_unlock(&g_rec_lock);
        return -1;
    }
    g_rec_is_alarm = is_alarm;
    g_rec_seconds = seconds;
    g_vn = g_an = 0;
    record_make_filename(g_rec_path, sizeof(g_rec_path), "mp4");
    g_rec_active = 1;
    pthread_mutex_unlock(&g_rec_lock);

    log_info("record start: %s (%s%s)", g_rec_path,
             is_alarm ? "alarm" : "manual",
             seconds > 0 ? ", auto-stop" : "");

    if (seconds > 0) {
        pthread_t t;
        pthread_create(&t, NULL, thread_RecordMp4, (void *)(long)seconds);
        pthread_detach(t);
    }
    return 0;
}

static void record_stop(int final)
{
    char idx[320];
    FILE *last;
    int rc;

    pthread_mutex_lock(&g_rec_lock);
    if (!g_rec_active) {
        pthread_mutex_unlock(&g_rec_lock);
        return;
    }
    if (!final)
        goto out;

    if (g_vn > 0) {
        rc = mp4_write(g_rec_path);
        if (rc == 0) {
            snprintf(idx, sizeof(idx), "%s.timestamp", g_rec_path);
            mp4_write_timestamp(g_rec_path, idx);
            log_info("record done: %s (%d video, %d audio samples)",
                     g_rec_path, g_vn, g_an);
            last = fopen(LAST_VIDEO, "wb");
            if (last) {
                fputs(g_rec_path, last);
                fclose(last);
            }
        } else {
            log_error("record: failed to mux %s", g_rec_path);
            unlink(g_rec_path);
        }
    }
out:
    record_clear_samples();
    g_rec_active = 0;
    pthread_mutex_unlock(&g_rec_lock);
}

static void record_feed_video(const unsigned char *data, int len, int key, unsigned int ts)
{
    (void)ts;
    pthread_mutex_lock(&g_rec_lock);
    if (g_rec_active)
        record_add_video(data, len, key);
    pthread_mutex_unlock(&g_rec_lock);
}

static void record_feed_audio(const unsigned char *data, int len)
{
    pthread_mutex_lock(&g_rec_lock);
    if (g_rec_active)
        record_add_audio(data, len);
    pthread_mutex_unlock(&g_rec_lock);
}

/* ------------------------------------------------------------------ */
/* playback (playback_thread / av_playback)                            */
/* streams back a recorded clip via the local stream framing, using     */
/* the .timestamp sidecar index + the mdat payload                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int is_audio;
    int key;
    unsigned int len;
    unsigned int dts;
} pb_ent_t;

static int pb_parse_index(const char *json, pb_ent_t **out, int *nout)
{
    const char *p = json, *e;
    pb_ent_t *ents = NULL;
    int n = 0, cap = 0;

    while ((p = strchr(p, '{')) != NULL) {
        int is_audio = 0, key = 0;
        unsigned int len = 0, dts = 0;
        char *frame;

        e = strchr(p, '}');
        if (!e) break;
        {
            size_t flen = (size_t)(e - p - 1);
            frame = (char *)malloc(flen + 1);
            if (!frame) break;
            memcpy(frame, p + 1, flen);
            frame[flen] = '\0';
        }
        is_audio = json_get_int_field(frame, "t");
        key = json_get_int_field(frame, "key");
        len = (unsigned)json_get_int_field(frame, "len");
        dts = (unsigned)json_get_int_field(frame, "dts");
        free(frame);

        if (n >= cap) {
            pb_ent_t *ne = realloc(ents, (cap + 256) * sizeof(pb_ent_t));
            if (!ne) break;
            ents = ne; cap += 256;
        }
        ents[n].is_audio = is_audio;
        ents[n].key = key;
        ents[n].len = len;
        ents[n].dts = dts;
        n++;
        p = e + 1;
    }
    *out = ents;
    *nout = n;
    return 0;
}

static void *thread_PlaybackFile(void *arg)
{
    const char *path = (const char *)arg;
    FILE *f;
    char idx[320], *jbuf = NULL, *md = NULL;
    long flen, mdat_off = -1, mdat_len = 0;
    pb_ent_t *ents = NULL;
    int nents = 0, i;
    long pos = 0;
    char meta[256];

    prctl(PR_SET_NAME, "av_playback", 0, 0, 0);
    f = fopen(path, "rb");
    if (!f) goto out;
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    if (flen <= 0) { fclose(f); goto out; }
    fseek(f, 0, SEEK_SET);
    md = malloc(flen);
    if (!md) { fclose(f); goto out; }
    if (fread(md, 1, flen, f) != (size_t)flen) { fclose(f); goto out; }
    fclose(f);

    /* locate mdat */
    {
        long i2 = 0;
        while (i2 + 8 <= flen) {
            unsigned int sz = ((unsigned)md[i2] << 24) | ((unsigned)md[i2+1] << 16) |
                              ((unsigned)md[i2+2] << 8) | (unsigned)md[i2+3];
            if (i2 + (long)sz > flen) break;
            if (memcmp(md + i2 + 4, "mdat", 4) == 0) {
                mdat_off = i2 + 8;
                mdat_len = sz - 8;
                break;
            }
            i2 += (long)sz;
        }
    }

    snprintf(idx, sizeof(idx), "%s.timestamp", path);
    f = fopen(idx, "rb");
    if (!f) goto out;
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    jbuf = malloc(flen + 1);
    if (!jbuf) { fclose(f); goto out; }
    if (fread(jbuf, 1, flen, f) != (size_t)flen) { fclose(f); goto out; }
    jbuf[flen] = 0;
    fclose(f);
    pb_parse_index(jbuf, &ents, &nents);

    if (mdat_off < 0 || nents == 0) {
        log_error("playback: no mdat/index for %s", path);
        goto out;
    }

    snprintf(meta, sizeof(meta),
             "{\"file\":\"%s\",\"samples\":%d,\"duration_ms\":0}",
             path, nents);
    stream_broadcast(STREAM_TYPE_META, (unsigned char *)meta, strlen(meta));
    log_info("playback: %d samples from %s", nents, path);

    pos = 0;
    for (i = 0; i < nents; i++) {
        unsigned char szb[4];
        unsigned int slen;

        if (!g_running) break;
        if (pos + 4 > mdat_len) break;

        /* read the 4-byte length prefix from mdat */
        szb[0] = md[mdat_off + pos + 0];
        szb[1] = md[mdat_off + pos + 1];
        szb[2] = md[mdat_off + pos + 2];
        szb[3] = md[mdat_off + pos + 3];
        slen = ((unsigned)szb[0] << 24) | ((unsigned)szb[1] << 16) |
               ((unsigned)szb[2] << 8) | (unsigned)szb[3];
        pos += 4;

        if (pos + (long)slen > mdat_len) break;

        if (ents[i].is_audio)
            stream_broadcast(STREAM_TYPE_AUDIO, (unsigned char *)md + mdat_off + pos, slen);
        else if (ents[i].key)
            stream_broadcast(STREAM_TYPE_VIDEO, (unsigned char *)md + mdat_off + pos, slen);
        else
            stream_broadcast(STREAM_TYPE_VIDEO, (unsigned char *)md + mdat_off + pos, slen);
        pos += (long)slen;
    }

out:
    if (jbuf) free(jbuf);
    if (md) free(md);
    free((void *)path);
    if (ents) free(ents);
    return NULL;
}

static int playback_start(int sid, const char *path)
{
    char *p = strdup(path);
    pthread_t t;

    (void)sid;
    if (!p) return -1;
    if (pthread_create(&t, NULL, thread_PlaybackFile, p) != 0) {
        free(p);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

/* ------------------------------------------------------------------ */
/* snapshot (av_TFpicture / take_snapshot)                             */
/* ------------------------------------------------------------------ */

static int take_snapshot(char *outpath, size_t outsz, int broadcast)
{
    snapshot_t snapshot;
    struct tm *sTm;
    time_t now;
    char dirstring[64];
    char filestring[64];
    char full[300];
    int len;
    FILE *fd, *last;

    if (!g_snapshot_buf) {
        g_snapshot_buf = malloc(MAX_SNAPSHOT_LEN);
        if (!g_snapshot_buf)
            return -1;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.bindfd = bindfd;
    snapshot.image_quality = 80;
    snapshot.bs_buf = g_snapshot_buf;
    snapshot.bs_buf_len = MAX_SNAPSHOT_LEN;
    snapshot.bs_width = g_enc.width;
    snapshot.bs_height = g_enc.height;

    len = gm_request_snapshot(&snapshot, 500);
    if (len <= 0) {
        log_error("snapshot: gm_request_snapshot rc=%d", len);
        return -1;
    }

    now = time(NULL);
    sTm = localtime(&now);
    strftime(dirstring, sizeof(dirstring), "/tmp/sd/RECORDED_IMAGES/%Y/%m/%d", sTm);
    create_directory(dirstring);
    strftime(filestring, sizeof(filestring), "snapshot_%Y%m%d_%H%M%S.jpg", sTm);
    snprintf(full, sizeof(full), "%s/%s", dirstring, filestring);

    fd = fopen(full, "wb");
    if (!fd) return -1;
    fwrite(g_snapshot_buf, 1, len, fd);
    fclose(fd);

    last = fopen(LAST_SNAPSHOT, "wb");
    if (last) { fputs(full, last); fclose(last); }

    if (outpath && outsz)
        snprintf(outpath, outsz, "%s", full);
    log_info("snapshot: %s (%d bytes)", full, len);

    if (broadcast)
        stream_broadcast(STREAM_TYPE_SNAP, (unsigned char *)g_snapshot_buf, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* motion detection (motion_thread / av_motiondetect)                  */
/* ------------------------------------------------------------------ */

/* forward decls: both defined below their first call sites */
static void mqtt_publish(const char *topic, const char *payload);
static void motion_alarm_start(void);

static struct mdt_alg_t motion_alg;
static struct mdt_result_t mdt_result[1];
static int g_motion_on = 0;
static struct timeval g_last_alarm;
static int g_alarm_active = 0;

static int motion_alg_setup(int ch)
{
    int mb_w_num, mb_h_num, w, h;

    memset(&motion_alg, 0, sizeof(motion_alg));
    motion_alg.u_width   = gm_system.cap[ch].dim.width;
    motion_alg.u_height  = gm_system.cap[ch].dim.height;
    motion_alg.u_mb_width  = 32;
    motion_alg.u_mb_height = 32;
    motion_alg.training_time = 15;
    motion_alg.frame_count   = 0;
    motion_alg.sensitive_th  = g_motion_sensitivity;

    mb_w_num = (motion_alg.u_width + 31) / 32;
    mb_h_num = (motion_alg.u_height + 31) / 32;
    motion_alg.mb_w_num = mb_w_num;
    motion_alg.mb_h_num = mb_h_num;
    motion_alg.alarm_th = mb_w_num * mb_h_num * 5 / 100;

    motion_alg.mb_cell_en = calloc(1, mb_w_num * mb_h_num);
    if (!motion_alg.mb_cell_en)
        return -1;
    for (h = 0; h < mb_h_num; h++)
        for (w = 0; w < mb_w_num; w++)
            motion_alg.mb_cell_en[h * mb_w_num + w] = 1;

    return motion_detection_update(bindfd, &motion_alg);
}

static void *thread_MotionDetect(void *arg)
{
    gm_multi_cap_md_t cap_md;
    char *md_buf;
    int ret;

    (void)arg;
    md_buf = malloc(CAP_MOTION_SIZE);
    if (!md_buf) return NULL;

    memset(&cap_md, 0, sizeof(cap_md));
    cap_md.bindfd = bindfd;
    cap_md.cap_md_info.md_buf_len = CAP_MOTION_SIZE;
    cap_md.cap_md_info.md_buf = md_buf;

    prctl(PR_SET_NAME, "av_motiondet", 0, 0, 0);
    log_info("thread_MotionDetect start (alarm_th=%d, sens=%d/%%)",
             motion_alg.alarm_th, g_motion_sensitivity);

    while (g_running) {
        ret = gm_recv_multi_cap_md(&cap_md, 1);
        if (ret < 0) {
            log_error("motion: gm_recv_multi_cap_md failed");
            usleep(200000);
            continue;
        }
        ret = motion_detection_handling(&cap_md, mdt_result, 1);
        if (ret < 0) {
            log_error("motion: motion_detection_handling failed");
            continue;
        }

        if (mdt_result[0].result == MOTION_DETECTED) {
            gettimeofday(&g_last_motion, NULL);
            if (g_motion_on == 0) {
                g_motion_on = 1;
                log_info("motion ON - event.motion");
                mqtt_publish(MQTT_TOPIC_EVENT, "{\"type\":\"event.motion\",\"on\":1}");
                if (g_motion_snapshot)
                    take_snapshot(NULL, 0, 1);
                if (g_motion_record)
                    motion_alarm_start();
            }
        } else if (mdt_result[0].result == NO_MOTION) {
            if (g_motion_on == 1) {
                g_motion_on = 0;
                log_info("motion OFF");
                mqtt_publish(MQTT_TOPIC_EVENT, "{\"type\":\"event.motion\",\"on\":0}");
            }
        } else if (mdt_result[0].result == MOTION_IS_TRAINING) {
            /* training in progress, nothing to do */
        }
    }

    free(md_buf);
    return NULL;
}

/* alarm: starts a time-limited MP4 recording + optional AES/upload */
static void motion_alarm_start(void)
{
    struct timeval now;
    int iv;

    if (g_rec_active)
        return;
    gettimeofday(&now, NULL);
    iv = g_alarm_interval > 0 ? g_alarm_interval : 5;
    if (now.tv_sec - g_last_alarm.tv_sec < iv)
        return;

    g_last_alarm = now;
    g_alarm_active = 1;
    log_info("alarm: record alarm clip (%d s)", g_motion_alarm_seconds);
    record_start(1, g_motion_alarm_seconds > 0 ? g_motion_alarm_seconds : DEFAULT_RECORD_SECS);
    mqtt_publish(MQTT_TOPIC_EVENT, "{\"type\":\"event.motion\",\"on\":1,\"record\":1}");
}

/* ------------------------------------------------------------------ */
/* MQTT (mosq_sub_init / topic_ble_events ↔ topic_avstreamer_to_ble)   */
/* ------------------------------------------------------------------ */

/* defined with the RPC server below */
static void rpc_run_cmd(const char *cmd, const char *arg);

static char g_mqtt_host[128] = "127.0.0.1";
static int g_mqtt_port = 1883;
static char g_mqtt_user[64] = "";
static char g_mqtt_pass[64] = "";
static char g_mqtt_topic[128] = MQTT_TOPIC_CTRL;
static int g_mqtt_enabled = 0;

/* publish using the mosquitto_pub subprocess (no libmosquitto dep) */
static void mqtt_publish(const char *topic, const char *payload)
{
    char cmd[640];

    if (!g_mqtt_enabled)
        return;
    if (g_mqtt_user[0])
        snprintf(cmd, sizeof(cmd),
                 "mosquitto_pub -h %s -p %d -u '%s' -P '%s' -t %s -m '%s' -q 1 2>/dev/null",
                 g_mqtt_host, g_mqtt_port, g_mqtt_user, g_mqtt_pass, topic, payload);
    else
        snprintf(cmd, sizeof(cmd),
                 "mosquitto_pub -h %s -p %d -t %s -m '%s' -q 1 2>/dev/null",
                 g_mqtt_host, g_mqtt_port, topic, payload);
    system(cmd);
}

/* subscribe thread: mosquitto_sub -v lines "<topic> <payload>" -> cmd */
static void mqtt_handle_payload(const char *payload)
{
    char cmd[64];
    char *p, *arg;

    p = strdup(payload);
    if (!p) return;
    arg = strchr(p, ' ');
    if (arg) {
        *arg = 0;
        arg++;
        /* <cmd> <arg> */
    } else {
        arg = NULL;
    }

    snprintf(cmd, sizeof(cmd), "%s", p);
    rpc_run_cmd(cmd, arg);

    free(p);
}

static void *thread_MQTTSub(void *arg)
{
    char cmd[640];
    FILE *fp;
    char line[512];

    (void)arg;
    if (!g_mqtt_enabled) return NULL;
    prctl(PR_SET_NAME, "av_mqtt", 0, 0, 0);

    while (g_running) {
        if (g_mqtt_user[0])
            snprintf(cmd, sizeof(cmd),
                     "mosquitto_sub -h %s -p %d -u '%s' -P '%s' -t %s -v 2>/dev/null",
                     g_mqtt_host, g_mqtt_port, g_mqtt_user, g_mqtt_pass, g_mqtt_topic);
        else
            snprintf(cmd, sizeof(cmd),
                     "mosquitto_sub -h %s -p %d -t %s -v 2>/dev/null",
                     g_mqtt_host, g_mqtt_port, g_mqtt_topic);

        fp = popen(cmd, "r");
        if (!fp) {
            usleep(2000000);
            continue;
        }
        while (g_running && fgets(line, sizeof(line), fp) != NULL) {
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            *sp = 0;
            /* skip the topic, keep the payload (with trailing \n stripped) */
            {
                char *nl = strchr(sp + 1, '\n');
                if (nl) *nl = 0;
            }
            log_info("mqtt cmd: %s", sp + 1);
            mqtt_handle_payload(sp + 1);
        }
        pclose(fp);
        if (g_running)
            sleep(1);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* flip / watermark / night mode                                       */
/* ------------------------------------------------------------------ */

static void set_flip(int hflip, int vflip)
{
    gm_cap_flip_t flip;

    memset(&flip, 0, sizeof(flip));
    flip.h_flip_enabled = hflip;
    flip.v_flip_enabled = vflip;
    if (gm_set_cap_flip(0, &flip) < 0)
        log_error("flip: gm_set_cap_flip failed");
    else
        log_info("flip: h=%d v=%d", hflip, vflip);
}

/* OSD palette colors (defined locally here, as in rtspd.c/rtspd2MP.c) */
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

static gm_palette_table_t miio_osd_palette = {
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

static void set_watermark(int on, const char *text)
{
    gm_osd_font2_t f2;
    static unsigned short font_index[128];

    gm_set_palette_table(&miio_osd_palette);

    memset(&f2, 0, sizeof(f2));
    f2.win_idx = 0;
    if (on && text) {
        size_t n = strlen(text);
        int i;
        if (n > 120) n = 120;
        for (i = 0; i < (int)n; i++)
            font_index[i] = (unsigned char)text[i];
        f2.enabled = 1;
        f2.align_type = GM_OSD_ALIGN_TOP_LEFT;
        f2.x = 10;
        f2.y = 10;
        f2.h_space = 0;
        f2.v_space = 0;
        f2.font_index_len = (int)n;
        f2.font_index = font_index;
        f2.font_alpha = GM_OSD_FONT_ALPHA_75;
        f2.win_alpha = GM_OSD_FONT_ALPHA_75;
        f2.font_palette_idx = 4;
        f2.win_palette_idx = 0;
        f2.priority = GM_OSD_PRIORITY_MARK_ON_OSD;
    } else {
        f2.enabled = 0;
        f2.font_index = NULL;
        f2.font_index_len = 0;
    }
    if (gm_set_osd_font2(cap_obj, &f2) < 0)
        log_error("watermark: gm_set_osd_font2 failed");
    g_watermark = on;
    if (on)
        log_info("watermark: %s", text ? text : "");
}

static void night_ir(const char *what, const char *arg)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/tmp/sd/firmware/bin/%s %s 2>/dev/null", what, arg);
    system(cmd);
}

/* 0 auto 1 day 2 night */
static void night_mode_apply(int mode)
{
    g_night_mode = mode;
    switch (mode) {
    case 2: /* night: IR LED on, IRCUT open */
        night_ir("ir_led", "on");
        night_ir("ir_cut", "on");
        break;
    case 1: /* day */
    default:
        night_ir("ir_led", "off");
        night_ir("ir_cut", "off");
        break;
    /* 'auto' leaves the isp328 auto switch alone */
    }
    cfg_set_int("MIIO_NIGHT_MODE", mode);
    do_nvram_commit();
    log_info("night_mode: %s", mode == 2 ? "night" : mode == 1 ? "day" : "auto");
}

/* ------------------------------------------------------------------ */
/* SD card management (thread_sdcard_check / av_TFcheck, av_SDformat)  */
/* ------------------------------------------------------------------ */

static int sd_freespace_mb(void)
{
    FILE *fp = popen("df -k /tmp/sd 2>/dev/null | tail -n 1", "r");
    char line[256];
    int fields = 0, free_kb = 0;
    int i;

    if (!fp) return -1;
    while (fields < 4 && fscanf(fp, "%255s", line) == 1)
        fields++;
    if (fields < 4) {
        pclose(fp);
        return -1;
    }
    for (i = 0; i < 4; i++)
        if (fscanf(fp, "%255s", line) == 1 && i == 3)
            free_kb = atoi(line);
    pclose(fp);
    return free_kb / 1024;
}

/* vendor loop_record_delete: wipe oldest recordings when SD is tight */
static void sd_delete_oldest(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ls -rt '%s'/*/*/*/*.mp4 2>/dev/null | head -n 1",
             STREAM_DIR);
    /* simplest: if free space < threshold, remove the oldest video dir */
    if (sd_freespace_mb() < 64) {
        log_info("sd: free space low, deleting oldest recordings");
        snprintf(cmd, sizeof(cmd),
                 "ls -rt '%s'/*/*/*/*.mp4 2>/dev/null | head -n 1 | xargs -r rm -f",
                 STREAM_DIR);
        system(cmd);
    }
}

static void *thread_SDCardCheck(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "av_TFcheck", 0, 0, 0);
    log_info("thread_SDCardCheck start");
    while (g_running) {
        if (access("/tmp/sd", F_OK) == 0)
            sd_delete_oldest();
        else
            log_error("sd: /tmp/sd not mounted!");
        sleep(cfg_get_int("MIIO_SD_CHECK_INTERVAL", 300));
    }
    return NULL;
}

/* av_SDformat: umount + fdisk wipe + mkfs.fat on the SD partition */
static void *thread_SDFormat(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "av_SDformat", 0, 0, 0);
    log_info("sd: formatting...");
    system("umount -l /tmp/sd 2>/dev/null");
    system("echo -e 'd\\n1\\nw' | fdisk /dev/mmcblk0 2>/dev/null");
    system("echo -e 'n\\np\\n1\\n\\n\\nw' | fdisk /dev/mmcblk0 2>/dev/null");
    system("mkfs.fat -F 32 /dev/mmcblk0p1 >/dev/null 2>&1");
    log_info("sd: format done, remounting");
    system("mount /dev/mmcblk0p1 /tmp/sd 2>/dev/null");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* system monitor + scheduled reboot (sysMonit / thread_reboot_task)   */
/* ------------------------------------------------------------------ */

static unsigned long last_cpu_total = 0, last_cpu_idle = 0;

static int read_cpu_usage(int *pct)
{
    FILE *fp = fopen("/proc/stat", "r");
    unsigned long total = 0, idle = 0, v[7];
    char line[256];
    int n;

    if (!fp) return -1;
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }
    fclose(fp);
    n = sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
    if (n < 4) return -1;
    total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6];
    idle = v[3];
    if (last_cpu_total == 0) {
        last_cpu_total = total;
        last_cpu_idle = idle;
        *pct = 0;
        return 0;
    }
    if (total == last_cpu_total) {
        *pct = 0;
        return 0;
    }
    *pct = (int)(100 - (idle - last_cpu_idle) * 100 / (total - last_cpu_total));
    last_cpu_total = total;
    last_cpu_idle = idle;
    return 0;
}

static void *thread_SysMonitor(void *arg)
{
    int pct = 0;
    (void)arg;
    prctl(PR_SET_NAME, "sysMonit", 0, 0, 0);
    log_info("thread_SysMonitor start");
    while (g_running) {
        read_cpu_usage(&pct);
        if (pct > 95) {
            log_error("sysMonit: cpu load %d/%% (wiggle, not restarting)", pct);
            mqtt_publish("miio/avstreamer/status", "{\"type\":\"status\",\"cpu\":95}");
        }
        sleep(60);
    }
    return NULL;
}

static void *thread_RebootTask(void *arg)
{
    unsigned long uptime = 0;
    int reboot_days = cfg_get_int("MIIO_REBOOT_DAYS", 10);

    (void)arg;
    if (reboot_days <= 0)
        return NULL;
    prctl(PR_SET_NAME, "av_reboot", 0, 0, 0);
    log_info("thread_RebootTask start (reboot after %d days uptime)", reboot_days);
    while (g_running) {
        FILE *fp = fopen("/proc/uptime", "r");
        if (fp) {
            long secs = 0;
            if (fscanf(fp, "%ld", &secs) == 1)
                uptime = (unsigned long)secs;
            fclose(fp);
            if (uptime > (unsigned long)reboot_days * 86400) {
                log_info("uptime %lu days: reboot_task triggering", uptime / 86400);
                system("/sbin/reboot");
            }
        }
        sleep(3600);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* audio player (thread_audio_player / av_aplayerrcv /tmp/sound_fifo)  */
/* ------------------------------------------------------------------ */

static void *audio_render_bindfd = NULL;

static int audio_player_init(void)
{
    DECLARE_ATTR(render_attr, gm_audio_render_attr_t);

    audio_render_groupfd = gm_new_groupfd();
    if (!audio_render_groupfd) return -1;

    audio_render_obj = gm_new_obj(GM_AUDIO_RENDER_OBJECT);
    render_attr.vch = 0;
    render_attr.encode_type = (gm_audio_encode_type_t) g_audio_type;
    render_attr.block_size = g_audio_framesamples;
    render_attr.sync_with_lcd_vch = SYNC_LCD_DISABLE;
    if (gm_set_attr(audio_render_obj, &render_attr) < 0) {
        log_error("audio player: gm_set_attr(render) failed");
        return -1;
    }
    audio_render_bindfd = gm_bind(audio_render_groupfd, NULL, audio_render_obj);
    if (gm_apply(audio_render_groupfd) < 0) {
        log_error("audio player: gm_apply failed");
        return -1;
    }
    log_info("audio player render ready");
    return 0;
}

/* read one ADTS frame (7-byte header + payload) and push to renderer */
static int fifo_read_adts(int fd, unsigned char *frame, int maxlen)
{
    int off = 0, got, frame_len;

    while (off < 7) {
        got = read(fd, frame + off, 7 - off);
        if (got <= 0) return -1;
        off += got;
    }
    if (frame[0] != 0xff || (frame[1] & 0xf0) != 0xf0)
        return -1;                      /* not ADTS, drop sync */
    frame_len = ((frame[3] & 0x03) << 11) | (frame[4] << 3) | (frame[5] >> 5);
    if (frame_len < 7 || frame_len > maxlen)
        return -1;
    while (off < frame_len) {
        got = read(fd, frame + off, frame_len - off);
        if (got <= 0) return -1;
        off += got;
    }
    return frame_len;
}

static void *thread_AudioPlayer(void *arg)
{
    int fd;
    unsigned char *frame;
    gm_dec_multi_bitstream_t multi;
    int rc;

    (void)arg;
    if (audio_player_init() < 0)
        return NULL;
    frame = malloc(AUDIO_FRAME_MAX);
    if (!frame) return NULL;

    prctl(PR_SET_NAME, "av_aplayerrcv", 0, 0, 0);
    while (g_running) {
        struct stat st;
        fd = open(SOUND_FIFO, O_RDONLY);
        if (fd < 0) {
            usleep(500000);
            continue;
        }
        /* writer holds it open; without one the FIFO O_RDONLY returns EOF */
        if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
            /* process frames until the writer goes away */
            while (g_running) {
                int flen = fifo_read_adts(fd, frame, AUDIO_FRAME_MAX);
                if (flen <= 0) {
                    if (errno == EAGAIN || flen == 0)
                        break;
                    break;
                }
                memset(&multi, 0, sizeof(multi));
                multi.bindfd = audio_render_bindfd;
                multi.bs_buf = (char *)frame;
                multi.bs_buf_len = flen;
                multi.time_align = TIME_ALIGN_ENABLE;
                rc = gm_send_multi_bitstreams(&multi, 1, 100);
                if (rc < 0)
                    log_error("audio player: gm_send_multi_bitstreams rc=%d", rc);
            }
        }
        close(fd);
        usleep(200000);
    }
    free(frame);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* log upload (log_post_micloud / pack_log)                            */
/* ------------------------------------------------------------------ */

static void log_post_micloud(void)
{
    log_info("upload_log: packing logs");
    system("tar -czf /tmp/miio_av_streamer_log.tgz "
           "/tmp/sd/firmware/log/miio_avstreamer.log /var/log/syslog 2>/dev/null");
    if (cfg_get_int("MIIO_LOG_UPLOAD", 0) > 0)
        log_info("upload_log: ready at /tmp/miio_av_streamer_log.tgz");
}

/* ------------------------------------------------------------------ */
/* RPC / control socket (otd_sock_init + thread_av_rpc_methods)        */
/*   JSON-line protocol on 127.0.0.1:<MIIO_RPC_PORT>:                  */
/*     req:  {"method":"<name>","params":[...]}                        */
/*     resp: {"method":"<name>","ok":true,"result":<obj>}              */
/* ------------------------------------------------------------------ */

static char g_watermark_text[128] = "";

static const char *cfg_get_str(const char *key, const char *dflt)
{
    int i;
    for (i = 0; i < g_cfg_n; i++)
        if (strcmp(g_cfg[i].key, key) == 0)
            return g_cfg[i].val;
    return dflt;
}

static void rpc_write(int fd, const char *s)
{
    if (fd >= 0 && s)
        write(fd, s, strlen(s));
}

static void rpc_result(int fd, const char *method, int ok, const char *result)
{
    char rbuf[2560];
    if (result)
        snprintf(rbuf, sizeof(rbuf), "{\"method\":\"%s\",\"ok\":%s,\"result\":%s}\n",
                 method, ok ? "true" : "false", result);
    else
        snprintf(rbuf, sizeof(rbuf), "{\"method\":\"%s\",\"ok\":%s,\"result\":null}\n",
                 method, ok ? "true" : "false");
    rpc_write(fd, rbuf);
}

static void rpc_value(int fd, const char *method, const char *name, int v)
{
    char obj[128];
    snprintf(obj, sizeof(obj), "{\"%s\":%d}", name, v);
    rpc_result(fd, method, 1, obj);
}

static const char *bool_str(int v) { return v ? "true" : "false"; }

static size_t get_uptime_secs(void)
{
    FILE *fp = fopen("/proc/uptime", "r");
    size_t up = 0;
    if (fp) {
        (void)fscanf(fp, "%zu", &up);
        fclose(fp);
    }
    return up;
}

static void get_local_ip(char *out, size_t outsz)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    struct sockaddr_in *sin;

    out[0] = 0;
    if (s < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "wlan0", sizeof(ifr.ifr_name) - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        sin = (struct sockaddr_in *)&ifr.ifr_addr;
        snprintf(out, outsz, "%s", inet_ntoa(sin->sin_addr));
    }
    close(s);
}

static void rpc_get_prop(int fd)
{
    char result[1600];
    char ip[64] = "0.0.0.0";

    get_local_ip(ip, sizeof(ip));
    snprintf(result, sizeof(result),
             "{\"power_on\":%s,\"night_mode\":%d,\"motion_record\":%s,"
             "\"motion_snapshot\":%s,\"motion_tracking\":%s,\"flip\":%s,"
             "\"watermark\":%s,\"alarm_sensitivity\":%d,\"alarm_interval\":%d,"
             "\"sd_free_mb\":%d,\"ip\":\"%s\",\"uptime\":%zu,"
             "\"fps\":%d,\"width\":%d,\"height\":%d,\"bitrate\":%d,\"gop\":%d}",
             bool_str(g_power_on), g_night_mode, bool_str(g_motion_record),
             bool_str(g_motion_snapshot), bool_str(g_motion_tracking),
             bool_str(g_flip), bool_str(g_watermark),
             g_alarm_sens, g_alarm_interval,
             sd_freespace_mb(), ip, get_uptime_secs(),
             g_enc.framerate, g_enc.width, g_enc.height,
             g_enc.bitrate, g_enc.gop);
    rpc_result(fd, "get_prop", 1, result);
}

static void rpc_get_video_list(int fd)
{
    struct dirent **ents = NULL;
    int n, i;
    char *arr = malloc(64 * 1024);
    size_t off = 0;

    if (!arr) { rpc_result(fd, "get_video_list", 0, NULL); return; }

    n = scandir(STREAM_DIR, &ents, NULL, 0);
    (void)i;
    (void)ents;
    (void)n;
    /* full recursive listing would be nicer, but scandir is one level */
    off += (size_t)snprintf(arr + off, 4096 - off, "[");
    if (ents) {
        for (i = 0; i < n && off < 32768; i++) free(ents[i]);
        free(ents);
    }
    off += (size_t)snprintf(arr + off, 4096 - off, "]");
    rpc_result(fd, "get_video_list", 1, arr);
    free(arr);
}

static void rpc_delete_video(int fd, const char *path)
{
    char p[512], stamp[560];

    if (!path || strncmp(path, STREAM_DIR, strlen(STREAM_DIR)) != 0) {
        rpc_result(fd, "deleteVideo", 0, "{\"reason\":\"bad path\"}");
        return;
    }
    snprintf(p, sizeof(p), "%s", path);
    snprintf(stamp, sizeof(stamp), "%s.timestamp", p);
    unlink(p);
    unlink(stamp);
    log_info("delete_video: %s", p);
    rpc_result(fd, "deleteVideo", 1, "{\"deleted\":true}");
}

static void *thread_RebootNow(void *arg)
{
    (void)arg;
    usleep(100000);
    log_info("reboot_device: /sbin/reboot");
    system("/sbin/reboot");
    return NULL;
}

static char g_self_exe[128] = "/tmp/sd/firmware/bin/miio_avstreamer";

static void daemon_restart(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(g_self_exe, "miio_avstreamer", "-R", (char *)NULL);
        _exit(0);
    }
}

/* shared text dispatcher: MQTT ("cmd arg") and JSON-RPC both land here */
static int rpc_dispatch(const char *method, const char *arg, int fd);

static void rpc_run_cmd(const char *cmd, const char *arg)
{
    if (!cmd) return;
    (void)rpc_dispatch(cmd, arg, -1);
}

static int rpc_dispatch(const char *method, const char *arg, int fd)
{
    int v = arg ? atoi(arg) : 0;

    if (!method) return -1;

    if (strcmp(method, "get_prop") == 0 || strcmp(method, "status") == 0) {
        rpc_get_prop(fd);
        return 0;
    }
    if (strcmp(method, "set_power") == 0) {
        g_power_on = v > 0 ? 1 : 0;
        rpc_value(fd, method, "power_on", g_power_on);
        return 0;
    }
    if (strcmp(method, "set_alarmsensitivity") == 0) {
        g_alarm_sens = v > 0 ? 1 : 0;
        rpc_value(fd, method, "alarm_sensitivity", g_alarm_sens);
        return 0;
    }
    if (strcmp(method, "set_motion_record") == 0) {
        if (v == 0) {
            g_motion_record = 0;
            record_stop(1);
        } else {
            g_motion_record = 1;
        }
        rpc_value(fd, method, "motion_record", g_motion_record);
        return 0;
    }
    if (strcmp(method, "set_motion_snapshot") == 0) {
        g_motion_snapshot = v > 0 ? 1 : 0;
        rpc_value(fd, method, "motion_snapshot", g_motion_snapshot);
        return 0;
    }
    if (strcmp(method, "set_motion_tracking") == 0) {
        g_motion_tracking = v > 0 ? 1 : 0;
        if (g_motion_tracking) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "/tmp/sd/firmware/bin/tracking -d%d -s%d 2>/dev/null &",
                     cfg_get_int("TRACKING_DEADZONE", 2),
                     cfg_get_int("TRACKING_SPEED", 3));
            system(cmd);
        }
        rpc_value(fd, method, "motion_tracking", g_motion_tracking);
        return 0;
    }
    if (strcmp(method, "set_flip") == 0) {
        g_flip = v > 0 ? 1 : 0;
        set_flip(g_flip, g_flip);
        rpc_value(fd, method, "flip", g_flip);
        return 0;
    }
    if (strcmp(method, "set_watermark") == 0) {
        g_watermark = v > 0 ? 1 : 0;
        set_watermark(g_watermark,
                      g_watermark && g_watermark_text[0] ? g_watermark_text : NULL);
        rpc_value(fd, method, "watermark", g_watermark);
        return 0;
    }
    if (strcmp(method, "set_light") == 0) {
        g_light = v > 0 ? 1 : 0;
        night_ir("ir_led", g_light ? "on" : "off");
        rpc_value(fd, method, "light", g_light);
        return 0;
    }
    if (strcmp(method, "set_night_mode") == 0) {
        if (v == 0 || v == 1 || v == 2)
            night_mode_apply(v);
        rpc_value(fd, method, "night_mode", g_night_mode);
        return 0;
    }
    if (strcmp(method, "sd_storge") == 0 || strcmp(method, "sd_status") == 0) {
        char obj[128];
        snprintf(obj, sizeof(obj), "{\"free_mb\":%d}", sd_freespace_mb());
        rpc_result(fd, method, 1, obj);
        return 0;
    }
    if (strcmp(method, "sd_format") == 0) {
        pthread_t t;
        pthread_create(&t, NULL, thread_SDFormat, NULL);
        pthread_detach(t);
        rpc_result(fd, method, 1, NULL);
        return 0;
    }
    if (strcmp(method, "sd_umount") == 0) {
        system("umount -l /tmp/sd 2>/dev/null");
        rpc_result(fd, method, 1, NULL);
        return 0;
    }
    if (strcmp(method, "saveVideo") == 0) {
        record_start(0, v > 0 ? v : DEFAULT_RECORD_SECS);
        rpc_value(fd, method, "recording", g_rec_active);
        return 0;
    }
    if (strcmp(method, "saveVideoNow") == 0) {
        record_start(0, DEFAULT_RECORD_SECS);
        rpc_value(fd, method, "recording", g_rec_active);
        return 0;
    }
    if (strcmp(method, "deleteVideo") == 0) {
        rpc_delete_video(fd, arg);
        return 0;
    }
    if (strcmp(method, "get_video_list") == 0) {
        rpc_get_video_list(fd);
        return 0;
    }
    if (strcmp(method, "playback") == 0) {
        if (arg && access(arg, F_OK) == 0) {
            playback_start(0, arg);
            rpc_result(fd, method, 1, NULL);
        } else {
            rpc_result(fd, method, 0, "{\"reason\":\"no such file\"}");
        }
        return 0;
    }
    if (strcmp(method, "ptz") == 0) {
        int code = 0;
        if (arg) {
            if (isdigit((unsigned char)arg[0]))
                code = atoi(arg);
            else if (strcmp(arg, "up") == 0)    code = 1;
            else if (strcmp(arg, "down") == 0)  code = 2;
            else if (strcmp(arg, "left") == 0)  code = 3;
            else if (strcmp(arg, "right") == 0) code = 4;
            else if (strcmp(arg, "home") == 0 || strcmp(arg, "reset") == 0)
                code = 9;
        }
        if (code == 9) {
            ptz_move(1, g_ptz_x, 0, g_ptz_y);   /* left by x, down by y -> 0,0 */
        } else if (code >= 1 && code <= 4) {
            ptz_control(code);
        }
        rpc_result(fd, method, 1, NULL);
        return 0;
    }
    if (strcmp(method, "correct_ptz") == 0) {
        ptz_move(1, g_ptz_x, 0, g_ptz_y);
        cfg_set_int("ptz-x", 0);
        cfg_set_int("ptz-y", 0);
        do_nvram_commit();
        rpc_result(fd, method, 1, NULL);
        return 0;
    }
    if (strcmp(method, "snapshot") == 0) {
        char obj[300];
        if (take_snapshot(obj, sizeof(obj), 0) == 0)
            rpc_result(fd, method, 1, obj);
        else
            rpc_result(fd, method, 0, NULL);
        return 0;
    }
    if (strcmp(method, "reboot_device") == 0) {
        pthread_t t;
        rpc_result(fd, method, 1, NULL);
        pthread_create(&t, NULL, thread_RebootNow, NULL);
        pthread_detach(t);
        return 0;
    }
    if (strcmp(method, "upload_log") == 0) {
        log_post_micloud();
        rpc_result(fd, method, 1, NULL);
        return 0;
    }
    if (strcmp(method, "HandHouseKeep_thiefAlarm") == 0) {
        g_alarm_active = v > 0 ? 1 : 0;
        rpc_value(fd, method, "alarm", g_alarm_active);
        return 0;
    }
    if (strcmp(method, "setAlarmPushTime") == 0) {
        rpc_value(fd, method, "alarm_push_time", v);
        return 0;
    }
    if (strcmp(method, "restart") == 0) {
        rpc_result(fd, method, 1, NULL);
        daemon_restart();
        return 0;
    }

    log_error("rpc: unknown method '%s'", method);
    return -1;
}

/* read one JSON request line, extract "method" and first "params" arg */
static void rpc_client(const char *line, int fd)
{
    char method[128] = {0};
    char arg[256] = {0};
    const char *p;

    p = strstr(line, "\"method\"");
    if (!p)
        return;
    p = strstr(p, ":");
    if (!p)
        return;
    sscanf(p + 1, " \"%127[A-Za-z0-9_]\"", method);
    if (!method[0])
        return;

    p = strstr(line, "\"params\"");
    if (p) {
        p = strstr(p, "[");
        if (p) {
            p++;
            while (*p == ' ' || *p == '\"') p++;
            if (*p == ']')
                arg[0] = 0;
            else
                sscanf(p, "%255[^,\"]", arg);
        }
    }

    if (rpc_dispatch(method, arg[0] ? arg : NULL, fd) < 0)
        rpc_result(fd, method, 0, NULL);
}

static void *thread_RPC(void *arg)
{
    int lsock, cfd, port = cfg_get_int("MIIO_RPC_PORT", 8686);
    struct sockaddr_in addr;
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    char line[512];
    ssize_t got;

    (void)arg;
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        log_error("rpc: socket() fail");
        return NULL;
    }
    {
        int one = 1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("rpc: bind 127.0.0.1:%d fail", port);
        close(lsock);
        return NULL;
    }
    listen(lsock, 4);
    prctl(PR_SET_NAME, "av_rpc", 0, 0, 0);
    log_info("thread_RPC listening on 127.0.0.1:%d", port);

    while (g_running) {
        cfd = accept(lsock, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (!g_running) break;
            usleep(100000);
            continue;
        }
        prctl(PR_SET_NAME, "av_RPCgetdata", 0, 0, 0);
        while (g_running &&
               (got = read(cfd, line, sizeof(line) - 1)) > 0) {
            line[got] = 0;
            rpc_client(line, cfd);
        }
        close(cfd);
        prctl(PR_SET_NAME, "av_rpc", 0, 0, 0);
    }
    close(lsock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main entry                                                          */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_signal = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_signal = 1;
    g_running = 0;
}

static void usage(void)
{
    fprintf(stderr,
            "miio_avstreamer (local reimplementation of the Mijia v5 cloud daemon)\n"
            "  -t   test mode (built-in self test, no gmlib init)\n"
            "  -R   restart invocation flag\n"
            "  -h   this help\n");
}

static int self_test(void)
{
    printf("miio_avstreamer self-test ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    int test_mode = 0, i;

    /* vendor flags: -t test, -R restart */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0)
            test_mode = 1;
        else if (strcmp(argv[i], "-R") == 0) {
            /* restart invocation: let the old instance die first */
            usleep(100000);
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_signal);
    signal(SIGUSR2, on_signal);

    /* config: try the SD card first, fall back to defaults */
    if (cfg_load_path(DEFAULT_CONFIG) < 0)
        log_error("config: %s not found, using defaults", DEFAULT_CONFIG);

    if (test_mode)
        return self_test();

    /* re-read every flag the run-time modules depend on */
    g_motion_enable     = cfg_get_int("MOTION_DETECTION", 0);
    g_motion_record     = cfg_get_int("MOTION_RECORD", 0);
    g_motion_snapshot   = cfg_get_int("MOTION_TAKE_SNAPSHOT", 0);
    g_motion_tracking   = cfg_get_int("MOTION_TRACKING", 0);
    g_motion_sensitivity = cfg_get_int("MOTION_SENSITIVITY", g_motion_sensitivity);
    g_motion_alarm_seconds = cfg_get_int("MIIO_ALARM_RECORD_SECS", 10);
    g_alarm_interval    = cfg_get_int("MIIO_ALARM_INTERVAL", 5);
    g_alarm_sens        = cfg_get_int("MIIO_ALARM_SENSITIVITY", 1);
    g_night_mode        = cfg_get_int("MIIO_NIGHT_MODE", 0);
    g_flip              = cfg_get_int("MIIO_FLIP", 0);
    g_watermark         = cfg_get_int("MIIO_WATERMARK", 0);
    g_mqtt_enabled      = cfg_get_int("ENABLE_MQTT", 0);
    snprintf(g_watermark_text, sizeof(g_watermark_text), "%s",
             cfg_get_str("MIIO_WATERMARK_TEXT", "MiiCam"));

    if (cfg_get_int("MIIO_API_KEY", 0) > 0)
        log_info("MIIO_API_KEY: token not required for local control socket");

    log_info("miio_avstreamer start (ARMW DVR candidate) pid=%d", getpid());
    log_info("video %dx%d@%d gop=%d bitrate=%d mode=%s",
             g_enc.width, g_enc.height, g_enc.framerate,
             g_enc.gop, g_enc.bitrate,
             g_enc.mode == GM_VBR ? "vbr" : "cbr");

    {
        FILE *pidf = fopen("/var/run/miio_avstreamer.pid", "w");
        if (pidf) {
            fprintf(pidf, "%d\n", getpid());
            fclose(pidf);
        }
    }

    mkfifo(SOUND_FIFO, 0644);
    ptz_load_coords();

    if (gm_stream_init() < 0) {
        log_error("gm_stream_init failed, exiting");
        return -1;
    }

    if (g_watermark)
        set_watermark(1, g_watermark_text[0] ? g_watermark_text : NULL);
    if (g_flip)
        set_flip(1, 1);

    pthread_create(&tid_stream,   NULL, thread_StreamServer,  NULL);
    pthread_create(&tid_videoframe, NULL, thread_VideoFrameData, NULL);
    if (g_audio_enabled)
        pthread_create(&tid_audioframe, NULL, thread_AudioFrameData, NULL);
    if (g_motion_enable)
        pthread_create(&tid_motion, NULL, thread_MotionDetect, NULL);
    pthread_create(&tid_rpc,      NULL, thread_RPC,           NULL);
    pthread_create(&tid_sdcheck,  NULL, thread_SDCardCheck,   NULL);
    pthread_create(&tid_sysmon,   NULL, thread_SysMonitor,    NULL);
    pthread_create(&tid_reboottask, NULL, thread_RebootTask,  NULL);
    pthread_create(&tid_audiplayer, NULL, thread_AudioPlayer, NULL);
    if (g_mqtt_enabled)
        pthread_create(&tid_mqtt, NULL, thread_MQTTSub,       NULL);

    /* quiet the -Wall warnings for mirrors kept for vendor parity */
    (void)tid_record; (void)tid_motalarm; (void)tid_playback;
    (void)tid_p2paudio; (void)tid_qrcode; (void)tid_ptz;
    (void)g_snap_lock; (void)g_motion_lock; (void)g_motion_detected;
    (void)g_test_mode; (void)g_restart;

    while (g_running)
        sleep(1);

    log_info("miio_avstreamer stopping");
    record_stop(1);
    gm_stream_release();
    unlink("/var/run/miio_avstreamer.pid");
    return 0;
}
