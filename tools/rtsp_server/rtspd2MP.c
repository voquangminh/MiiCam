/**
 * @file rtspd.c
 *  RTSP daemon cho camera GrainMedia GM8136, profile 2MP.
 *  Bien soan dua tren mau chinh hang gm_graph/product/GM8136_2MP/samples/rtspd.c
 *  va cac sample chinh hang khac trong gm_lib/samples/ (encode_with_osd.c,
 *  encode_with_snapshot.c, encode_with_capture_motion_detection2.c,
 *  audio_record.c) - dung DUNG API that cua SDK (gmlib.h / librtsp.h),
 *  khong con la HAL gia lap.
 *
 * Tinh nang:
 *   - Video H.264 1920x1280 @20fps, CBR bitrate chuan (4096 kbps)
 *   - Audio livesound G.711 A-law 8kHz mono, ghep chung 1 RTSP stream voi video
 *   - OSD 2 dong: line1 = text tuy chinh, line2 = timestamp (cap nhat moi giay)
 *   - Motion Detect (thuat toan chinh hang capture_motion_detection2) ->
 *     tu dong snapshot + bat/tat ghi hinh
 *   - Snapshot JPEG theo yeu cau (gioi han phan cung: toi da 640x480/D1,
 *     xem [SNAPSHOT] trong gmlib.cfg cua GM8136_2MP)
 *   - Ghi hinh H.264 ra file, chia segment tai bien I-frame
 *   - RTSP server dung librtsp (thu vien rieng cua GM, ho tro nhieu client
 *     xem dong thoi tren cung 1 hang doi -> on dinh cho TinyCam/SmartRTSP)
 *
 * Build: xem Makefile di kem (dung toolchain uclibc ARMv5TE cua nha san xuat)
 *
 * Ket noi: rtsp://<ip-camera>:554/live/ch00_0
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
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
/* Module thuat toan Motion Detect chinh hang - dung dung quy uoc cua
 * chinh vendor (#include truc tiep file .c, xem
 * gm_lib/samples/encode_with_capture_motion_detection2.c) */
#include "algorithm/capture_motion_detection2.c"

/* =======================================================================
 * CAU HINH THEO YEU CAU DE BAI
 * ===================================================================== */
#define CFG_WIDTH            1920
#define CFG_HEIGHT           1280
#define CFG_FPS              20
#define CFG_BITRATE_KBPS     4096      /* bitrate CBR "chuan" cho 2MP@20fps */
#define CFG_GOP              (CFG_FPS * 2)   /* I-frame moi 2 giay */

#define CFG_AUDIO_SAMPLERATE 8000
#define CFG_AUDIO_FRAME_SAMPLES  320   /* G711 mono: 320*n, n=1 -> 40ms/frame */

#define CFG_OSD_LINE1_TEXT   "Mijia-1080p"
#define CFG_OSD_X            16
#define CFG_OSD_Y            16
#define CFG_OSD_LINE_GAP     28

#define CFG_SNAPSHOT_DIR     "/tmp/sd/snapshot"
#define CFG_RECORD_DIR       "/tmp/sd/record"
#define CFG_RECORD_SEGMENT_SEC      300   /* 5 phut / file */
#define CFG_RECORD_ON_MOTION_ONLY   1     /* 0 = ghi 24/7, 1 = chi khi co dong */
#define CFG_MOTION_POST_REC_SEC     10    /* tiep tuc ghi bao lau sau khi het dong */

#define RTSP_PORT            554
#define STREAM_NAME          "live/ch00_0"

#define VIDEO_BS_BUF_LEN     (CFG_WIDTH * CFG_HEIGHT * 3 / 2)
#define AUDIO_BS_BUF_LEN     4096
#define SDPSTR_MAX           128
#define RTP_HZ               90000

/* =======================================================================
 * BIEN TOAN CUC
 * ===================================================================== */
gm_system_t gm_system;
void *groupfd;                 /* gm_new_groupfd() */

void *capture_object;          /* GM_CAP_OBJECT (dung chung cho video + OSD + MD) */
void *venc_object;             /* GM_ENCODER_OBJECT (H.264) */
void *venc_bindfd;             /* gm_bind(capture, venc) */

void *audio_grab_object;       /* GM_AUDIO_GRAB_OBJECT */
void *audio_enc_object;        /* GM_AUDIO_ENCODER_OBJECT */
void *audio_bindfd;            /* gm_bind(audio_grab, audio_enc) */

static int   g_vqno = -1, g_aqno = -1;   /* librtsp queue handle */
static int   g_sr = -1;                  /* librtsp stream registration handle */
static char  g_vsdp[SDPSTR_MAX];
static char  g_asdp[SDPSTR_MAX];
static volatile int g_play = 0;
static volatile int g_running = 1;

static pthread_t th_video, th_audio, th_motion, th_osdclock;

/* --- Recording state --- */
static FILE      *g_rec_fp = NULL;
static time_t      g_rec_start_time = 0;
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_motion_active = 0;
static time_t       g_motion_last_time = 0;

/* --- Motion detect state (theo mau capture_motion_detection2) --- */
#define MD_CH               0
#define MD_MB_SIZE          32
struct mdt_alg_t    g_mdt_alg    = { sub_region: NULL };
struct mdt_result_t g_mdt_result = { sub_region: NULL };

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
 * (theo mau gm_lib/samples/encode_with_osd.c: update_osd_font())
 * ===================================================================== */
#define OSD_PALETTE_COLOR_WHITE   0xEB80EB80   /* YCrYCb, chu trang */
#define OSD_PALETTE_COLOR_BLACK   0x10801080   /* nen den */

static void osd_setup_palette(void)
{
    gm_palette_table_t palette;
    int i;

    memset(&palette, 0, sizeof(palette));
    for (i = 0; i < 16; i++)
        palette.palette_table[i] = OSD_PALETTE_COLOR_BLACK;
    palette.palette_table[0] = OSD_PALETTE_COLOR_WHITE; /* idx0 = font */
    palette.palette_table[1] = OSD_PALETTE_COLOR_BLACK; /* idx1 = nen/border */

    if (gm_set_palette_table(&palette) < 0)
        fprintf(stderr, "[OSD] gm_set_palette_table loi\n");
}

/* Ve 1 dong text ASCII vao cua so OSD win_idx, tai vi tri (x,y) */
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
        f.font_index[i] = (unsigned short) text[i];   /* font ASCII co san trong chip */

    f.font_alpha       = GM_OSD_FONT_ALPHA_100;
    f.win_alpha        = GM_OSD_FONT_ALPHA_50;   /* nen ban trong suot de de doc */
    f.win_palette_idx  = 1;
    f.font_palette_idx = 0;
    f.priority         = GM_OSD_PRIORITY_MARK_ON_OSD;
    f.smooth.enabled   = 1;
    f.smooth.level     = GM_OSD_FONT_SMOOTH_LEVEL_WEAK;
    f.marquee.mode     = GM_OSD_MARQUEE_MODE_NONE;
    f.border.enabled   = 1;
    f.border.width     = 0;
    f.border.type      = GM_OSD_BORDER_TYPE_WIN;
    f.border.palette_idx = 1;
    f.font_zoom        = GM_OSD_FONT_ZOOM_NONE;

    if (gm_set_osd_font(capture_object, &f) < 0)
        fprintf(stderr, "[OSD] gm_set_osd_font win_idx=%d loi\n", win_idx);
}

/* Thread rieng: cap nhat line2 = timestamp moi giay (chip khong co OSD
 * timestamp tu dong, phai ve lai bang tay dinh ky - day la cach lam
 * chuan cho dong SDK GM8136/GM8139) */
static void *osd_clock_thread(void *arg)
{
    (void) arg;
    char buf[32];
    time_t t;
    struct tm tmv;

    while (g_running) {
        t = time(NULL);
        localtime_r(&t, &tmv);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        osd_set_line(1, CFG_OSD_X, CFG_OSD_Y + CFG_OSD_LINE_GAP, buf);
        sleep(1);
    }
    return NULL;
}

static void osd_init(void)
{
    osd_setup_palette();
    osd_set_line(0, CFG_OSD_X, CFG_OSD_Y, CFG_OSD_LINE1_TEXT);
    /* line2 (timestamp) duoc ve lan dau ngay trong osd_clock_thread */
}

/* =======================================================================
 * SNAPSHOT (theo mau gm_lib/samples/encode_with_snapshot.c)
 * Gioi han phan cung GM8136: bs_width 128~720, bs_height 96~576 (xem
 * [SNAPSHOT] trong gmlib.cfg: yuv_max_width=640 yuv_max_height=480).
 * Khong the chup JPEG full 1920x1280 truc tiep qua API nay.
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
    snap.bs_width        = 640;   /* gioi han phan cung, xem ghi chu tren */
    snap.bs_height        = 480;

    len = gm_request_snapshot(&snap, 800 /* ms timeout */);
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
 * RECORDING: ghi bitstream H.264 ra file, chia segment tai bien I-frame
 * ===================================================================== */
static void recorder_start_segment(void)
{
    char path[256];
    ensure_dir(CFG_RECORD_DIR);
    build_timestamped_path(path, sizeof(path), CFG_RECORD_DIR, "rec", "h264");
    if (g_rec_fp) fclose(g_rec_fp);
    g_rec_fp = fopen(path, "wb");
    if (!g_rec_fp) {
        fprintf(stderr, "[REC] khong the tao file %s\n", path);
        return;
    }
    g_rec_start_time = time(NULL);
    fprintf(stderr, "[REC] bat dau ghi: %s\n", path);
}

static void recorder_stop(void)
{
    if (g_rec_fp) {
        fclose(g_rec_fp);
        g_rec_fp = NULL;
        fprintf(stderr, "[REC] dung ghi\n");
    }
}

/* Goi tu video thread cho MOI frame nhan duoc tu VENC */
static void recorder_feed(const char *data, int len, int is_keyframe)
{
    pthread_mutex_lock(&g_rec_lock);

    if (CFG_RECORD_ON_MOTION_ONLY && g_rec_fp && !g_motion_active) {
        if ((time(NULL) - g_motion_last_time) >= CFG_MOTION_POST_REC_SEC)
            recorder_stop();
    }

    if (g_rec_fp) {
        if (is_keyframe && CFG_RECORD_SEGMENT_SEC > 0 &&
            (time(NULL) - g_rec_start_time) >= CFG_RECORD_SEGMENT_SEC) {
            recorder_start_segment();
        }
        fwrite(data, 1, len, g_rec_fp);
    }

    pthread_mutex_unlock(&g_rec_lock);
}

/* Goi tu motion thread khi bat dau / ket thuc chuyen dong */
static void on_motion_start(void)
{
    fprintf(stderr, "[MD] phat hien chuyen dong\n");
    g_motion_active = 1;
    g_motion_last_time = time(NULL);
    take_snapshot();

    pthread_mutex_lock(&g_rec_lock);
    if (CFG_RECORD_ON_MOTION_ONLY && !g_rec_fp)
        recorder_start_segment();
    pthread_mutex_unlock(&g_rec_lock);
}

static void on_motion_stop(void)
{
    fprintf(stderr, "[MD] het chuyen dong\n");
    g_motion_active = 0;
    g_motion_last_time = time(NULL);
    /* Viec dong file duoc recorder_feed() xu ly sau CFG_MOTION_POST_REC_SEC */
}

/* =======================================================================
 * MOTION DETECT (theo mau encode_with_capture_motion_detection2.c)
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

    g_mdt_alg.u_width      = CFG_WIDTH;
    g_mdt_alg.u_height     = CFG_HEIGHT;
    g_mdt_alg.u_mb_width   = MD_MB_SIZE;
    g_mdt_alg.u_mb_height  = MD_MB_SIZE;
    g_mdt_alg.training_time = 15;
    g_mdt_alg.frame_count   = 0;
    g_mdt_alg.sensitive_th  = 80;   /* do nhay, 0~100 */

    mb_w_num = (g_mdt_alg.u_width  + (MD_MB_SIZE - 1)) / MD_MB_SIZE;
    mb_h_num = (g_mdt_alg.u_height + (MD_MB_SIZE - 1)) / MD_MB_SIZE;
    g_mdt_alg.mb_w_num = mb_w_num;
    g_mdt_alg.mb_h_num = mb_h_num;

    /* 1 vung quan tam duy nhat = toan bo khung hinh */
    g_mdt_alg.sub_region[0].is_enabled    = 1;
    g_mdt_alg.sub_region[0].start_block_x = 0;
    g_mdt_alg.sub_region[0].start_block_y = 0;
    g_mdt_alg.sub_region[0].end_block_x   = mb_w_num - 1;
    g_mdt_alg.sub_region[0].end_block_y   = mb_h_num - 1;
    g_mdt_alg.sub_region[0].alarm_th      = 80;
    g_mdt_alg.sub_region[0].alarm         = NO_MOTION;
    g_mdt_alg.sub_region_num = 1;

    /* Tham so tinh chinh thuat toan - gia tri khuyen nghi cua vendor
     * (xem gm_lib/samples/encode_with_capture_motion_detection2.c) */
    set_cap_motion(MD_CH, 0, 32);       /* alpha */
    set_cap_motion(MD_CH, 1, 7371);     /* tbg */
    set_cap_motion(MD_CH, 2, 7);        /* init val */
    set_cap_motion(MD_CH, 3, 9);        /* tb */
    set_cap_motion(MD_CH, 4, 11);       /* sigma */
    set_cap_motion(MD_CH, 5, 15);       /* prune */
    set_cap_motion(MD_CH, 7, 0x9ffb0);  /* alpha accuracy */
    set_cap_motion(MD_CH, 8, 9);        /* tg */
    set_cap_motion(MD_CH, 10, 0x7fe0);  /* one min alpha */

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
        /* MOTION_IS_TRAINING: dang trong giai doan hoc nen, bo qua */

        usleep(200000); /* ~5 lan/giay, du nhanh cho camera giam sat */
    }

    free(cap_md.cap_md_info.md_buf);
    return NULL;
}

/* =======================================================================
 * VIDEO ENCODE THREAD: poll + nhan bitstream H.264, day vao RTSP + recorder
 * ===================================================================== */
static void *video_thread(void *arg)
{
    (void) arg;
    char *bs_buf;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];
    gm_ss_entity entity;
    int ret, sdp_ready = 0;

    bs_buf = (char *) malloc(VIDEO_BS_BUF_LEN);
    if (!bs_buf) {
        fprintf(stderr, "[VID] khong du bo nho cho bitstream buffer\n");
        return NULL;
    }

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = venc_bindfd;
    poll_fds[0].event  = GM_POLL_READ;

    while (g_running) {
        ret = gm_poll(poll_fds, 1, 500);
        if (ret == GM_TIMEOUT) continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        if (poll_fds[0].revent.event != GM_POLL_READ) continue;
        if (poll_fds[0].revent.bs_len > VIDEO_BS_BUF_LEN) {
            fprintf(stderr, "[VID] buffer khong du: %u > %d\n",
                    poll_fds[0].revent.bs_len, VIDEO_BS_BUF_LEN);
            continue;
        }
        multi_bs[0].bindfd = venc_bindfd;
        multi_bs[0].bs.bs_buf = bs_buf;
        multi_bs[0].bs.bs_buf_len = VIDEO_BS_BUF_LEN;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        if (gm_recv_multi_bitstreams(multi_bs, 1) < 0) continue;
        if (multi_bs[0].retval != GM_SUCCESS) continue;

        /* Lan dau tien nhan duoc I-frame -> sinh SDP roi dang ky RTSP stream.
         * (Giong het co che update_video_sdp() trong rtspd.c goc cua vendor) */
        if (!sdp_ready) {
            if (!multi_bs[0].bs.keyframe) continue;
            stream_sdp_parameter_encoder("H264",
                                          (unsigned char *) multi_bs[0].bs.bs_buf,
                                          multi_bs[0].bs.bs_len,
                                          g_vsdp, SDPSTR_MAX);
            sdp_ready = 1;
            fprintf(stderr, "[VID] Da sinh SDP video, sdp_ready\n");
            continue; /* dang ky stream se do main() dam nhiem, bo qua frame nay */
        }

        /* Ghi hinh (doc lap voi viec co client RTSP dang xem hay khong) */
        recorder_feed(multi_bs[0].bs.bs_buf, multi_bs[0].bs.bs_len,
                       multi_bs[0].bs.keyframe);

        /* Day vao hang doi RTSP neu da co client PLAY */
        if (g_play && g_vqno >= 0) {
            entity.data = multi_bs[0].bs.bs_buf;
            entity.size = multi_bs[0].bs.bs_len;
            entity.timestamp = multi_bs[0].bs.timestamp * (RTP_HZ / 1000);
            stream_media_enqueue(GM_SS_TYPE_H264, g_vqno, &entity);
        }
    }

    free(bs_buf);
    return NULL;
}

/* =======================================================================
 * AUDIO ENCODE THREAD: poll + nhan bitstream G.711A, day vao RTSP
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
        if (poll_fds[0].revent.bs_len > AUDIO_BS_BUF_LEN) continue;

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
    /* librtsp bao buffer da duoc tieu thu xong; do bs_buf cua ta la buffer
     * rieng cua tung thread (khong dung chung), khong can xu ly gi them. */
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
 * KHOI TAO / GIAI PHONG GRAPH (capture + video encoder + audio)
 * ===================================================================== */
static int graph_init(void)
{
    DECLARE_ATTR(cap_attr, gm_cap_attr_t);
    DECLARE_ATTR(h264e_attr, gm_h264e_attr_t);
    DECLARE_ATTR(dnr_attr, gm_3dnr_attr_t);
    DECLARE_ATTR(audio_grab_attr, gm_audio_grab_attr_t);
    DECLARE_ATTR(audio_enc_attr, gm_audio_enc_attr_t);

    int width = CFG_WIDTH, height = CFG_HEIGHT;

    gm_init();
    gm_get_sysinfo(&gm_system);

    /* Neu sensor thuc te nho hon cau hinh yeu cau thi canh bao va dung
     * dung do phan giai cua sensor de tranh gm_apply() that bai. */
    if (width > gm_system.cap[0].dim.width || height > gm_system.cap[0].dim.height) {
        fprintf(stderr, "[WARN] Yeu cau %dx%d vuot qua sensor that (%dx%d), "
                "dung do phan giai sensor.\n",
                width, height, gm_system.cap[0].dim.width, gm_system.cap[0].dim.height);
        width  = gm_system.cap[0].dim.width;
        height = gm_system.cap[0].dim.height;
    }

    groupfd = gm_new_groupfd();

    /* ---- Capture + Video Encoder (H.264) ---- */
    capture_object = gm_new_obj(GM_CAP_OBJECT);
    venc_object    = gm_new_obj(GM_ENCODER_OBJECT);

    cap_attr.cap_vch = MD_CH;
    /* GM8136/GM8139 capture path: 0(liveview) 1/2(substream) 3(mainstream) */
    cap_attr.path = 3;
    cap_attr.enable_mv_data = 1;  /* BAT de Motion Detect co du lieu MV */
    gm_set_attr(capture_object, &cap_attr);

    if (width >= (gm_system.cap[0].dim.width / 2) &&
        height >= (gm_system.cap[0].dim.height / 2)) {
        dnr_attr.enabled = 1;
        gm_set_attr(capture_object, &dnr_attr);
    }

    h264e_attr.dim.width  = width;
    h264e_attr.dim.height = height;
    h264e_attr.frame_info.framerate = CFG_FPS;
    h264e_attr.ratectl.mode         = GM_CBR;
    h264e_attr.ratectl.gop          = CFG_GOP;
    h264e_attr.ratectl.bitrate      = CFG_BITRATE_KBPS;
    h264e_attr.ratectl.bitrate_max  = CFG_BITRATE_KBPS;
    h264e_attr.ratectl.init_quant   = 25;
    h264e_attr.ratectl.min_quant    = 20;
    h264e_attr.ratectl.max_quant    = 51;
    h264e_attr.b_frame_num          = 0;
    h264e_attr.enable_mv_data       = 0;   /* MV nhung trong H264 bitstream, khong can */
    gm_set_attr(venc_object, &h264e_attr);

    venc_bindfd = gm_bind(groupfd, capture_object, venc_object);

    /* ---- Audio Grab + Encoder (G.711 A-law) ---- */
    audio_grab_object = gm_new_obj(GM_AUDIO_GRAB_OBJECT);
    audio_enc_object   = gm_new_obj(GM_AUDIO_ENCODER_OBJECT);

    audio_grab_attr.vch          = 0;
    audio_grab_attr.sample_rate  = CFG_AUDIO_SAMPLERATE;
    audio_grab_attr.sample_size  = 16;
    audio_grab_attr.channel_type = GM_MONO;
    gm_set_attr(audio_grab_object, &audio_grab_attr);

    audio_enc_attr.encode_type   = GM_G711_ALAW;
    audio_enc_attr.bitrate        = 64000;  /* G711: co dinh, "don't care" theo doc SDK */
    audio_enc_attr.frame_samples  = CFG_AUDIO_FRAME_SAMPLES;
    gm_set_attr(audio_enc_object, &audio_enc_attr);

    audio_bindfd = gm_bind(groupfd, audio_grab_object, audio_enc_object);

    if (gm_apply(groupfd) < 0) {
        fprintf(stderr, "[FATAL] gm_apply() that bai!\n");
        return -1;
    }
    return 0;
}

static void graph_release(void)
{
    if (venc_bindfd)  gm_unbind(venc_bindfd);
    if (audio_bindfd) gm_unbind(audio_bindfd);
    gm_apply(groupfd);

    if (venc_object)        gm_delete_obj(venc_object);
    if (audio_enc_object)   gm_delete_obj(audio_enc_object);
    if (audio_grab_object)  gm_delete_obj(audio_grab_object);
    if (capture_object)     gm_delete_obj(capture_object);

    gm_delete_groupfd(groupfd);
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
    (void) argc; (void) argv;
    int ret;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("==== GM8136 2MP RTSP Daemon (SDK goc GrainMedia) ====\n");
    printf("Video : %dx%d @%dfps, CBR %dkbps, GOP=%d\n",
           CFG_WIDTH, CFG_HEIGHT, CFG_FPS, CFG_BITRATE_KBPS, CFG_GOP);
    printf("Audio : G.711A %dHz mono\n", CFG_AUDIO_SAMPLERATE);

    if (graph_init() < 0)
        return 1;

    /* OSD: line1 = text, line2 = timestamp (thread rieng cap nhat) */
    osd_init();

    /* Motion Detect: can bindfd cua venc da co truoc (motion_detection_update
     * gan mdt_alg voi venc_bindfd) */
    motion_detection_init();
    if (motion_setup() != 0)
        fprintf(stderr, "[WARN] Motion Detect khong khoi tao duoc, tinh nang se tat\n");

    /* Khoi dong video/audio encode thread TRUOC de co SDP video (can 1
     * I-frame) roi moi dang ky RTSP stream. */
    pthread_create(&th_video, NULL, video_thread, NULL);
    pthread_create(&th_audio, NULL, audio_thread, NULL);
    pthread_create(&th_motion, NULL, motion_thread, NULL);
    pthread_create(&th_osdclock, NULL, osd_clock_thread, NULL);

    /* Cho toi khi co SDP video (video_thread se set qua bien tam) -
     * don gian hoa: cho toi da 3s hoac den khi g_vsdp khac rong. */
    {
        int waited_ms = 0;
        while (g_vsdp[0] == '\0' && waited_ms < 5000) {
            usleep(50000);
            waited_ms += 50;
        }
    }

    if ((ret = stream_server_init(NULL, RTSP_PORT, 0, 1444, 256,
                                   4 /*max streams*/, 64 /*vq_max*/, 5 /*vq_len*/,
                                   64 /*aq_max*/, 4 /*aq_len*/,
                                   frm_cb, cmd_cb)) < 0) {
        fprintf(stderr, "[FATAL] stream_server_init loi %d\n", ret);
        goto cleanup;
    }
    if ((ret = stream_server_start()) < 0) {
        fprintf(stderr, "[FATAL] stream_server_start loi %d\n", ret);
        goto cleanup;
    }

    g_vqno = stream_queue_alloc(GM_SS_TYPE_H264);
    g_aqno = stream_queue_alloc(GM_SS_TYPE_G711A);

    /* Ghi chu: G.711A la static RTP payload type (PT=8, PCMA/8000) nen
     * KHONG can goi stream_sdp_parameter_encoder() cho audio - librtsp tu
     * dien SDP chuan cho cac type tinh (xem ghi chu trong librtsp.h, chi
     * "H264","MPEG4","MJPEG","AAC" moi can encode SDP dong). Neu qua trinh
     * test thuc te tren TinyCam/SmartRTSP khong thay track audio, hay dat
     * thu g_asdp[] = "a=rtpmap:8 PCMA/8000\r\n" truoc khi stream_reg(). */
    g_asdp[0] = '\0';

    g_sr = stream_reg(STREAM_NAME, g_vqno, g_vsdp, g_aqno, g_asdp,
                       1 /*live*/, 0, 0, 0, 0, NULL, NULL);
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
    pthread_join(th_audio, NULL);
    pthread_join(th_motion, NULL);
    pthread_join(th_osdclock, NULL);

    if (g_sr >= 0) stream_dereg(g_sr, 1);
    stream_server_stop();

    pthread_mutex_lock(&g_rec_lock);
    recorder_stop();
    pthread_mutex_unlock(&g_rec_lock);

    motion_detection_end();
    if (g_mdt_alg.sub_region) free(g_mdt_alg.sub_region);
    if (g_mdt_result.sub_region) free(g_mdt_result.sub_region);
    if (g_snapshot_buf) free(g_snapshot_buf);

    graph_release();
    return 0;
}
