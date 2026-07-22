/**
 * @file rtspd.c
 *  RTSP daemon cho camera GrainMedia GM8136 (2MP), dung SDK goc GrainMedia
 *  (gmlib.h / librtsp.h). Ho tro tham so dong lenh + file cau hinh
 *  /tmp/sd/midgard.ini, tuong thich cach dung cua rtspd2MP tren firmware
 *  Mijia hack.
 *
 * Tinh nang:
 *   - Video H.264 / MPEG4 / MJPEG, do phan giai/fps/bitrate/mode tuy chinh
 *   - Audio livesound G.711 A-law 8kHz mono (groupfd RIENG voi video - xem
 *     ghi chu quan trong o graph_init())
 *   - OSD: line1 = text tuy chinh (mac dinh = hostname), line2 = timestamp
 *     (bat/tat qua -o, zoom qua -z, mau nen qua -B)
 *   - Motion Detect (-d) -> snapshot (-s) + ghi clip 10s (-r)
 *   - RTSP server dung librtsp, ho tro nhieu client xem dong thoi
 *
 * Vi du:
 *   ./rtspd -b 4096 -f 20 -w 1920 -h 1080 -m 1 -o -t "CAM-01" -d -s -r
 *
 * Ket noi: rtsp://<ip-camera>:554/live/ch00_0
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
 * HANG SO CO DINH (khong doi qua tham so)
 * ===================================================================== */
#define CFG_INI_PATH          "/tmp/sd/midgard.ini"

#define CFG_AUDIO_SAMPLERATE  8000
#define CFG_AUDIO_FRAME_SAMPLES  320   /* G711 mono: 320*n, n=1 -> 40ms/frame */

#define CFG_SNAPSHOT_DIR      "/mnt/sd/snapshot"
#define CFG_RECORD_DIR        "/mnt/sd/record"
#define CFG_MOTION_CLIP_SEC   10   /* "-r: Record a 10 second clip on motion" */

#define RTSP_PORT             554
#define STREAM_NAME           "live/ch00_0"

#define SDPSTR_MAX            128
#define RTP_HZ                90000
#define AUDIO_BS_BUF_LEN      4096

/* =======================================================================
 * ENCODER TYPE (mac dinh H264; -j = MJPEG, -4 = MPEG4)
 * ===================================================================== */
typedef enum { ENC_H264 = 0, ENC_MPEG4 = 1, ENC_MJPEG = 2 } enc_type_t;

static const char *enc_type_name[] = { "H264", "MPEG4", "MJPEG" };

/* =======================================================================
 * CAU HINH (gia tri mac dinh -> ghi de boi ini -> ghi de boi CLI)
 * ===================================================================== */
typedef struct {
    int bitrate;        /* kbps, 1-8192, mac dinh 2048 */
    int framerate;       /* 1-20, mac dinh 20 */
    int width;            /* 1-1920, mac dinh 1920 */
    int height;            /* 1-1280, mac dinh 1280 */
    int mode;              /* 1-4 -> GM_CBR..GM_EVBR, mac dinh 1 */
    enc_type_t enc_type;   /* mac dinh ENC_H264 */

    int  osd_timestamp;    /* 0/1, mac dinh 1 (on) */
    char osd_text[64];     /* mac dinh = hostname */
    int  osd_zoom;         /* 0-4, mac dinh 0 */
    int  osd_bg_palette;   /* 0-15, mac dinh 1 */

    int motion_detect;     /* 0/1, mac dinh 0 (off) */
    int motion_snapshot;   /* 0/1, mac dinh 0 */
    int motion_record;     /* 0/1, mac dinh 0 */
} config_t;

static config_t g_cfg;

/* =======================================================================
 * BIEN TOAN CUC HE THONG / RTSP / GRAPH
 * ===================================================================== */
gm_system_t gm_system;

/* QUAN TRONG: video va audio KHONG duoc chung 1 groupfd tren SDK nay
 * (thuc te bao loi "audio function can't be group with video." khi
 * chung groupfd). Vi vay dung 2 groupfd doc lap. */
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

static pthread_t th_video, th_audio, th_motion, th_osdclock;

/* --- Recording (clip 10s theo motion) --- */
static FILE      *g_rec_fp = NULL;
static time_t      g_rec_end_time = 0;   /* thoi diem clip hien tai phai ket thuc */
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Motion detect state --- */
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
    char hostname[64];

    memset(c, 0, sizeof(*c));
    c->bitrate      = 2048;
    c->framerate     = 20;
    c->width          = 1920;
    c->height          = 1080;
    c->mode             = 1;
    c->enc_type          = ENC_H264;

    c->osd_timestamp      = 1;
    if (gethostname(hostname, sizeof(hostname)) == 0)
        strncpy(c->osd_text, hostname, sizeof(c->osd_text) - 1);
    else
        strncpy(c->osd_text, "Mijia-1080p", sizeof(c->osd_text) - 1);
    c->osd_zoom            = 0;
    c->osd_bg_palette      = 1;

    c->motion_detect        = 0;
    c->motion_snapshot      = 0;
    c->motion_record        = 0;
}

/* ------------------------- /tmp/sd/midgard.ini -------------------------
 * Dinh dang don gian "key = value" (khong phan biet hoa/thuong o key),
 * dong bat dau bang '#' hoac ';' la comment, dong [section] bi bo qua.
 * Cac key duoc ho tro (trung ten voi tham so CLI cho de nho):
 *   bitrate, framerate, width, height, mode, encoding (h264/mpeg4/mjpeg),
 *   osd_timestamp (0/1), osd_text, osd_zoom, osd_bg_palette,
 *   motion_detect (0/1), motion_snapshot (0/1), motion_record (0/1)
 * ------------------------------------------------------------------- */
static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char) *s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char) *end)) end--;
    end[1] = '\0';
    return s;
}

static void config_load_ini(config_t *c, const char *path)
{
    FILE *fp;
    char line[256], *p, *key, *val;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[CFG] Khong tim thay %s, dung gia tri mac dinh.\n", path);
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        p = trim(line);
        if (p[0] == '\0' || p[0] == '#' || p[0] == ';' || p[0] == '[')
            continue;

        val = strchr(p, '=');
        if (!val) continue;
        *val = '\0';
        key = trim(p);
        val = trim(val + 1);

        if (!strcasecmp(key, "bitrate"))            c->bitrate = atoi(val);
        else if (!strcasecmp(key, "framerate"))       c->framerate = atoi(val);
        else if (!strcasecmp(key, "width"))            c->width = atoi(val);
        else if (!strcasecmp(key, "height"))            c->height = atoi(val);
        else if (!strcasecmp(key, "mode"))               c->mode = atoi(val);
        else if (!strcasecmp(key, "encoding")) {
            if (!strcasecmp(val, "mjpeg"))     c->enc_type = ENC_MJPEG;
            else if (!strcasecmp(val, "mpeg4")) c->enc_type = ENC_MPEG4;
            else                                  c->enc_type = ENC_H264;
        }
        else if (!strcasecmp(key, "osd_timestamp"))      c->osd_timestamp = atoi(val);
        else if (!strcasecmp(key, "osd_text"))            strncpy(c->osd_text, val, sizeof(c->osd_text) - 1);
        else if (!strcasecmp(key, "osd_zoom"))             c->osd_zoom = atoi(val);
        else if (!strcasecmp(key, "osd_bg_palette"))        c->osd_bg_palette = atoi(val);
        else if (!strcasecmp(key, "motion_detect"))          c->motion_detect = atoi(val);
        else if (!strcasecmp(key, "motion_snapshot"))         c->motion_snapshot = atoi(val);
        else if (!strcasecmp(key, "motion_record"))            c->motion_record = atoi(val);
        else
            fprintf(stderr, "[CFG] Key khong ro trong %s: '%s'\n", path, key);
    }
    fclose(fp);
    fprintf(stderr, "[CFG] Da doc cau hinh tu %s\n", path);
}

#define CHECK_RANGE(val, lo, hi, optname) \
    do { if ((val) < (lo) || (val) > (hi)) { \
        fprintf(stderr, "Gia tri khong hop le cho -%s: %d (pham vi %d-%d)\n", \
                optname, (val), (lo), (hi)); \
        print_usage(); } } while (0)

static void config_parse_args(config_t *c, int argc, char *argv[])
{
    int opt;
    int osd_flag = 0;

    /* optstring: cac option co gia tri theo sau dau ':' */
    while ((opt = getopt(argc, argv, "b:f:w:h:m:jot:z:dsrB:4")) != -1) {
        switch (opt) {
            case 'b': c->bitrate = atoi(optarg); CHECK_RANGE(c->bitrate, 1, 8192, "b"); break;
            case 'f': c->framerate = atoi(optarg); CHECK_RANGE(c->framerate, 1, 20, "f"); break;
            case 'w': c->width = atoi(optarg); CHECK_RANGE(c->width, 1, 1920, "w"); break;
            case 'h': c->height = atoi(optarg); CHECK_RANGE(c->height, 1, 1280, "h"); break;
            case 'm': c->mode = atoi(optarg); CHECK_RANGE(c->mode, 1, 4, "m"); break;
            case 'j': c->enc_type = ENC_MJPEG; break;
            case '4': c->enc_type = ENC_MPEG4; break;
            case 'o': osd_flag = 1; break;   /* xem ghi chu duoi main() */
            case 't': strncpy(c->osd_text, optarg, sizeof(c->osd_text) - 1); break;
            case 'z': c->osd_zoom = atoi(optarg); CHECK_RANGE(c->osd_zoom, 0, 4, "z"); break;
            case 'd': c->motion_detect = 1; break;
            case 's': c->motion_snapshot = 1; break;
            case 'r': c->motion_record = 1; break;
            case 'B': c->osd_bg_palette = atoi(optarg); CHECK_RANGE(c->osd_bg_palette, 0, 15, "B"); break;
            default:
                print_usage();
        }
    }

    /* -o: "Enable OSD timestamp (default: on)" - ban than da mac dinh
     * bat san; co mat co -o tren dong lenh nghia la NGUOI DUNG CHU DONG
     * muon bat (vi du de ghi de ini da tat no) -> luon ep bat khi co -o. */
    if (osd_flag)
        c->osd_timestamp = 1;

    /* -s / -r chi co y nghia khi -d cung duoc bat (can du lieu motion) */
    if ((c->motion_snapshot || c->motion_record) && !c->motion_detect) {
        fprintf(stderr, "[WARN] -s/-r can -d (motion detection) de hoat dong, "
                "tu dong bat -d.\n");
        c->motion_detect = 1;
    }
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

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    return inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
}

/* =======================================================================
 * OSD: line1 = text tuy chinh, line2 = timestamp
 * ===================================================================== */
#define OSD_PALETTE_COLOR_WHITE   0xEB80EB80
#define OSD_PALETTE_COLOR_BLACK   0x10801080

static void osd_setup_palette(void)
{
    gm_palette_table_t palette;
    int i;

    memset(&palette, 0, sizeof(palette));
    for (i = 0; i < 16; i++)
        palette.palette_table[i] = OSD_PALETTE_COLOR_BLACK;
    palette.palette_table[0] = OSD_PALETTE_COLOR_WHITE; /* idx0 = font */
    /* idx cua nen lay tu g_cfg.osd_bg_palette (-B), gan them 1 gia tri
     * den mac dinh phong khi nguoi dung chon idx chua duoc gan mau khac */
    if (g_cfg.osd_bg_palette >= 0 && g_cfg.osd_bg_palette < 16)
        palette.palette_table[g_cfg.osd_bg_palette] = OSD_PALETTE_COLOR_BLACK;

    if (gm_set_palette_table(&palette) < 0)
        fprintf(stderr, "[OSD] gm_set_palette_table loi\n");
}

static void osd_set_line(int win_idx, int x, int y, const char *text)
{
    gm_osd_font_t f;
    int i, len;

    memset(&f, 0, sizeof(f));
    len = (int) strlen(text);
    if (len > GM_MAX_OSD_FONTS)
        len = GM_MAX_OSD_FONTS;

    f.win_idx     = win_idx;
    f.enabled     = 1;
    f.align_type  = GM_OSD_ALIGN_TOP_LEFT;
    f.x           = x;
    f.y           = y;
    f.h_words     = len;
    f.v_words     = 1;
    f.h_space     = 0;
    f.v_space     = 0;
    f.font_index_len = len;
    for (i = 0; i < len; i++)
        f.font_index[i] = (unsigned short) text[i];

    f.font_alpha       = GM_OSD_FONT_ALPHA_100;
    f.win_alpha        = GM_OSD_FONT_ALPHA_50;
    f.win_palette_idx  = g_cfg.osd_bg_palette;
    f.font_palette_idx = 0;
    f.priority         = GM_OSD_PRIORITY_MARK_ON_OSD;
    f.smooth.enabled   = 1;
    f.smooth.level     = GM_OSD_FONT_SMOOTH_LEVEL_WEAK;
    f.marquee.mode     = GM_OSD_MARQUEE_MODE_NONE;
    f.border.enabled   = 1;
    f.border.width     = 0;
    f.border.type      = GM_OSD_BORDER_TYPE_WIN;
    f.border.palette_idx = g_cfg.osd_bg_palette;
    f.font_zoom        = (gm_osd_font_zoom_t) g_cfg.osd_zoom; /* 0-4 anh xa truc tiep */

    if (gm_set_osd_font(capture_object, &f) < 0)
        fprintf(stderr, "[OSD] gm_set_osd_font win_idx=%d loi\n", win_idx);
}

static void osd_clear_line(int win_idx)
{
    gm_osd_font_t f;
    memset(&f, 0, sizeof(f));
    f.win_idx = win_idx;
    f.enabled = 0;
    gm_set_osd_font(capture_object, &f);
}

static void *osd_clock_thread(void *arg)
{
    (void) arg;
    char buf[32];
    time_t t;
    struct tm tmv;
    int line2_y = 16 + 28;

    if (!g_cfg.osd_timestamp)
        return NULL;

    while (g_running) {
        t = time(NULL);
        localtime_r(&t, &tmv);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        osd_set_line(1, 16, line2_y, buf);
        sleep(1);
    }
    return NULL;
}

static void osd_init(void)
{
    osd_setup_palette();
    osd_set_line(0, 16, 16, g_cfg.osd_text);
    if (!g_cfg.osd_timestamp)
        osd_clear_line(1);
    /* Neu bat, line2 se duoc ve lan dau ngay trong osd_clock_thread */
}

/* =======================================================================
 * SNAPSHOT
 * Gioi han phan cung GM8136: bs_width 128~720, bs_height 96~576.
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
    snap.bs_width        = 640;
    snap.bs_height        = 480;

    len = gm_request_snapshot(&snap, 800);
    if (len <= 0) {
        fprintf(stderr, "[SNAP] gm_request_snapshot that bai (len=%d)\n", len);
        return NULL;
    }

    ensure_dir(CFG_SNAPSHOT_DIR);
    build_timestamped_path(path, sizeof(path), CFG_SNAPSHOT_DIR, "snap", "jpg");
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[SNAP] khong the tao file %s\n", path);
        return NULL;
    }
    fwrite(g_snapshot_buf, 1, len, fp);
    fclose(fp);
    fprintf(stderr, "[SNAP] da luu %s (%d bytes)\n", path, len);
    return path;
}

/* =======================================================================
 * RECORDING: ghi clip co dinh CFG_MOTION_CLIP_SEC giay khi co motion (-r)
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
    if (!g_rec_fp) {
        fprintf(stderr, "[REC] khong the tao file %s\n", path);
        return;
    }
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
        recorder_start_clip();   /* moi lan co dong moi -> lam moi clip 10s */
        pthread_mutex_unlock(&g_rec_lock);
    }
}

static void on_motion_stop(void)
{
    fprintf(stderr, "[MD] het chuyen dong\n");
    /* Clip van tiep tuc chay du CFG_MOTION_CLIP_SEC giay ke tu luc bat dau,
     * xu ly boi recorder_feed(), khong can lam gi them o day. */
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

    if (!g_cfg.motion_detect)
        return NULL;

    memset(&cap_md, 0, sizeof(cap_md));
    cap_md.bindfd = venc_bindfd;
    cap_md.cap_md_info.md_buf_len = CAP_MOTION_SIZE;
    cap_md.cap_md_info.md_buf = (char *) malloc(CAP_MOTION_SIZE);
    if (!cap_md.cap_md_info.md_buf) {
        fprintf(stderr, "[MD] khong du bo nho cho md_buf\n");
        return NULL;
    }

    while (g_running) {
        ret = gm_recv_multi_cap_md(&cap_md, 1);
        if (ret < 0) {
            usleep(50000);
            continue;
        }
        ret = motion_detection_handling(&cap_md, &g_mdt_result, 1);
        if (ret < 0) {
            fprintf(stderr, "[MD] motion_detection_handling loi\n");
            continue;
        }

        if (g_mdt_result.ch_result == MOTION_IS_READY) {
            cur_motion = (g_mdt_result.sub_region[0].reg_result == MOTION_DETECTED) ? 1 : 0;
            if (cur_motion && !prev_motion)
                on_motion_start();
            else if (!cur_motion && prev_motion)
                on_motion_stop();
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
    char *bs_buf;
    int bs_buf_len;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];
    gm_ss_entity entity;
    int ret, sdp_ready = 0;
    int ss_type;

    bs_buf_len = g_cfg.width * g_cfg.height * 3 / 2;
    bs_buf = (char *) malloc(bs_buf_len);
    if (!bs_buf) {
        fprintf(stderr, "[VID] khong du bo nho cho bitstream buffer\n");
        return NULL;
    }

    switch (g_cfg.enc_type) {
        case ENC_MPEG4: ss_type = GM_SS_TYPE_MP4; break;
        case ENC_MJPEG: ss_type = GM_SS_TYPE_MJPEG; break;
        default:         ss_type = GM_SS_TYPE_H264; break;
    }

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = venc_bindfd;
    poll_fds[0].event  = GM_POLL_READ;

    while (g_running) {
        ret = gm_poll(poll_fds, 1, 500);
        if (ret == GM_TIMEOUT) continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        if (poll_fds[0].revent.event != GM_POLL_READ) continue;
        if ((int) poll_fds[0].revent.bs_len > bs_buf_len) {
            fprintf(stderr, "[VID] buffer khong du: %u > %d\n",
                    poll_fds[0].revent.bs_len, bs_buf_len);
            continue;
        }
        multi_bs[0].bindfd = venc_bindfd;
        multi_bs[0].bs.bs_buf = bs_buf;
        multi_bs[0].bs.bs_buf_len = bs_buf_len;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        if (gm_recv_multi_bitstreams(multi_bs, 1) < 0) continue;
        if (multi_bs[0].retval != GM_SUCCESS) continue;

        if (!sdp_ready) {
            /* MJPEG: moi frame la keyframe. H264/MPEG4: cho I-frame dau. */
            if (g_cfg.enc_type != ENC_MJPEG && !multi_bs[0].bs.keyframe)
                continue;
            stream_sdp_parameter_encoder((char *) enc_type_name[g_cfg.enc_type],
                                          (unsigned char *) multi_bs[0].bs.bs_buf,
                                          multi_bs[0].bs.bs_len,
                                          g_vsdp, SDPSTR_MAX);
            sdp_ready = 1;
            fprintf(stderr, "[VID] Da sinh SDP video (%s), sdp_ready\n",
                    enc_type_name[g_cfg.enc_type]);
            continue;
        }

        if (g_cfg.motion_record)
            recorder_feed(multi_bs[0].bs.bs_buf, multi_bs[0].bs.bs_len);

        if (g_play && g_vqno >= 0) {
            entity.data = multi_bs[0].bs.bs_buf;
            entity.size = multi_bs[0].bs.bs_len;
            entity.timestamp = multi_bs[0].bs.timestamp * (RTP_HZ / 1000);
            stream_media_enqueue(ss_type, g_vqno, &entity);
        }
    }

    free(bs_buf);
    return NULL;
}

/* =======================================================================
 * AUDIO ENCODE THREAD
 * ===================================================================== */
static void *audio_thread(void *arg)
{
    (void) arg;
    char *bs_buf;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];
    gm_ss_entity entity;
    int ret;

    bs_buf = (char *) malloc(AUDIO_BS_BUF_LEN);
    if (!bs_buf) return NULL;

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = audio_bindfd;
    poll_fds[0].event  = GM_POLL_READ;

    while (g_running) {
        ret = gm_poll(poll_fds, 1, 500);
        if (ret == GM_TIMEOUT) continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        if (poll_fds[0].revent.event != GM_POLL_READ) continue;
        if ((int) poll_fds[0].revent.bs_len > AUDIO_BS_BUF_LEN) continue;

        multi_bs[0].bindfd = audio_bindfd;
        multi_bs[0].bs.bs_buf = bs_buf;
        multi_bs[0].bs.bs_buf_len = AUDIO_BS_BUF_LEN;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        if (gm_recv_multi_bitstreams(multi_bs, 1) < 0) continue;
        if (multi_bs[0].retval != GM_SUCCESS) continue;

        if (g_play && g_aqno >= 0) {
            entity.data = multi_bs[0].bs.bs_buf;
            entity.size = multi_bs[0].bs.bs_len;
            entity.timestamp = multi_bs[0].bs.timestamp * (RTP_HZ / 1000);
            stream_media_enqueue(GM_SS_TYPE_G711A, g_aqno, &entity);
        }
    }

    free(bs_buf);
    return NULL;
}

/* =======================================================================
 * CALLBACK CUA LIBRTSP
 * ===================================================================== */
static int frm_cb(int type, int qno, gm_ss_entity *entity)
{
    (void) type; (void) qno; (void) entity;
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

    /* Neu do phan giai yeu cau vuot qua sensor that thi ha xuong dung
     * kich thuoc sensor bao cao qua gm_get_sysinfo() de tranh gm_apply() loi. */
    if (g_cfg.width > gm_system.cap[0].dim.width ||
        g_cfg.height > gm_system.cap[0].dim.height) {
        fprintf(stderr, "[WARN] Yeu cau %dx%d vuot qua sensor that (%dx%d), "
                "dung do phan giai sensor.\n",
                g_cfg.width, g_cfg.height,
                gm_system.cap[0].dim.width, gm_system.cap[0].dim.height);
        g_cfg.width  = gm_system.cap[0].dim.width;
        g_cfg.height = gm_system.cap[0].dim.height;
    }

    rc_mode = (gm_enc_ratecontrol_mode_t) g_cfg.mode; /* 1=CBR 2=VBR 3=ECBR 4=EVBR */

    /* -------------------------------------------------------------
     * GROUPFD RIENG CHO VIDEO. QUAN TRONG: khong duoc bind audio vao
     * chung groupfd nay, SDK GM8136 tra loi loi
     * "Error! audio function can't be group with video." neu lam vay.
     * ------------------------------------------------------------- */
    video_groupfd = gm_new_groupfd();

    capture_object = gm_new_obj(GM_CAP_OBJECT);
    venc_object    = gm_new_obj(GM_ENCODER_OBJECT);

    cap_attr.cap_vch = MD_CH;
    cap_attr.path = 3;   /* GM8136/GM8139: 0 liveview, 1/2 substream, 3 mainstream */
    cap_attr.enable_mv_data = g_cfg.motion_detect ? 1 : 0;
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
            mpeg4e_attr.ratectl.gop          = g_cfg.framerate * 2;
            mpeg4e_attr.ratectl.bitrate       = g_cfg.bitrate;
            mpeg4e_attr.ratectl.bitrate_max    = g_cfg.bitrate;
            gm_set_attr(venc_object, &mpeg4e_attr);
            break;
        case ENC_MJPEG:
            mjpege_attr.dim.width  = g_cfg.width;
            mjpege_attr.dim.height = g_cfg.height;
            mjpege_attr.frame_info.framerate = g_cfg.framerate;
            mjpege_attr.quality = 70;
            mjpege_attr.mode = rc_mode;
            mjpege_attr.bitrate = g_cfg.bitrate;
            mjpege_attr.bitrate_max = g_cfg.bitrate;
            gm_set_attr(venc_object, &mjpege_attr);
            break;
        default: /* ENC_H264 */
            h264e_attr.dim.width  = g_cfg.width;
            h264e_attr.dim.height = g_cfg.height;
            h264e_attr.frame_info.framerate = g_cfg.framerate;
            h264e_attr.ratectl.mode         = rc_mode;
            h264e_attr.ratectl.gop          = g_cfg.framerate * 2;
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

    /* -------------------------------------------------------------
     * GROUPFD RIENG CHO AUDIO (xem ghi chu tren)
     * ------------------------------------------------------------- */
    audio_groupfd = gm_new_groupfd();

    audio_grab_object = gm_new_obj(GM_AUDIO_GRAB_OBJECT);
    audio_enc_object   = gm_new_obj(GM_AUDIO_ENCODER_OBJECT);

    audio_grab_attr.vch          = 0;
    audio_grab_attr.sample_rate  = CFG_AUDIO_SAMPLERATE;
    audio_grab_attr.sample_size  = 16;
    audio_grab_attr.channel_type = GM_MONO;
    gm_set_attr(audio_grab_object, &audio_grab_attr);

    audio_enc_attr.encode_type   = GM_G711_ALAW;
    audio_enc_attr.bitrate        = 64000;
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

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    config_set_defaults(&g_cfg);
    config_load_ini(&g_cfg, CFG_INI_PATH);
    config_parse_args(&g_cfg, argc, argv);

    printf("==== GM8136 2MP RTSP Daemon (SDK goc GrainMedia) ====\n");
    printf("Video : %dx%d @%dfps, %s mode=%d bitrate=%dkbps GOP=%d, codec=%s\n",
           g_cfg.width, g_cfg.height, g_cfg.framerate,
           (g_cfg.mode == 1) ? "CBR" : (g_cfg.mode == 2) ? "VBR" :
           (g_cfg.mode == 3) ? "ECBR" : "EVBR",
           g_cfg.mode, g_cfg.bitrate, g_cfg.framerate * 2,
           enc_type_name[g_cfg.enc_type]);
    printf("Audio : G.711A %dHz mono (groupfd rieng voi video)\n", CFG_AUDIO_SAMPLERATE);
    printf("OSD   : text=\"%s\" timestamp=%s zoom=%d bg_palette=%d\n",
           g_cfg.osd_text, g_cfg.osd_timestamp ? "on" : "off",
           g_cfg.osd_zoom, g_cfg.osd_bg_palette);
    printf("Motion: detect=%s snapshot=%s record=%s\n",
           g_cfg.motion_detect ? "on" : "off",
           g_cfg.motion_snapshot ? "on" : "off",
           g_cfg.motion_record ? "on" : "off");

    if (graph_init() < 0)
        return 1;

    osd_init();

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
    if (g_cfg.osd_timestamp)
        pthread_create(&th_osdclock, NULL, osd_clock_thread, NULL);

    {
        int waited_ms = 0;
        while (g_vsdp[0] == '\0' && waited_ms < 5000) {
            usleep(50000);
            waited_ms += 50;
        }
    }

    if ((ret = stream_server_init(NULL, RTSP_PORT, 0, 1444, 256,
                                   4, 64, 5, 64, 4, frm_cb, cmd_cb)) < 0) {
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
    g_aqno = audio_bindfd ? stream_queue_alloc(GM_SS_TYPE_G711A) : -1;

    /* G.711A la static RTP payload type (PT=8, PCMA/8000), khong can
     * stream_sdp_parameter_encoder(). Neu track audio khong hien tren
     * TinyCam/SmartRTSP, thu dat g_asdp = "a=rtpmap:8 PCMA/8000\r\n". */
    g_asdp[0] = '\0';

    g_sr = stream_reg(STREAM_NAME, g_vqno, g_vsdp,
                       g_aqno, g_asdp,
                       1, 0, 0, 0, 0, NULL, NULL);
    if (g_sr < 0) {
        fprintf(stderr, "[FATAL] stream_reg loi %d\n", g_sr);
        goto cleanup;
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
    if (g_cfg.osd_timestamp) pthread_join(th_osdclock, NULL);

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
