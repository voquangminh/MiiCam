/**
 * @file rtspd.c
 *  RTSP daemon cho camera GrainMedia GM8136 (2MP), dung SDK goc GrainMedia
 *  (gmlib.h / librtsp.h).
 *
 *  Kien truc luong video/audio va OSD trong ban nay duoc doi chieu TRUC
 *  TIEP voi source code thuc te dang chay tren firmware muc tieu (du an
 *  MiiCam, https://github.com/voquangminh/MiiCam) ma nguoi dung da cung
 *  cap, thay vi tu suy doan API nhu cac ban truoc. Cu the:
 *
 *  - Video/audio dung 1 BUFFER DUY NHAT moi stream (khong phai pool nhieu
 *    buffer), voi co che "bo qua nhan frame moi neu buffer truoc chua
 *    duoc gui xong" (backpressure that su len tang thu hardware/driver,
 *    khong co "ep giai phong sau timeout" nhu cac ban truoc - day rat co
 *    the la nguyen nhan gay hien tuong client bi "hep dan roi teardown"
 *    ma nguoi dung gap phai, vi ban truoc lam sai lech nhip cung cua
 *    librtsp bang cach luon san sang buffer moi qua pool).
 *  - OSD dung gm_set_osd_font2() (KHONG PHAI gm_set_osd_font()) voi 1 cua
 *    so 2 dong (v_words=2) chua font_index cap phat dong, thay vi 2 cua
 *    so rieng biet.
 *  - stream_reg() dang ky KHONG auth truoc, roi goi stream_authorization()
 *    rieng neu co RTSP_USER/RTSP_PASS (doc qua getenv(), giong het cach
 *    lam cua reference - vi midgard.ini duoc script khoi dong "source"
 *    truoc khi goi rtspd, cac bien tro thanh environment variables).
 *
 * Tinh nang: video H.264/MPEG4/MJPEG, audio AAC-LC, OSD 2 dong (text +
 * timestamp), motion detect -> snapshot + ghi clip, RTSP server da client.
 *
 * Ket noi: rtsp://<ip-camera>:554/live/ch00_0
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <signal.h>

#include "librtsp.h"
#include "gmlib.h"
#include "algorithm/capture_motion_detection2.c"

/* =======================================================================
 * HANG SO CO DINH
 * ===================================================================== */
#define CFG_INI_PATH          "/tmp/sd/midgard.ini"

#define CFG_AUDIO_SAMPLERATE  8000
#define CFG_AUDIO_BITRATE     32000
#define CFG_AUDIO_FRAME_SAMPLES  1024

#define CFG_SNAPSHOT_DIR      "/tmp/sd/RECORDED_IMAGES"
#define CFG_RECORD_DIR        "/tmp/sd/RECORDED_VIDEOS"
#define CFG_MOTION_CLIP_SEC   10

#define RTSP_PORT             554
#define STREAM_NAME           "live/ch00_0"

#define SDPSTR_MAX            128
#define RTP_HZ                90000
#define AUDIO_BS_BUF_LEN      4096

#define VQ_LEN                20  /* gia tri da chung minh trong production (MiiCam rtspd.c that), khong phai 5 */
#define AQ_LEN                2   /* gia tri da chung minh trong production */

typedef enum { ENC_H264 = 0, ENC_MPEG4 = 1, ENC_MJPEG = 2 } enc_type_t;
static const char *enc_type_name[] = { "H264", "MPEG4", "MJPEG" };

/* =======================================================================
 * CAU HINH
 * ===================================================================== */
typedef struct {
    int bitrate, framerate, width, height, mode;
    enc_type_t enc_type;

    int  osd_enable;
    char osd_text[64];
    int  osd_zoom;
    int  osd_bg_palette;

    int motion_detect, motion_snapshot, motion_record;
} config_t;

static config_t g_cfg;
static char g_rtsp_user[64] = {0};
static char g_rtsp_pass[64] = {0};
static int  g_rtsp_use_auth = 0;

/* =======================================================================
 * BIEN TOAN CUC HE THONG / RTSP / GRAPH
 * ===================================================================== */
gm_system_t gm_system;

void *video_groupfd;
void *audio_groupfd;

void *capture_object;
void *venc_object;
void *venc_bindfd;

void *audio_grab_object;
void *audio_enc_object;
void *audio_bindfd;

static int   g_vqno = -1, g_aqno = -1;
static int   g_sr = -1;
static char  g_vsdp[SDPSTR_MAX];
static char  g_asdp[SDPSTR_MAX];
static volatile int g_play = 0;
static volatile int g_running = 1;

static pthread_t th_video, th_audio, th_motion, th_osd;

typedef struct {
    char *buf;
    int   buf_len;
    volatile int pending;
    int   size;
} single_buf_t;

static single_buf_t g_vid = {0};
static single_buf_t g_aud = {0};
static pthread_mutex_t g_vid_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_aud_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_enqueue_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile unsigned long g_vid_sent = 0, g_vid_dropped = 0;
static volatile unsigned long g_aud_sent = 0, g_aud_dropped = 0;

/* !!! SUA LOI QUAN TRONG NHAT (phat hien khi doi chieu voi rtspd.c THAT
 * dang chay production - du an MiiCam) !!!
 *
 * Truoc day toi hieu SAI dong "timestamp: Global across all entities,
 * in 90 KHz" trong librtsp.h la "phai tu tao 1 dong ho wall-clock duy
 * nhat dung chung cho ca audio va video". Day la nguyen nhan CHINH gay
 * lech A-V hang chuc giay qua nhieu lan test.
 *
 * Su that (theo dung code goc dang chay production): MOI loai stream
 * dung RTP clock rate RIENG theo dung chuan RFC 3551 - H.264 luon la
 * 90000Hz, con AAC/G711 dung DUNG sample rate cua no (8000Hz o day),
 * KHONG PHAI 90000Hz. Va quan trong nhat: VAN DUNG bs.timestamp CUA
 * CHINH THIET BI tra ve (khong phai tu tao tu wall-clock), code that
 * dung:
 *   get_tick_gm(tv_ms)   = tv_ms * (RTP_HZ/1000)        = tv_ms * 90   (video)
 *   get_autick_gm(tv_ms) = tv_ms * 8                                    (audio, 8kHz)
 */
static unsigned int video_rtp_ts(unsigned int tv_ms)
{
    return tv_ms * (RTP_HZ / 1000); /* 90kHz, chuan H.264/MPEG4/MJPEG */
}

static unsigned int audio_rtp_ts(unsigned int tv_ms)
{
    return tv_ms * (CFG_AUDIO_SAMPLERATE / 1000); /* = tv_ms*8 o 8kHz */
}

static FILE      *g_rec_fp = NULL;
static time_t      g_rec_end_time = 0;
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;

#define MD_CH               0
#define MD_MB_SIZE          32
struct mdt_alg_t    g_mdt_alg    = { sub_region: NULL };
struct mdt_result_t g_mdt_result = { sub_region: NULL };

/* =======================================================================
 * USAGE / CLI PARSING
 * ===================================================================== */
static void print_usage(void)
{
    printf("Usage:\n");
    printf(" ./rtspd [-bfwhm] [-j|-4]\n");
    printf(
        "\nAvailable options:\n"
        "-b [1-8192]    - Set the bitrate         (default: 2048)\n"
        "-f [1-20]      - Set the framerate       (default: 20)\n"
        "-w [1-1920]    - Set the image width     (default: 1920 pixels)\n"
        "-h [1-1280]    - Set the image height    (default: 1290 pixels)\n"
        "-m [1-4]       - Set the bitrate mode    (default: 1, CBR)\n\n"
        "-j (optional)  - Use MJPEG encoding      (default: off)\n"
        "-4 (optional)  - Use MPEG4 encoding      (default: off)\n"
        "-o (optional)  - Enable OSD timestamp    (default: on)\n"
        "-t [text]      - Set OSD string text     (default: 'hostname')\n"
        "-z [0-4]       - Set OSD font zoom (0=none,1=2x,2=3x,3=4x,4=1/2) (default: 0)\n"
        "-d (optional)  - Enable motion detection (default: off)\n"
        "-s (optional)  - Take a snapshot when motion detected (default: off)\n"
        "-r (optional)  - Record a 10 second clip on motion    (default: off)\n"
        "-B [0-15]      - Set OSD background palette index (default: 1)\n"
    );
    exit(EXIT_FAILURE);
}

static void config_set_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->bitrate    = 2048;
    c->framerate  = 20;
    c->width      = 1920;
    c->height     = 1080;
    c->mode       = GM_CBR;
    c->enc_type   = ENC_H264;

    c->osd_enable      = 1;
    c->osd_text[0]     = '\0';
    c->osd_zoom        = GM_OSD_FONT_ZOOM_NONE;
    c->osd_bg_palette  = 1;

    c->motion_detect   = 0;
    c->motion_snapshot = 0;
    c->motion_record   = 0;
}

static void read_hostname(char *out, size_t outlen)
{
    FILE *f;
    char line[256];

    out[0] = '\0';
    f = fopen(CFG_INI_PATH, "r");
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        char *v, *end;

        if (strncmp(line, "HOSTNAME=", 9) != 0)
            continue;

        v = line + 9;
        if (*v == '"') v++;

        end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' ||
                            end[-1] == '"'  || end[-1] == ' '))
            *--end = '\0';

        strncpy(out, v, outlen - 1);
        out[outlen - 1] = '\0';
        break;
    }
    fclose(f);
}

#define CHECK_RANGE(val, lo, hi, optname) \
    do { if ((val) < (lo) || (val) > (hi)) { \
        fprintf(stderr, "Gia tri khong hop le cho -%s: %d (pham vi %d-%d)\n", \
                optname, (val), (lo), (hi)); \
        print_usage(); } } while (0)

static void config_parse_args(config_t *c, int argc, char *argv[])
{
    int opt;

    optind = 1;
    while ((opt = getopt(argc, argv, "b:f:w:h:m:jot:z:dsrB:4")) != -1) {
        switch (opt) {
            case 'b': c->bitrate = atoi(optarg); CHECK_RANGE(c->bitrate, 1, 8192, "b"); break;
            case 'f': c->framerate = atoi(optarg); CHECK_RANGE(c->framerate, 1, 30, "f"); break;
            case 'w': c->width = atoi(optarg); CHECK_RANGE(c->width, 1, 1920, "w"); break;
            case 'h': c->height = atoi(optarg); CHECK_RANGE(c->height, 1, 1280, "h"); break;
            case 'm': c->mode = atoi(optarg); CHECK_RANGE(c->mode, 1, 4, "m"); break;
            case 'j': c->enc_type = ENC_MJPEG; break;
            case '4': c->enc_type = ENC_MPEG4; break;
            case 'o':
                c->osd_enable = 1;
                if (optarg) strncpy(c->osd_text, optarg, sizeof(c->osd_text) - 1);
                break;
            case 't':
                strncpy(c->osd_text, optarg, sizeof(c->osd_text) - 1);
                c->osd_enable = 1;
                break;
            case 'z': c->osd_zoom = atoi(optarg); CHECK_RANGE(c->osd_zoom, 0, 4, "z"); break;
            case 'd': c->motion_detect = 1; break;
            case 's': c->motion_snapshot = 1; break;
            case 'r': c->motion_record = 1; break;
            case 'B':
                c->osd_bg_palette = atoi(optarg);
                CHECK_RANGE(c->osd_bg_palette, 0, 15, "B");
                c->osd_enable = 1;
                break;
            default:
                print_usage();
        }
    }

    if ((c->motion_snapshot || c->motion_record) && !c->motion_detect) {
        fprintf(stderr, "[FATAL] -s/-r can -d (motion detection) de hoat dong\n");
        exit(EXIT_FAILURE);
    }
}

/* =======================================================================
 * DOC /tmp/sd/midgard.ini - dung cu phap shell KEY="value" / KEY=value
 * (file nay duoc script khoi dong "source" truoc khi goi rtspd, nen cac
 * gia tri string thuong duoc bao trong dau nhay kep giong bien shell).
 * Cac key THAT da xac nhan tu log thuc te tren camera:
 *   HOSTNAME, RTSP_ENABLED, RTSP_ARGS, RTSP_USER, RTSP_PASS,
 *   MOTION_ENABLED, SNAPSHOT_ENABLED, RECORD_ENABLED, OSD_BG
 * (cac key khac nhu CLOUD_DISABLED, SSH_ROOT_PASS, NTP, miio_* ... la
 * cua cac dich vu KHAC trong firmware, rtspd bo qua khong dung toi).
 * ===================================================================== */
static int ini_get(const char *path, const char *key, char *out, size_t outlen)
{
    FILE *f;
    char line[256];
    size_t keylen = strlen(key);

    out[0] = '\0';
    f = fopen(path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char *v, *end;

        if (strncmp(line, key, keylen) != 0 || line[keylen] != '=')
            continue;

        v = line + keylen + 1;
        if (*v == '"') v++;

        end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' ||
                            end[-1] == '"'  || end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        strncpy(out, v, outlen - 1);
        out[outlen - 1] = '\0';
        fclose(f);
        return (out[0] != '\0');
    }
    fclose(f);
    return 0;
}

/* RTSP_ARGS="-b 4096 -d -s -r" -> tach thanh argv[] roi chay qua CUNG
 * mot ham getopt() nhu tham so dong lenh that, de dong bo logic 100%. */
static void parse_rtsp_args_string(config_t *c, const char *args_str)
{
    char buf[256];
    char *tokens[32];
    int ntok = 0;
    char *p, *saveptr = NULL;

    if (!args_str || args_str[0] == '\0') return;

    strncpy(buf, args_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    tokens[ntok++] = (char *) "rtspd";  /* argv[0] gia, getopt bo qua */
    p = strtok_r(buf, " \t", &saveptr);
    while (p && ntok < 31) {
        tokens[ntok++] = p;
        p = strtok_r(NULL, " \t", &saveptr);
    }
    fprintf(stderr, "[CFG] Ap dung RTSP_ARGS=\"%s\" (%d token)\n", args_str, ntok - 1);
    config_parse_args(c, ntok, tokens);
}

/* =======================================================================
 * TIEN ICH
 * ===================================================================== */
static void ensure_dir(const char *dir)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    system(cmd);
}

static void build_timestamped_path(char *out, size_t out_sz,
                                    const char *dir, const char *prefix, const char *ext)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(out, out_sz, "%s/%s_%04d%02d%02d_%02d%02d%02d.%s",
             dir, prefix,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ext);
}

char *get_local_ip(void)
{
    int fd;
    struct ifreq ifr;
    struct sockaddr_in sin;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
    ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    return inet_ntoa(sin.sin_addr);
}

/* =======================================================================
 * OSD: gm_set_osd_font2() voi 1 cua so 2 dong
 * ===================================================================== */
#define OSD_PALETTE_COLOR_WHITE   0xEB80EB80
#define OSD_PALETTE_COLOR_BLACK   0x10801080

static unsigned short g_osd_font2_text[64];
static pthread_mutex_t g_osd_mutex = PTHREAD_MUTEX_INITIALIZER;

static void osd_setup_palette(void)
{
    gm_palette_table_t palette;
    int i;
    memset(&palette, 0, sizeof(palette));
    for (i = 0; i < 16; i++)
        palette.palette_table[i] = OSD_PALETTE_COLOR_BLACK;
    palette.palette_table[0] = OSD_PALETTE_COLOR_WHITE;
    if (g_cfg.osd_bg_palette >= 0 && g_cfg.osd_bg_palette < 16)
        palette.palette_table[g_cfg.osd_bg_palette] = OSD_PALETTE_COLOR_BLACK;
    if (gm_set_palette_table(&palette) < 0)
        fprintf(stderr, "[OSD] gm_set_palette_table loi\n");
}

static void osd_set_text(const char *line1, const char *line2)
{
    gm_osd_font2_t f;
    int h_words, v_words, line1_len, line2_len, i;

    if (!capture_object || !line1 || !line2) return;

    line1_len = (int) strlen(line1);
    line2_len = (int) strlen(line2);
    h_words = (line1_len > line2_len) ? line1_len : line2_len;
    v_words = 2;
    if (h_words > 32) h_words = 32;
    if (line1_len > h_words) line1_len = h_words;
    if (line2_len > h_words) line2_len = h_words;

    pthread_mutex_lock(&g_osd_mutex);

    for (i = 0; i < (int)(sizeof(g_osd_font2_text) / sizeof(g_osd_font2_text[0])); i++)
        g_osd_font2_text[i] = (unsigned short) ' ';
    for (i = 0; i < line1_len; i++)
        g_osd_font2_text[i] = (unsigned short) line1[i];
    for (i = 0; i < line2_len; i++)
        g_osd_font2_text[h_words + i] = (unsigned short) line2[i];

    memset(&f, 0, sizeof(f));
    f.enabled = 1;
    f.win_idx = 0;
    f.align_type = GM_OSD_ALIGN_TOP_LEFT;
    f.x = 10;
    f.y = 10;
    f.h_words = h_words;
    f.v_words = v_words;
    f.h_space = 0;
    f.v_space = 0;
    f.font_index_len = h_words * v_words;
    f.font_index = g_osd_font2_text;
    f.font_alpha = GM_OSD_FONT_ALPHA_75;
    f.win_alpha  = GM_OSD_FONT_ALPHA_75;
    f.font_palette_idx = 0;
    f.win_palette_idx  = g_cfg.osd_bg_palette;
    f.priority = GM_OSD_PRIORITY_MARK_ON_OSD;
    f.smooth.enabled = 1;
    f.smooth.level = GM_OSD_FONT_SMOOTH_LEVEL_WEAK;
    f.marquee.mode = GM_OSD_MARQUEE_MODE_NONE;
    f.border.enabled = 0;
    f.border.width = 1;
    f.border.type = GM_OSD_BORDER_TYPE_WIN;
    f.border.palette_idx = 1;
    f.font_zoom = (gm_osd_font_zoom_t) g_cfg.osd_zoom;

    if (gm_set_osd_font2(capture_object, &f) < 0)
        fprintf(stderr, "[OSD] gm_set_osd_font2 loi\n");

    pthread_mutex_unlock(&g_osd_mutex);
}

static void osd_update(void)
{
    char timestamp[32];
    char line1[64];
    time_t now;
    struct tm tm;

    if (g_cfg.osd_text[0] != '\0')
        snprintf(line1, sizeof(line1), "%s", g_cfg.osd_text);
    else
        snprintf(line1, sizeof(line1), "chuangmi");

    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
    osd_set_text(line1, timestamp);
}

static void *osd_thread(void *arg)
{
    (void) arg;
    if (!g_cfg.osd_enable) return NULL;
    while (g_running) {
        osd_update();
        sleep(1);
    }
    return NULL;
}

/* =======================================================================
 * SNAPSHOT
 * ===================================================================== */
#define SNAPSHOT_MAX_LEN   (256 * 1024)
static char *g_snapshot_buf = NULL;

static const char *take_snapshot(void)
{
    static char path[256];
    snapshot_t snap;
    int len;
    FILE *fp;

    if (!g_snapshot_buf) {
        g_snapshot_buf = (char *) malloc(SNAPSHOT_MAX_LEN);
        if (!g_snapshot_buf) return NULL;
    }

    memset(&snap, 0, sizeof(snap));
    snap.bindfd        = venc_bindfd;
    snap.image_quality  = 80;
    snap.bs_buf         = g_snapshot_buf;
    snap.bs_buf_len      = SNAPSHOT_MAX_LEN;
    snap.bs_width        = g_cfg.width;
    snap.bs_height        = g_cfg.height;

    len = gm_request_snapshot(&snap, 500);
    if (len <= 0) {
        fprintf(stderr, "[SNAP] gm_request_snapshot that bai (len=%d)\n", len);
        return NULL;
    }

    ensure_dir(CFG_SNAPSHOT_DIR);
    build_timestamped_path(path, sizeof(path), CFG_SNAPSHOT_DIR, "snap", "jpg");
    fp = fopen(path, "wb");
    if (!fp) return NULL;
    fwrite(g_snapshot_buf, 1, len, fp);
    fclose(fp);
    fprintf(stderr, "[SNAP] da luu %s (%d bytes)\n", path, len);
    return path;
}

/* =======================================================================
 * RECORDING
 * ===================================================================== */
static const char *rec_file_ext(void)
{
    switch (g_cfg.enc_type) {
        case ENC_MPEG4: return "m4v";
        case ENC_MJPEG: return "mjpeg";
        default:         return "h264";
    }
}

static void recorder_start_clip(void)
{
    char path[256];
    ensure_dir(CFG_RECORD_DIR);
    build_timestamped_path(path, sizeof(path), CFG_RECORD_DIR, "motion", rec_file_ext());
    if (g_rec_fp) fclose(g_rec_fp);
    g_rec_fp = fopen(path, "wb");
    if (!g_rec_fp) return;
    g_rec_end_time = time(NULL) + CFG_MOTION_CLIP_SEC;
    fprintf(stderr, "[REC] bat dau ghi clip %ds: %s\n", CFG_MOTION_CLIP_SEC, path);
}

static void recorder_feed(const char *data, int len)
{
    pthread_mutex_lock(&g_rec_lock);
    if (g_rec_fp) {
        if (time(NULL) >= g_rec_end_time) {
            fclose(g_rec_fp);
            g_rec_fp = NULL;
            fprintf(stderr, "[REC] ket thuc clip\n");
        } else {
            fwrite(data, 1, len, g_rec_fp);
        }
    }
    pthread_mutex_unlock(&g_rec_lock);
}

static void on_motion_start(void)
{
    fprintf(stderr, "[MD] phat hien chuyen dong\n");
    if (g_cfg.motion_snapshot)
        take_snapshot();
    if (g_cfg.motion_record) {
        pthread_mutex_lock(&g_rec_lock);
        recorder_start_clip();
        pthread_mutex_unlock(&g_rec_lock);
    }
}

static void on_motion_stop(void)
{
    fprintf(stderr, "[MD] het chuyen dong\n");
}

/* =======================================================================
 * MOTION DETECT
 * ===================================================================== */
static int set_cap_motion(int cap_vch, unsigned int id, unsigned int value)
{
    gm_cap_motion_t cm;
    cm.id = id;
    cm.value = value;
    return gm_set_cap_motion(cap_vch, &cm);
}

static int motion_setup(void)
{
    int mb_w_num, mb_h_num;

    g_mdt_alg.sub_region = (struct mdt_reg_t *) malloc(sizeof(struct mdt_reg_t));
    if (!g_mdt_alg.sub_region) return -1;
    memset(g_mdt_alg.sub_region, 0, sizeof(struct mdt_reg_t));

    g_mdt_alg.u_width      = g_cfg.width;
    g_mdt_alg.u_height     = g_cfg.height;
    g_mdt_alg.u_mb_width   = MD_MB_SIZE;
    g_mdt_alg.u_mb_height  = MD_MB_SIZE;
    g_mdt_alg.training_time = 15;
    g_mdt_alg.frame_count   = 0;
    g_mdt_alg.sensitive_th  = 80;

    mb_w_num = (g_mdt_alg.u_width  + (MD_MB_SIZE - 1)) / MD_MB_SIZE;
    mb_h_num = (g_mdt_alg.u_height + (MD_MB_SIZE - 1)) / MD_MB_SIZE;
    g_mdt_alg.mb_w_num = mb_w_num;
    g_mdt_alg.mb_h_num = mb_h_num;

    g_mdt_alg.sub_region[0].is_enabled    = 1;
    g_mdt_alg.sub_region[0].start_block_x = 0;
    g_mdt_alg.sub_region[0].start_block_y = 0;
    g_mdt_alg.sub_region[0].end_block_x   = mb_w_num - 1;
    g_mdt_alg.sub_region[0].end_block_y   = mb_h_num - 1;
    g_mdt_alg.sub_region[0].alarm_th      = 80;
    g_mdt_alg.sub_region[0].alarm         = NO_MOTION;
    g_mdt_alg.sub_region_num = 1;

    set_cap_motion(MD_CH, 0, 32);
    set_cap_motion(MD_CH, 1, 7371);
    set_cap_motion(MD_CH, 2, 7);
    set_cap_motion(MD_CH, 3, 9);
    set_cap_motion(MD_CH, 4, 11);
    set_cap_motion(MD_CH, 5, 15);
    set_cap_motion(MD_CH, 7, 0x9ffb0);
    set_cap_motion(MD_CH, 8, 9);
    set_cap_motion(MD_CH, 10, 0x7fe0);

    if (motion_detection_update(venc_bindfd, &g_mdt_alg) != 0) {
        fprintf(stderr, "[MD] motion_detection_update loi\n");
        return -1;
    }

    g_mdt_result.sub_region = (struct mdt_reg_result_t *)
                               malloc(sizeof(struct mdt_reg_result_t));
    if (!g_mdt_result.sub_region) return -1;
    g_mdt_result.sub_region_num = 1;
    return 0;
}

static void *motion_thread(void *arg)
{
    (void) arg;
    gm_multi_cap_md_t cap_md;
    static int prev_motion = 0;
    int ret, cur_motion;

    if (!g_cfg.motion_detect) return NULL;

    memset(&cap_md, 0, sizeof(cap_md));
    cap_md.bindfd = venc_bindfd;
    cap_md.cap_md_info.md_buf_len = CAP_MOTION_SIZE;
    cap_md.cap_md_info.md_buf = (char *) malloc(CAP_MOTION_SIZE);
    if (!cap_md.cap_md_info.md_buf) return NULL;

    while (g_running) {
        ret = gm_recv_multi_cap_md(&cap_md, 1);
        if (ret < 0) { usleep(50000); continue; }

        ret = motion_detection_handling(&cap_md, &g_mdt_result, 1);
        if (ret < 0) continue;

        if (g_mdt_result.ch_result == MOTION_IS_READY) {
            cur_motion = (g_mdt_result.sub_region[0].reg_result == MOTION_DETECTED) ? 1 : 0;
            if (cur_motion && !prev_motion) on_motion_start();
            else if (!cur_motion && prev_motion) on_motion_stop();
            prev_motion = cur_motion;
        }
        usleep(200000);
    }

    free(cap_md.cap_md_info.md_buf);
    return NULL;
}

/* =======================================================================
 * VIDEO ENCODE THREAD
 * ===================================================================== */
static void *video_thread(void *arg)
{
    (void) arg;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];
    gm_ss_entity entity;
    int ret, sdp_ready = 0;
    int ss_type;
    struct timeval hb_last, hb_now;

    g_vid.buf_len = g_cfg.width * g_cfg.height * 3 / 2;
    g_vid.buf = (char *) malloc(g_vid.buf_len);
    if (!g_vid.buf) {
        fprintf(stderr, "[VID] khong du bo nho\n");
        return NULL;
    }
    g_vid.pending = 0;

    switch (g_cfg.enc_type) {
        case ENC_MPEG4: ss_type = GM_SS_TYPE_MP4; break;
        case ENC_MJPEG: ss_type = GM_SS_TYPE_MJPEG; break;
        default:         ss_type = GM_SS_TYPE_H264; break;
    }

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = venc_bindfd;
    poll_fds[0].event  = GM_POLL_READ;

    gettimeofday(&hb_last, NULL);

    while (g_running) {
        ret = gm_poll(poll_fds, 1, 500);

        gettimeofday(&hb_now, NULL);
        if ((hb_now.tv_sec - hb_last.tv_sec) >= 5) {
            fprintf(stderr, "[VID] heartbeat: sent=%lu dropped=%lu g_play=%d pending=%d\n",
                    g_vid_sent, g_vid_dropped, g_play, g_vid.pending);
            hb_last = hb_now;
        }

        if (ret == GM_TIMEOUT) continue;
        if (g_vid.pending) continue;

        if (poll_fds[0].revent.event != GM_POLL_READ) continue;
        if ((int) poll_fds[0].revent.bs_len > g_vid.buf_len) continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        multi_bs[0].bindfd = venc_bindfd;
        multi_bs[0].bs.bs_buf = g_vid.buf;
        multi_bs[0].bs.bs_buf_len = g_vid.buf_len;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        if (gm_recv_multi_bitstreams(multi_bs, 1) < 0) continue;
        if (multi_bs[0].retval != GM_SUCCESS) continue;

        if (!sdp_ready) {
            if (g_cfg.enc_type != ENC_MJPEG && !multi_bs[0].bs.keyframe)
                continue;
            stream_sdp_parameter_encoder((char *) enc_type_name[g_cfg.enc_type],
                                          (unsigned char *) multi_bs[0].bs.bs_buf,
                                          multi_bs[0].bs.bs_len,
                                          g_vsdp, SDPSTR_MAX);
            sdp_ready = 1;
            fprintf(stderr, "[VID] Da sinh SDP video (%s)\n", enc_type_name[g_cfg.enc_type]);
            continue;
        }

        if (g_cfg.motion_record)
            recorder_feed(multi_bs[0].bs.bs_buf, multi_bs[0].bs.bs_len);

        if (!g_play || g_vqno < 0)
            continue;

        entity.data = multi_bs[0].bs.bs_buf;
        entity.size = multi_bs[0].bs.bs_len;
        entity.timestamp = video_rtp_ts(multi_bs[0].bs.timestamp);

        pthread_mutex_lock(&g_vid_lock);
        g_vid.pending = 1;
        g_vid.size    = multi_bs[0].bs.bs_len;
        pthread_mutex_unlock(&g_vid_lock);

        pthread_mutex_lock(&g_enqueue_mutex);
        ret = stream_media_enqueue(ss_type, g_vqno, &entity);
        pthread_mutex_unlock(&g_enqueue_mutex);

        if (ret < 0) {
            pthread_mutex_lock(&g_vid_lock);
            g_vid.pending = 0;
            pthread_mutex_unlock(&g_vid_lock);
            g_vid_dropped++;
            if (ret != ERR_FULL)
                fprintf(stderr, "[VID] stream_media_enqueue loi %d\n", ret);
        } else {
            g_vid_sent++;
        }
    }

    free(g_vid.buf);
    return NULL;
}

/* =======================================================================
 * AUDIO ENCODE THREAD
 * ===================================================================== */
static void *audio_thread(void *arg)
{
    (void) arg;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];
    gm_ss_entity entity;
    int ret, sdp_ready = 0;
    struct timeval hb_last, hb_now;

    g_aud.buf_len = AUDIO_BS_BUF_LEN;
    g_aud.buf = (char *) malloc(AUDIO_BS_BUF_LEN);
    if (!g_aud.buf) return NULL;
    g_aud.pending = 0;

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = audio_bindfd;
    poll_fds[0].event  = GM_POLL_READ;

    gettimeofday(&hb_last, NULL);

    while (g_running) {
        ret = gm_poll(poll_fds, 1, 500);

        gettimeofday(&hb_now, NULL);
        if ((hb_now.tv_sec - hb_last.tv_sec) >= 5) {
            fprintf(stderr, "[AUD] heartbeat: sent=%lu dropped=%lu g_play=%d pending=%d\n",
                    g_aud_sent, g_aud_dropped, g_play, g_aud.pending);
            hb_last = hb_now;
        }

        if (ret == GM_TIMEOUT) continue;
        if (g_aud.pending) continue;
        if (poll_fds[0].revent.event != GM_POLL_READ) continue;
        if ((int) poll_fds[0].revent.bs_len > AUDIO_BS_BUF_LEN) continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        multi_bs[0].bindfd = audio_bindfd;
        multi_bs[0].bs.bs_buf = g_aud.buf;
        multi_bs[0].bs.bs_buf_len = AUDIO_BS_BUF_LEN;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        if (gm_recv_multi_bitstreams(multi_bs, 1) < 0) continue;
        if (multi_bs[0].retval != GM_SUCCESS) continue;

        if (!sdp_ready) {
            stream_sdp_parameter_encoder("AAC",
                                          (unsigned char *) multi_bs[0].bs.bs_buf,
                                          multi_bs[0].bs.bs_len,
                                          g_asdp, SDPSTR_MAX);
            sdp_ready = 1;
            fprintf(stderr, "[AUD] Da sinh SDP audio (AAC)\n");
            continue;
        }

        if (!g_play || g_aqno < 0)
            continue;

        entity.data = multi_bs[0].bs.bs_buf;
        entity.size = multi_bs[0].bs.bs_len;
        entity.timestamp = audio_rtp_ts(multi_bs[0].bs.timestamp);

        pthread_mutex_lock(&g_aud_lock);
        g_aud.pending = 1;
        g_aud.size    = multi_bs[0].bs.bs_len;
        pthread_mutex_unlock(&g_aud_lock);

        pthread_mutex_lock(&g_enqueue_mutex);
        ret = stream_media_enqueue(GM_SS_TYPE_AAC, g_aqno, &entity);
        pthread_mutex_unlock(&g_enqueue_mutex);

        if (ret < 0) {
            pthread_mutex_lock(&g_aud_lock);
            g_aud.pending = 0;
            pthread_mutex_unlock(&g_aud_lock);
            g_aud_dropped++;
            if (ret != ERR_FULL)
                fprintf(stderr, "[AUD] stream_media_enqueue loi %d\n", ret);
        } else {
            g_aud_sent++;
        }
    }

    free(g_aud.buf);
    return NULL;
}

/* =======================================================================
 * CALLBACK CUA LIBRTSP
 * ===================================================================== */
static int frm_cb(int type, int qno, gm_ss_entity *entity)
{
    (void) type;

    if (qno == g_vqno) {
        pthread_mutex_lock(&g_vid_lock);
        if (g_vid.pending && g_vid.buf == entity->data && g_vid.size == entity->size)
            g_vid.pending = 0;
        pthread_mutex_unlock(&g_vid_lock);
    } else if (qno == g_aqno) {
        pthread_mutex_lock(&g_aud_lock);
        if (g_aud.pending && g_aud.buf == entity->data && g_aud.size == entity->size)
            g_aud.pending = 0;
        pthread_mutex_unlock(&g_aud_lock);
    }
    return 0;
}

static int cmd_cb(char *name, int sno, int cmd, void *p)
{
    (void) name; (void) sno; (void) p;
    switch (cmd) {
        case GM_STREAM_CMD_OPTION:
        case GM_STREAM_CMD_DESCRIBE:
        case GM_STREAM_CMD_SETUP:
            return 0;
        case GM_STREAM_CMD_PLAY:
            g_play = 1;
            fprintf(stderr, "[RTSP] Client PLAY\n");
            return 0;
        case GM_STREAM_CMD_PAUSE:
            fprintf(stderr, "[RTSP] Client PAUSE\n");
            return 0;
        case GM_STREAM_CMD_TEARDOWN:
            g_play = 0;
            fprintf(stderr, "[RTSP] Client TEARDOWN\n");
            return 0;
        default:
            return 0;
    }
}

/* =======================================================================
 * KHOI TAO / GIAI PHONG GRAPH
 * ===================================================================== */
static int graph_init(void)
{
    DECLARE_ATTR(cap_attr, gm_cap_attr_t);
    DECLARE_ATTR(h264e_attr, gm_h264e_attr_t);
    DECLARE_ATTR(mpeg4e_attr, gm_mpeg4e_attr_t);
    DECLARE_ATTR(mjpege_attr, gm_mjpege_attr_t);
    DECLARE_ATTR(dnr_attr, gm_3dnr_attr_t);
    DECLARE_ATTR(audio_grab_attr, gm_audio_grab_attr_t);
    DECLARE_ATTR(audio_enc_attr, gm_audio_enc_attr_t);
    gm_enc_ratecontrol_mode_t rc_mode;

    gm_init();
    gm_get_sysinfo(&gm_system);

    if (g_cfg.width > gm_system.cap[0].dim.width ||
        g_cfg.height > gm_system.cap[0].dim.height) {
        fprintf(stderr, "[WARN] Yeu cau %dx%d vuot qua sensor that (%dx%d), "
                "dung do phan giai sensor.\n",
                g_cfg.width, g_cfg.height,
                gm_system.cap[0].dim.width, gm_system.cap[0].dim.height);
        g_cfg.width  = gm_system.cap[0].dim.width;
        g_cfg.height = gm_system.cap[0].dim.height;
    }

    rc_mode = (gm_enc_ratecontrol_mode_t) g_cfg.mode;

    video_groupfd = gm_new_groupfd();

    capture_object = gm_new_obj(GM_CAP_OBJECT);
    venc_object    = gm_new_obj(GM_ENCODER_OBJECT);

    cap_attr.cap_vch = MD_CH;
    cap_attr.path = 3;
    cap_attr.enable_mv_data = 1;
    gm_set_attr(capture_object, &cap_attr);

    if (g_cfg.width >= (gm_system.cap[0].dim.width / 2) &&
        g_cfg.height >= (gm_system.cap[0].dim.height / 2)) {
        dnr_attr.enabled = 1;
        gm_set_attr(capture_object, &dnr_attr);
    }

    switch (g_cfg.enc_type) {
        case ENC_MPEG4:
            mpeg4e_attr.dim.width  = g_cfg.width;
            mpeg4e_attr.dim.height = g_cfg.height;
            mpeg4e_attr.frame_info.framerate = g_cfg.framerate;
            mpeg4e_attr.ratectl.mode        = rc_mode;
            mpeg4e_attr.ratectl.gop          = 60;
            mpeg4e_attr.ratectl.bitrate       = g_cfg.bitrate;
            mpeg4e_attr.ratectl.bitrate_max    = g_cfg.bitrate;
            gm_set_attr(venc_object, &mpeg4e_attr);
            break;
        case ENC_MJPEG:
            mjpege_attr.dim.width  = g_cfg.width;
            mjpege_attr.dim.height = g_cfg.height;
            mjpege_attr.frame_info.framerate = g_cfg.framerate;
            mjpege_attr.quality = 30;
            gm_set_attr(venc_object, &mjpege_attr);
            break;
        default:
            h264e_attr.dim.width  = g_cfg.width;
            h264e_attr.dim.height = g_cfg.height;
            h264e_attr.frame_info.framerate = g_cfg.framerate;
            h264e_attr.ratectl.mode         = rc_mode;
            h264e_attr.ratectl.gop          = 40;
            h264e_attr.ratectl.bitrate      = g_cfg.bitrate;
            h264e_attr.ratectl.bitrate_max  = g_cfg.bitrate;
            h264e_attr.ratectl.init_quant   = 25;
            h264e_attr.ratectl.min_quant    = 20;
            h264e_attr.ratectl.max_quant    = 51;
            h264e_attr.b_frame_num          = 0;
            h264e_attr.enable_mv_data       = 0;
            gm_set_attr(venc_object, &h264e_attr);
            break;
    }

    venc_bindfd = gm_bind(video_groupfd, capture_object, venc_object);

    if (gm_apply(video_groupfd) < 0) {
        fprintf(stderr, "[FATAL] gm_apply(video_groupfd) that bai!\n");
        return -1;
    }

    audio_groupfd = gm_new_groupfd();
    audio_grab_object = gm_new_obj(GM_AUDIO_GRAB_OBJECT);
    audio_enc_object   = gm_new_obj(GM_AUDIO_ENCODER_OBJECT);

    audio_grab_attr.vch          = 0;
    audio_grab_attr.sample_rate  = CFG_AUDIO_SAMPLERATE;
    audio_grab_attr.sample_size  = 16;
    audio_grab_attr.channel_type = GM_MONO;
    gm_set_attr(audio_grab_object, &audio_grab_attr);

    audio_enc_attr.encode_type   = GM_AAC;
    audio_enc_attr.bitrate        = CFG_AUDIO_BITRATE;
    audio_enc_attr.frame_samples  = CFG_AUDIO_FRAME_SAMPLES;
    gm_set_attr(audio_enc_object, &audio_enc_attr);

    audio_bindfd = gm_bind(audio_groupfd, audio_grab_object, audio_enc_object);

    if (gm_apply(audio_groupfd) < 0) {
        fprintf(stderr, "[FATAL] gm_apply(audio_groupfd) that bai! "
                "(Tiep tuc chay CHI VIDEO, khong audio)\n");
        gm_unbind(audio_bindfd);
        audio_bindfd = NULL;
    }

    return 0;
}

static void graph_release(void)
{
    if (venc_bindfd) {
        gm_unbind(venc_bindfd);
        gm_apply(video_groupfd);
    }
    if (audio_bindfd) {
        gm_unbind(audio_bindfd);
        gm_apply(audio_groupfd);
    }

    if (venc_object)        gm_delete_obj(venc_object);
    if (capture_object)     gm_delete_obj(capture_object);
    if (audio_enc_object)   gm_delete_obj(audio_enc_object);
    if (audio_grab_object)  gm_delete_obj(audio_grab_object);

    if (video_groupfd) gm_delete_groupfd(video_groupfd);
    if (audio_groupfd) gm_delete_groupfd(audio_groupfd);

    gm_release();
}

/* =======================================================================
 * MAIN
 * ===================================================================== */
static void sig_handler(int sig)
{
    (void) sig;
    g_running = 0;
}

int main(int argc, char *argv[])
{
    int ret;
    char hostbuf[32];

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    config_set_defaults(&g_cfg);

    /* ---- 1. Doc /tmp/sd/midgard.ini (uu tien THAP nhat, CLI de len tren) ---- */
    {
        char val[256];

        if (ini_get(CFG_INI_PATH, "RTSP_ENABLED", val, sizeof(val)) && atoi(val) == 0) {
            fprintf(stderr, "[CFG] RTSP_ENABLED=0 trong midgard.ini -> thoat.\n");
            return 0;
        }
        if (ini_get(CFG_INI_PATH, "MOTION_ENABLED", val, sizeof(val)))
            g_cfg.motion_detect = atoi(val);
        if (ini_get(CFG_INI_PATH, "SNAPSHOT_ENABLED", val, sizeof(val)))
            g_cfg.motion_snapshot = atoi(val);
        if (ini_get(CFG_INI_PATH, "RECORD_ENABLED", val, sizeof(val)))
            g_cfg.motion_record = atoi(val);
        if (ini_get(CFG_INI_PATH, "OSD_BG", val, sizeof(val))) {
            g_cfg.osd_bg_palette = atoi(val);
        }
        /* RTSP_ARGS la co che chinh de firmware truyen tham so cho rtspd -
         * ap dung SAU cac key rieng le o tren (co the ghi de lai), nhung
         * TRUOC CLI that (CLI luon la uu tien cao nhat). */
        if (ini_get(CFG_INI_PATH, "RTSP_ARGS", val, sizeof(val)))
            parse_rtsp_args_string(&g_cfg, val);
    }

    /* ---- 2. Tham so dong lenh THAT - uu tien cao nhat, ghi de tat ca ---- */
    config_parse_args(&g_cfg, argc, argv);

    if (g_cfg.osd_text[0] == '\0') {
        read_hostname(hostbuf, sizeof(hostbuf));
        if (hostbuf[0] != '\0')
            strncpy(g_cfg.osd_text, hostbuf, sizeof(g_cfg.osd_text) - 1);
    }

    {
        const char *u = getenv("RTSP_USER");
        const char *p = getenv("RTSP_PASS");
        if (u && u[0] != '\0' && p && p[0] != '\0') {
            strncpy(g_rtsp_user, u, sizeof(g_rtsp_user) - 1);
            strncpy(g_rtsp_pass, p, sizeof(g_rtsp_pass) - 1);
            g_rtsp_use_auth = 1;
        } else {
            /* Fallback: doc truc tiep tu ini neu script khoi dong khong
             * "export" bien nay (chi "source" thoi thi con rtspd KHONG
             * thay duoc qua getenv()). */
            char u2[64], p2[64];
            if (ini_get(CFG_INI_PATH, "RTSP_USER", u2, sizeof(u2)) &&
                ini_get(CFG_INI_PATH, "RTSP_PASS", p2, sizeof(p2))) {
                strncpy(g_rtsp_user, u2, sizeof(g_rtsp_user) - 1);
                strncpy(g_rtsp_pass, p2, sizeof(g_rtsp_pass) - 1);
                g_rtsp_use_auth = 1;
                fprintf(stderr, "[CFG] Dung RTSP_USER/PASS tu ini (khong "
                        "thay trong environment variables)\n");
            }
        }
    }

    printf("==== GM8136 2MP RTSP Daemon (SDK goc GrainMedia) ====\n");
    printf("Video : %dx%d @%dfps, mode=%d bitrate=%dkbps, codec=%s\n",
           g_cfg.width, g_cfg.height, g_cfg.framerate, g_cfg.mode,
           g_cfg.bitrate, enc_type_name[g_cfg.enc_type]);
    printf("Audio : AAC-LC %dHz mono, %dbps\n", CFG_AUDIO_SAMPLERATE, CFG_AUDIO_BITRATE);
    printf("OSD   : enable=%d text=\"%s\" zoom=%d bg_palette=%d\n",
           g_cfg.osd_enable, g_cfg.osd_text, g_cfg.osd_zoom, g_cfg.osd_bg_palette);
    printf("Motion: detect=%d snapshot=%d record=%d\n",
           g_cfg.motion_detect, g_cfg.motion_snapshot, g_cfg.motion_record);
    printf("Auth  : %s\n", g_rtsp_use_auth ? "enabled" : "disabled");

    if (graph_init() < 0)
        return 1;

    if (g_cfg.osd_enable)
        osd_setup_palette();

    if (g_cfg.motion_detect) {
        motion_detection_init();
        if (motion_setup() != 0) {
            fprintf(stderr, "[WARN] Motion Detect khong khoi tao duoc, tat tinh nang.\n");
            g_cfg.motion_detect = 0;
        }
    }

    pthread_create(&th_video, NULL, video_thread, NULL);
    if (audio_bindfd)
        pthread_create(&th_audio, NULL, audio_thread, NULL);
    if (g_cfg.motion_detect)
        pthread_create(&th_motion, NULL, motion_thread, NULL);
    if (g_cfg.osd_enable)
        pthread_create(&th_osd, NULL, osd_thread, NULL);

    {
        int waited_ms = 0;
        while (waited_ms < 5000) {
            int vid_ok = (g_vsdp[0] != '\0');
            int aud_ok = (!audio_bindfd) || (g_asdp[0] != '\0');
            if (vid_ok && aud_ok) break;
            usleep(50000);
            waited_ms += 50;
        }
        if (g_vsdp[0] == '\0')
            fprintf(stderr, "[WARN] Chua co SDP video sau 5s, van tiep tuc dang ky stream\n");
    }

    if ((ret = stream_server_init(NULL, RTSP_PORT, 0, 1444, 256,
                                   4, 64, VQ_LEN, 64, AQ_LEN, frm_cb, cmd_cb)) < 0) {
        fprintf(stderr, "[FATAL] stream_server_init loi %d\n", ret);
        goto cleanup;
    }
    if ((ret = stream_server_start()) < 0) {
        fprintf(stderr, "[FATAL] stream_server_start loi %d\n", ret);
        goto cleanup;
    }

    g_vqno = stream_queue_alloc(g_cfg.enc_type == ENC_MPEG4 ? GM_SS_TYPE_MP4 :
                                 g_cfg.enc_type == ENC_MJPEG ? GM_SS_TYPE_MJPEG :
                                 GM_SS_TYPE_H264);
    g_aqno = audio_bindfd ? stream_queue_alloc(GM_SS_TYPE_AAC) : -1;

    g_sr = stream_reg(STREAM_NAME, g_vqno, g_vsdp, g_aqno, g_asdp,
                       1, 0, 0, 0, 0, NULL, NULL);
    if (g_sr < 0) {
        fprintf(stderr, "[FATAL] stream_reg loi %d\n", g_sr);
        goto cleanup;
    }

    if (g_rtsp_use_auth) {
        stream_authorization(g_sr, g_rtsp_user, g_rtsp_pass);
        fprintf(stderr, "[RTSP] Da bat auth cho user=%s\n", g_rtsp_user);
    }

    printf("RTSP san sang: rtsp://%s:%d/%s\n", get_local_ip(), RTSP_PORT, STREAM_NAME);
    printf("Nhan Ctrl+C de dung.\n");

    while (g_running)
        sleep(1);

cleanup:
    fprintf(stderr, "Dang dung RTSP daemon...\n");
    g_running = 0;

    pthread_join(th_video, NULL);
    if (audio_bindfd) pthread_join(th_audio, NULL);
    if (g_cfg.motion_detect) pthread_join(th_motion, NULL);
    if (g_cfg.osd_enable) pthread_join(th_osd, NULL);

    if (g_sr >= 0) stream_dereg(g_sr, 1);
    stream_server_stop();

    pthread_mutex_lock(&g_rec_lock);
    if (g_rec_fp) { fclose(g_rec_fp); g_rec_fp = NULL; }
    pthread_mutex_unlock(&g_rec_lock);

    if (g_cfg.motion_detect) {
        motion_detection_end();
        if (g_mdt_alg.sub_region) free(g_mdt_alg.sub_region);
        if (g_mdt_result.sub_region) free(g_mdt_result.sub_region);
    }
    if (g_snapshot_buf) free(g_snapshot_buf);

    graph_release();
    return 0;
}
