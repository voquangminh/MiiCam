/*
 * libchuangmi_codec - gmlib video/audio codec wrapper for the Chuangmi camera
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chuangmi_codec.h"

#define VIDEO_BS_BUF_LEN   (1280 * 720 * 3 / 2)
#define AUDIO_BS_BUF_LEN   (256 * 1024)
#define POLL_TIMEOUT_MS    500

/* gm_file_attr_t / gm_audio_render_attr_t layout of the on-camera vendor
 * libgm.so differs from the 2015 SDK header; these offsets are the ones
 * proven by the vendor aac_player reconstruction (see tools/rtsp_server/aac_play.c). */
#define GM_FILE_ATTR_SIZE       68
#define GM_RENDER_ATTR_SIZE     64
#define GM_STREAM_DESC_SIZE     128
#define FILE_SAMPLE_RATE_OFF    48
#define FILE_SAMPLE_SIZE_OFF    50
#define FILE_CHANNEL_TYPE_OFF   52
#define RENDER_VALUE0_OFF       40
#define RENDER_VALUE1_OFF       44
#define RENDER_VALUE2_OFF       48
#define STREAM_BIND_OFF         0
#define STREAM_DATA_OFF         4
#define STREAM_LENGTH_OFF       8

static int codec_initialized;
static int gm_owned;

static struct {
    int running;
    pthread_t thread;
    void *groupfd;
    void *cap_obj;
    void *enc_obj;
    void *bindfd;
    int type;
    gm_h264e_attr_t h264e_attr;
    codec_video_frame_cb cb;
    void *user;
} video;

static struct {
    int running;
    pthread_t thread;
    void *groupfd;
    void *grab_obj;
    void *enc_obj;
    void *bindfd;
    codec_audio_frame_cb cb;
    void *user;
} audio_enc;

static struct {
    int running;
    void *groupfd;
    void *file_obj;
    void *render_obj;
    void *bindfd;
} audio_play;

static unsigned int codec_read_chipid(void)
{
    FILE *fp;
    char buffer[256];
    int i;
    unsigned int chipid = 0;
    char *match;

    fp = fopen("/proc/pmu/chipver", "r");
    if (!fp)
        return 0;

    i = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    if (i <= 0)
        return 0;
    buffer[i] = '\0';

    match = strstr(buffer, "81");
    if (match == NULL)
        return 0;
    sscanf(match, "%X", &chipid);
    return chipid;
}

unsigned int codec_chipid(void)
{
    return codec_read_chipid();
}

const gm_cap_sys_info_t *codec_cap_info(int vch)
{
    static gm_system_t sysinfo;
    static int loaded;

    if (!loaded) {
        if (gm_get_sysinfo(&sysinfo) < 0)
            return NULL;
        loaded = 1;
    }

    if (vch < 0 || vch >= CAPTURE_VCH_NUMBER || !sysinfo.cap[vch].valid)
        return NULL;

    return &sysinfo.cap[vch];
}

int codec_is_initialized(void)
{
    return codec_initialized ? 0 : -1;
}

int codec_init(void)
{
    if (codec_initialized)
        return 0;

    if (gm_init() < 0) {
        fprintf(stderr, "*** Error: gm_init failed\n");
        return -1;
    }
    gm_owned = 1;
    codec_initialized = 1;
    return 0;
}

int codec_end(void)
{
    if (!codec_initialized)
        return -1;

    codec_video_stop();
    codec_audio_enc_stop();
    codec_audio_play_stop();

    if (gm_owned) {
        gm_release();
        gm_owned = 0;
    }
    codec_initialized = 0;
    return 0;
}

/* *********************************************************************** */
/* Video encoder                                                           */
/* *********************************************************************** */

static void *video_encode_thread(void *arg)
{
    char *bs_buf;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];

    (void)arg;

    bs_buf = (char *)malloc(VIDEO_BS_BUF_LEN);
    if (!bs_buf)
        return NULL;

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = video.bindfd;
    poll_fds[0].event = GM_POLL_READ;

    while (video.running) {
        int ret;

        ret = gm_poll(poll_fds, 1, POLL_TIMEOUT_MS);
        if (ret == GM_TIMEOUT || !video.running)
            continue;
        if (ret < 0)
            break;

        if (!(poll_fds[0].revent.event & GM_POLL_READ))
            continue;
        if (poll_fds[0].revent.bs_len > VIDEO_BS_BUF_LEN)
            continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        multi_bs[0].bindfd = poll_fds[0].bindfd;
        multi_bs[0].bs.bs_buf = bs_buf;
        multi_bs[0].bs.bs_buf_len = VIDEO_BS_BUF_LEN;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        ret = gm_recv_multi_bitstreams(multi_bs, 1);
        if (ret < 0)
            continue;

        if (multi_bs[0].retval == GM_SUCCESS && video.cb && multi_bs[0].bs.bs_len > 0) {
            video.cb(multi_bs[0].bs.bs_buf,
                     multi_bs[0].bs.bs_len,
                     multi_bs[0].bs.keyframe,
                     multi_bs[0].bs.timestamp,
                     multi_bs[0].bs.newbs_flag,
                     video.user);
        }
    }

    free(bs_buf);
    return NULL;
}

static int codec_video_apply_h264_attrs(void *obj, const codec_video_config_t *cfg)
{
    DECLARE_ATTR(h264e_attr, gm_h264e_attr_t);
    DECLARE_ATTR(h264_adv, gm_h264_advanced_attr_t);
    DECLARE_ATTR(vui_attr, gm_h264_vui_attr_t);

    h264e_attr.dim.width = cfg->width;
    h264e_attr.dim.height = cfg->height;
    h264e_attr.frame_info.framerate = cfg->framerate;
    h264e_attr.ratectl.mode = cfg->rc_mode ? (gm_enc_ratecontrol_mode_t)cfg->rc_mode : GM_CBR;
    h264e_attr.ratectl.gop = cfg->gop > 0 ? cfg->gop : cfg->framerate;
    h264e_attr.ratectl.bitrate = cfg->bitrate_kbps;
    h264e_attr.ratectl.bitrate_max = cfg->bitrate_kbps;
    h264e_attr.ratectl.init_quant = 25;
    h264e_attr.ratectl.min_quant = 20;
    h264e_attr.ratectl.max_quant = 51;
    h264e_attr.b_frame_num = 0;
    h264e_attr.enable_mv_data = 0;
    if (cfg->profile >= 0)
        h264e_attr.profile_setting.profile = (gm_h264e_profile_t)cfg->profile;
    if (cfg->level >= 0)
        h264e_attr.profile_setting.level = (gm_h264e_level_t)cfg->level;
    if (cfg->coding >= 0)
        h264e_attr.profile_setting.coding = (gm_h264e_coding_t)cfg->coding;
    if (gm_set_attr(obj, &h264e_attr) < 0)
        return -1;

    h264_adv.multi_slice = 4;
    h264_adv.field_coding = 0;
    h264_adv.gray_scale = cfg->gray_scale ? 1 : 0;
    if (gm_set_attr(obj, &h264_adv) < 0)
        return -1;

    vui_attr.param_info.param.video_format = 5;
    vui_attr.param_info.param.colour_primaries = 1;
    vui_attr.param_info.param.transfer_characteristics = 1;
    vui_attr.param_info.param.matrix_coefficient = 1;
    vui_attr.param_info.param.full_range = 1;
    vui_attr.param_info.param.timing_info_present_flag = 0;
    vui_attr.sar_info.sar.sar_width = 1;
    vui_attr.sar_info.sar.sar_height = 1;
    if (gm_set_attr(obj, &vui_attr) < 0)
        return -1;

    memcpy(&video.h264e_attr, &h264e_attr, sizeof(gm_h264e_attr_t));
    return 0;
}

int codec_video_start(const codec_video_config_t *cfg)
{
    DECLARE_ATTR(cap_attr, gm_cap_attr_t);

    if (!codec_initialized) {
        fprintf(stderr, "*** Error: codec library is uninitialized\n");
        return -1;
    }
    if (video.running) {
        fprintf(stderr, "*** Error: video encoder already running\n");
        return -1;
    }
    if (!cfg || cfg->width <= 0 || cfg->height <= 0 ||
        cfg->framerate <= 0 || !cfg->frame_cb) {
        fprintf(stderr, "*** Error: invalid video config\n");
        return -1;
    }

    memset(&video, 0, sizeof(video));
    video.type = cfg->type;
    video.cb = cfg->frame_cb;
    video.user = cfg->user;

    video.groupfd = gm_new_groupfd();
    video.cap_obj = gm_new_obj(GM_CAP_OBJECT);
    video.enc_obj = gm_new_obj(GM_ENCODER_OBJECT);
    if (!video.groupfd || !video.cap_obj || !video.enc_obj)
        goto fail;

    cap_attr.cap_vch = 0;
    cap_attr.path = 0;
    cap_attr.enable_mv_data = 0;
    if (gm_set_attr(video.cap_obj, &cap_attr) < 0)
        goto fail;

    switch (cfg->type) {
    case CODEC_ENC_H264:
        if (codec_video_apply_h264_attrs(video.enc_obj, cfg) < 0)
            goto fail;
        break;
    case CODEC_ENC_MPEG4: {
        DECLARE_ATTR(mpeg4e_attr, gm_mpeg4e_attr_t);
        mpeg4e_attr.dim.width = cfg->width;
        mpeg4e_attr.dim.height = cfg->height;
        mpeg4e_attr.frame_info.framerate = cfg->framerate;
        mpeg4e_attr.ratectl.mode = cfg->rc_mode ? (gm_enc_ratecontrol_mode_t)cfg->rc_mode : GM_CBR;
        mpeg4e_attr.ratectl.gop = cfg->gop > 0 ? cfg->gop : cfg->framerate;
        mpeg4e_attr.ratectl.bitrate = cfg->bitrate_kbps;
        mpeg4e_attr.ratectl.bitrate_max = cfg->bitrate_kbps;
        if (gm_set_attr(video.enc_obj, &mpeg4e_attr) < 0)
            goto fail;
        break;
    }
    case CODEC_ENC_MJPEG: {
        DECLARE_ATTR(mjpege_attr, gm_mjpege_attr_t);
        mjpege_attr.dim.width = cfg->width;
        mjpege_attr.dim.height = cfg->height;
        mjpege_attr.frame_info.framerate = cfg->framerate;
        mjpege_attr.quality = 70;
        if (gm_set_attr(video.enc_obj, &mjpege_attr) < 0)
            goto fail;
        break;
    }
    default:
        fprintf(stderr, "*** Error: unsupported encoder type %d\n", cfg->type);
        goto fail;
    }

    video.bindfd = gm_bind(video.groupfd, video.cap_obj, video.enc_obj);
    if (!video.bindfd)
        goto fail;

    if (gm_apply(video.groupfd) < 0) {
        fprintf(stderr, "*** Error: gm_apply failed for video pipeline\n");
        goto fail;
    }

    video.running = 1;
    if (pthread_create(&video.thread, NULL, video_encode_thread, NULL) < 0) {
        video.running = 0;
        goto fail;
    }

    return 0;

fail:
    if (video.bindfd)
        gm_unbind(video.bindfd);
    if (video.enc_obj)
        gm_delete_obj(video.enc_obj);
    if (video.cap_obj)
        gm_delete_obj(video.cap_obj);
    if (video.groupfd)
        gm_delete_groupfd(video.groupfd);
    memset(&video, 0, sizeof(video));
    return -1;
}

int codec_video_stop(void)
{
    if (!video.running)
        return -1;

    video.running = 0;
    pthread_join(video.thread, NULL);

    gm_unbind(video.bindfd);
    gm_apply(video.groupfd);
    gm_delete_obj(video.enc_obj);
    gm_delete_obj(video.cap_obj);
    gm_delete_groupfd(video.groupfd);
    memset(&video, 0, sizeof(video));
    return 0;
}

int codec_video_is_running(void)
{
    return video.running ? 0 : -1;
}

static int codec_video_update_ratecontrol(int bitrate_kbps, int framerate, int gop)
{
    if (!video.running || video.type != CODEC_ENC_H264)
        return -1;

    if (bitrate_kbps > 0) {
        video.h264e_attr.ratectl.bitrate = bitrate_kbps;
        video.h264e_attr.ratectl.bitrate_max = bitrate_kbps;
    }
    if (framerate > 0)
        video.h264e_attr.frame_info.framerate = framerate;
    if (gop > 0)
        video.h264e_attr.ratectl.gop = gop;

    if (gm_set_attr(video.enc_obj, &video.h264e_attr) < 0)
        return -1;
    if (gm_apply(video.groupfd) < 0)
        return -1;

    return 0;
}

int codec_video_set_bitrate(int bitrate_kbps)
{
    return codec_video_update_ratecontrol(bitrate_kbps, 0, 0);
}

int codec_video_set_framerate(int framerate)
{
    return codec_video_update_ratecontrol(0, framerate, 0);
}

int codec_video_set_gop(int gop)
{
    return codec_video_update_ratecontrol(0, 0, gop);
}

int codec_video_request_keyframe(void)
{
    if (!video.running)
        return -1;
    return gm_request_keyframe(video.bindfd);
}

int codec_snapshot_jpeg(char *buf, unsigned int buf_len, int quality,
                        int width, int height, int timeout_ms)
{
    snapshot_t snapshot;
    int len;

    if (!codec_initialized || !buf || buf_len == 0)
        return -1;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.bindfd = video.running ? video.bindfd : NULL;
    snapshot.image_quality = quality;
    snapshot.bs_buf = buf;
    snapshot.bs_buf_len = buf_len;
    snapshot.bs_width = width;
    snapshot.bs_height = height;

    len = gm_request_snapshot(&snapshot, timeout_ms > 0 ? timeout_ms : 1000);
    return len;
}

/* *********************************************************************** */
/* Audio encoder                                                           */
/* *********************************************************************** */

static void *audio_encode_thread(void *arg)
{
    char *bs_buf;
    gm_pollfd_t poll_fds[1];
    gm_enc_multi_bitstream_t multi_bs[1];

    (void)arg;

    bs_buf = (char *)malloc(AUDIO_BS_BUF_LEN);
    if (!bs_buf)
        return NULL;

    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].bindfd = audio_enc.bindfd;
    poll_fds[0].event = GM_POLL_READ;

    while (audio_enc.running) {
        int ret;

        ret = gm_poll(poll_fds, 1, POLL_TIMEOUT_MS);
        if (ret == GM_TIMEOUT || !audio_enc.running)
            continue;
        if (ret < 0)
            break;

        if (!(poll_fds[0].revent.event & GM_POLL_READ))
            continue;
        if (poll_fds[0].revent.bs_len > AUDIO_BS_BUF_LEN)
            continue;

        memset(multi_bs, 0, sizeof(multi_bs));
        multi_bs[0].bindfd = poll_fds[0].bindfd;
        multi_bs[0].bs.bs_buf = bs_buf;
        multi_bs[0].bs.bs_buf_len = AUDIO_BS_BUF_LEN;
        multi_bs[0].bs.mv_buf = 0;
        multi_bs[0].bs.mv_buf_len = 0;

        ret = gm_recv_multi_bitstreams(multi_bs, 1);
        if (ret < 0)
            continue;

        if (multi_bs[0].retval == GM_SUCCESS && audio_enc.cb && multi_bs[0].bs.bs_len > 0) {
            audio_enc.cb(multi_bs[0].bs.bs_buf, multi_bs[0].bs.bs_len, audio_enc.user);
        }
    }

    free(bs_buf);
    return NULL;
}

int codec_audio_enc_start(const codec_audio_enc_config_t *cfg)
{
    DECLARE_ATTR(audio_grab_attr, gm_audio_grab_attr_t);
    DECLARE_ATTR(audio_encode_attr, gm_audio_enc_attr_t);

    if (!codec_initialized) {
        fprintf(stderr, "*** Error: codec library is uninitialized\n");
        return -1;
    }
    if (audio_enc.running) {
        fprintf(stderr, "*** Error: audio encoder already running\n");
        return -1;
    }
    if (!cfg || !cfg->frame_cb || cfg->sample_rate <= 0) {
        fprintf(stderr, "*** Error: invalid audio encode config\n");
        return -1;
    }

    memset(&audio_enc, 0, sizeof(audio_enc));
    audio_enc.cb = cfg->frame_cb;
    audio_enc.user = cfg->user;

    audio_enc.groupfd = gm_new_groupfd();
    audio_enc.grab_obj = gm_new_obj(GM_AUDIO_GRAB_OBJECT);
    audio_enc.enc_obj = gm_new_obj(GM_AUDIO_ENCODER_OBJECT);
    if (!audio_enc.groupfd || !audio_enc.grab_obj || !audio_enc.enc_obj)
        goto fail;

    audio_grab_attr.vch = 0;
    audio_grab_attr.sample_rate = cfg->sample_rate;
    audio_grab_attr.sample_size = cfg->sample_size > 0 ? cfg->sample_size : 16;
    audio_grab_attr.channel_type = cfg->channel_type ? (gm_audio_channel_type_t)cfg->channel_type : GM_MONO;
    if (gm_set_attr(audio_enc.grab_obj, &audio_grab_attr) < 0)
        goto fail;

    audio_encode_attr.encode_type = (gm_audio_encode_type_t)cfg->encode_type;
    audio_encode_attr.bitrate = cfg->bitrate;
    audio_encode_attr.block_count = 2;
    audio_encode_attr.frame_samples = cfg->frame_samples;
    if (audio_encode_attr.frame_samples <= 0) {
        switch (cfg->encode_type) {
        case GM_PCM:   audio_encode_attr.frame_samples = 1024; break;
        case GM_AAC:   audio_encode_attr.frame_samples = 1024; break;
        case GM_ADPCM: audio_encode_attr.frame_samples = 505;  break;
        default:       audio_encode_attr.frame_samples = 320;  break;
        }
    }
    if (gm_set_attr(audio_enc.enc_obj, &audio_encode_attr) < 0)
        goto fail;

    audio_enc.bindfd = gm_bind(audio_enc.groupfd, audio_enc.grab_obj, audio_enc.enc_obj);
    if (!audio_enc.bindfd)
        goto fail;

    if (gm_apply(audio_enc.groupfd) < 0) {
        fprintf(stderr, "*** Error: gm_apply failed for audio pipeline\n");
        goto fail;
    }

    audio_enc.running = 1;
    if (pthread_create(&audio_enc.thread, NULL, audio_encode_thread, NULL) < 0) {
        audio_enc.running = 0;
        goto fail;
    }

    return 0;

fail:
    if (audio_enc.bindfd)
        gm_unbind(audio_enc.bindfd);
    if (audio_enc.enc_obj)
        gm_delete_obj(audio_enc.enc_obj);
    if (audio_enc.grab_obj)
        gm_delete_obj(audio_enc.grab_obj);
    if (audio_enc.groupfd)
        gm_delete_groupfd(audio_enc.groupfd);
    memset(&audio_enc, 0, sizeof(audio_enc));
    return -1;
}

int codec_audio_enc_stop(void)
{
    if (!audio_enc.running)
        return -1;

    audio_enc.running = 0;
    pthread_join(audio_enc.thread, NULL);

    gm_unbind(audio_enc.bindfd);
    gm_apply(audio_enc.groupfd);
    gm_delete_obj(audio_enc.enc_obj);
    gm_delete_obj(audio_enc.grab_obj);
    gm_delete_groupfd(audio_enc.groupfd);
    memset(&audio_enc, 0, sizeof(audio_enc));
    return 0;
}

int codec_audio_enc_is_running(void)
{
    return audio_enc.running ? 0 : -1;
}

/* *********************************************************************** */
/* Audio playback (speaker)                                                */
/* *********************************************************************** */

static void play_put_u16(void *base, int offset, unsigned short value)
{
    memcpy((char *)base + offset, &value, sizeof(value));
}

static void play_put_u32(void *base, int offset, unsigned int value)
{
    memcpy((char *)base + offset, &value, sizeof(value));
}

int codec_audio_play_start(int sample_rate)
{
    unsigned char file_attr[GM_FILE_ATTR_SIZE];
    unsigned char render_attr[GM_RENDER_ATTR_SIZE];

    if (!codec_initialized) {
        if (codec_init() < 0)
            return -1;
    }
    if (audio_play.running) {
        fprintf(stderr, "*** Error: audio playback already running\n");
        return -1;
    }

    memset(&audio_play, 0, sizeof(audio_play));
    audio_play.groupfd = gm_new_groupfd();
    audio_play.file_obj = gm_new_obj(GM_FILE_OBJECT);
    audio_play.render_obj = gm_new_obj(GM_AUDIO_RENDER_OBJECT);
    if (!audio_play.groupfd || !audio_play.file_obj || !audio_play.render_obj)
        goto fail;

    memset(file_attr, 0, sizeof(file_attr));
    memset(render_attr, 0, sizeof(render_attr));

    /* Header declares gm_init_attr as void; failures surface at gm_set_attr */
    gm_init_attr(file_attr, "gm_file_attr_t", GM_FILE_ATTR_SIZE, GM_VERSION_CODE);
    gm_init_attr(render_attr, "gm_audio_render_attr_t", GM_RENDER_ATTR_SIZE, GM_VERSION_CODE);

    play_put_u16(file_attr, FILE_SAMPLE_RATE_OFF, (unsigned short)sample_rate);
    play_put_u16(file_attr, FILE_SAMPLE_SIZE_OFF, 16);
    play_put_u32(file_attr, FILE_CHANNEL_TYPE_OFF, 1);

    /* Exact values used by the vendor player: AAC-LC, block 1024, no LCD sync */
    play_put_u32(render_attr, RENDER_VALUE0_OFF, 0);
    play_put_u32(render_attr, RENDER_VALUE1_OFF, GM_AAC);
    play_put_u32(render_attr, RENDER_VALUE2_OFF, 1024);

    if (gm_set_attr(audio_play.file_obj, file_attr) < 0 ||
        gm_set_attr(audio_play.render_obj, render_attr) < 0)
        goto fail;

    audio_play.bindfd = gm_bind(audio_play.groupfd, audio_play.file_obj, audio_play.render_obj);
    if (!audio_play.bindfd)
        goto fail;

    if (gm_apply(audio_play.groupfd) < 0)
        goto fail;

    audio_play.running = 1;
    return 0;

fail:
    if (audio_play.bindfd)
        gm_unbind(audio_play.bindfd);
    if (audio_play.file_obj)
        gm_delete_obj(audio_play.file_obj);
    if (audio_play.render_obj)
        gm_delete_obj(audio_play.render_obj);
    if (audio_play.groupfd)
        gm_delete_groupfd(audio_play.groupfd);
    memset(&audio_play, 0, sizeof(audio_play));
    return -1;
}

int codec_audio_play_frame(const char *data, unsigned int len)
{
    unsigned char descriptor[GM_STREAM_DESC_SIZE];
    unsigned int addr;

    if (!audio_play.running || !data || len == 0)
        return -1;

    memset(descriptor, 0, sizeof(descriptor));
    addr = (unsigned int)(unsigned long)audio_play.bindfd;
    play_put_u32(descriptor, STREAM_BIND_OFF, addr);
    addr = (unsigned int)(unsigned long)data;
    play_put_u32(descriptor, STREAM_DATA_OFF, addr);
    play_put_u32(descriptor, STREAM_LENGTH_OFF, len);

    return gm_send_multi_bitstreams((gm_dec_multi_bitstream_t *)descriptor, 1, 500);
}

int codec_audio_play_stop(void)
{
    if (!audio_play.running)
        return -1;

    gm_unbind(audio_play.bindfd);
    gm_apply(audio_play.groupfd);
    gm_delete_obj(audio_play.file_obj);
    gm_delete_obj(audio_play.render_obj);
    gm_delete_groupfd(audio_play.groupfd);
    memset(&audio_play, 0, sizeof(audio_play));
    sleep(1);
    return 0;
}
