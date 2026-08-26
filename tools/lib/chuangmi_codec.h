#ifndef chuangmi_codec_h
#define chuangmi_codec_h

#include <gmlib.h>

#define CODEC_ENC_H264   0
#define CODEC_ENC_MPEG4  1
#define CODEC_ENC_MJPEG  2

typedef void (*codec_video_frame_cb)(const char *data, unsigned int len,
                                     int keyframe, unsigned int timestamp,
                                     unsigned int newbs_flag, void *user);

typedef void (*codec_audio_frame_cb)(const char *data, unsigned int len,
                                     void *user);

typedef struct {
    int type;            /* CODEC_ENC_H264 / CODEC_ENC_MPEG4 / CODEC_ENC_MJPEG */
    int width;
    int height;
    int framerate;
    int gop;             /* 0 = framerate (1 second) */
    int bitrate_kbps;    /* ignored for MJPEG */
    int rc_mode;         /* GM_CBR / GM_VBR / GM_ECBR / GM_EVBR, 0 = GM_CBR */
    int profile;         /* gm_h264e_profile_t, -1 = driver default */
    int level;           /* gm_h264e_level_t, -1 = driver default */
    int coding;          /* gm_h264e_coding_t, -1 = driver default */
    int gray_scale;      /* 1 = monochrome encoding */
    codec_video_frame_cb frame_cb;
    void *user;
} codec_video_config_t;

typedef struct {
    int encode_type;     /* GM_PCM / GM_AAC / GM_ADPCM / GM_G711_ALAW / GM_G711_ULAW */
    int sample_rate;     /* 8000, 16000, ... */
    int sample_size;     /* bits per sample, usually 16 */
    int channel_type;    /* GM_MONO / GM_STEREO */
    int bitrate;         /* ADPCM: 16000/32000, AAC: 14500~192000, else 0 */
    int frame_samples;   /* see gmlib.h suggestions per codec, 0 = sane default */
    codec_audio_frame_cb frame_cb;
    void *user;
} codec_audio_enc_config_t;

int codec_init(void);
int codec_end(void);
int codec_is_initialized(void);
unsigned int codec_chipid(void);
const gm_cap_sys_info_t *codec_cap_info(int vch);

int codec_video_start(const codec_video_config_t *cfg);
int codec_video_stop(void);
int codec_video_is_running(void);
int codec_video_set_bitrate(int bitrate_kbps);
int codec_video_set_framerate(int framerate);
int codec_video_set_gop(int gop);
int codec_video_request_keyframe(void);
int codec_snapshot_jpeg(char *buf, unsigned int buf_len, int quality,
                        int width, int height, int timeout_ms);

int codec_audio_enc_start(const codec_audio_enc_config_t *cfg);
int codec_audio_enc_stop(void);
int codec_audio_enc_is_running(void);

int codec_audio_play_start(int sample_rate);
int codec_audio_play_frame(const char *data, unsigned int len);
int codec_audio_play_stop(void);

#endif
