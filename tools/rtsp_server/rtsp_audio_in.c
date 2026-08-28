/*
 * rtsp_audio_in.c - receive client audio over RTP and render to the camera
 * speaker (downlink / two-way audio).
 *
 * The RTSP network layer used by rtspd (librtsp.a) is a prebuilt static
 * library with no inbound audio callback, so the downlink receiver is a
 * standalone process. It binds the GM audio renderer exactly like
 * aac_play.c (GM_FILE_OBJECT -> GM_AUDIO_RENDER_OBJECT) and feeds decoded
 * audio frames to the renderer via gm_send_multi_bitstreams().
 *
 * The ADDA308 speaker path from the vendor firmware is known to accept
 * AAC-LC ADTS (16000 Hz mono). G.711 -> PCM rendering is included and
 * selected with -c, but needs on-camera validation (encode_type switched
 * between GM_AAC=2 and GM_PCM=1).
 *
 * Build:
 *   arm-unknown-linux-uclibcgnueabi-gcc -Os -Wall \
 *       rtsp_audio_in.c -L/path/to/gm_lib/lib -lgm -pthread -o rtsp_audio_in
 *
 * Usage:
 *   rtsp_audio_in [-p recvport] [-c {aac|pcm|ulaw|alaw}] [-r samplerate]
 *                 [-g g711channels] [-v]
 *
 * On the camera:
 *   rtsp_audio_in -p 5004 -c aac -r 16000
 *   then push RTP audio (RFC 3640 MPEG4-GENERIC AAC, or RFC 3551 G.711)
 *   to <camera>:5004.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define GM_ABI_VERSION          52
#define GM_FILE_OBJECT          0xfefe0004U
#define GM_AUDIO_RENDER_OBJECT  0xfefe0007U
#define GM_FILE_ATTR_SIZE       68U
#define GM_RENDER_ATTR_SIZE     64U
#define GM_STREAM_DESC_SIZE     128U
#define GM_SEND_TIMEOUT_MS      500

/* Offsets recovered from the vendor aac_player binary. */
#define FILE_SAMPLE_RATE_OFF    48U
#define FILE_SAMPLE_SIZE_OFF    50U
#define FILE_CHANNEL_TYPE_OFF   52U
#define RENDER_VALUE0_OFF       40U
#define RENDER_VALUE1_OFF       44U  /* audio encode type: 1=PCM 2=AAC ... */
#define RENDER_VALUE2_OFF       48U  /* render frame samples (e.g. 1024)   */
#define STREAM_BIND_OFF          0U
#define STREAM_DATA_OFF          4U
#define STREAM_LENGTH_OFF        8U

#define RTSP_RTP_MAX            1500
#define RTP_DEF_PORT            5004
#define ADTS_FRAME_MAX          12800
#define PCM_BLOCK_MAX           8192
#define RECV_BUF_SIZE           (RTSP_RTP_MAX * 4)

/* GM SDK ABI. These declarations match the ARM32 vendor binary imports. */
extern int   gm_init_private(int abi_version);
extern int   gm_init_attr(void *attr, const char *name,
                          int attr_size, int abi_version);
extern void *gm_new_groupfd(void);
extern int   gm_delete_groupfd(void *groupfd);
extern void *gm_new_obj(uint32_t object_type);
extern int   gm_delete_obj(void *object);
extern int   gm_set_attr(void *object, void *attr);
extern void *gm_bind(void *groupfd, void *source, void *destination);
extern int   gm_unbind(void *bindfd);
extern int   gm_apply(void *groupfd);
extern int   gm_send_multi_bitstreams(void *streams,
                                      int stream_count,
                                      int timeout_ms);
extern void  gm_release(void);

typedef enum {
    DL_CODEC_AAC,
    DL_CODEC_ULAW,
    DL_CODEC_ALAW,
    DL_CODEC_PCM
} dl_codec_t;

typedef struct {
    int          recv_port;
    dl_codec_t   codec;
    int          sample_rate;
    int          channels;
    int          verbose;
} dl_cfg_t;

static void put_u16(void *base, size_t offset, uint16_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void put_u32(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void put_ptr32(void *base, size_t offset, const void *value)
{
    put_u32(base, offset, (uint32_t)(uintptr_t)value);
}

static void dl_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void dl_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- ADTS helpers (shared logic with aac_play.c / rtspd.c) ---- */
static int adts_frame_length(const uint8_t h[7])
{
    int len;
    if (h[0] != 0xff || (h[1] & 0xf6) != 0xf0)
        return -1;
    len = ((h[3] & 0x03) << 11) | ((int)h[4] << 3) | ((h[5] >> 5) & 0x07);
    return len >= 7 ? len : -1;
}

/* Map a sample rate to its ADTS sampling-frequency index (ISO/IEC 13818-7). */
static int adts_sfi(int sample_rate)
{
    switch (sample_rate) {
    case 96000: return 0;  case 88200: return 1;  case 64000: return 2;
    case 48000: return 3;  case 44100: return 4;  case 32000: return 5;
    case 24000: return 6;  case 22050: return 7;  case 16000: return 8;
    case 12000: return 9;  case 11025: return 10; case 8000:  return 11;
    case 7350:  return 12;
    default:    return -1;
    }
}

/* Rebuild one ADTS frame from an RFC 3640 AU (AuHeader := 4-byte big-endian
 * AU-sizes, followed by the ADTS-stripped frame). We use the upstream
 * rtspd AUSize convention: header[0..1]=0x00,0x10, bits 16..31 carry the
 * size (see rtspd audio thread). */
static int aac_from_rfc3640(const dl_cfg_t *cfg, const uint8_t *raw,
                            int raw_len, uint8_t *out, int out_cap,
                            int *out_len)
{
    const uint8_t *au;
    int au_size, data_len;
    int hdr_len = 4;
    int sfi;
    int channel = (cfg->channels > 1) ? 2 : 1;
    int profile = 1;                       /* AAC-LC (object type 2 - 1) */
    int total;

    sfi = adts_sfi(cfg->sample_rate);
    if (sfi < 0)
        return -1;

    if (raw_len < hdr_len + 2)
        return -1;

    /* Decode the AU size. Attempt the rtspd layout first (size in bytes
     * 16..31 c.f. the 0x00 0x10 0xHH 0xLL AUSize header), else a plain
     * big-endian 16-bit size in the first two bytes. */
    if (raw[0] == 0 && raw[1] == 0x10) {
        au_size = ((int)raw[2] << 5) | ((int)raw[3] >> 3);
        hdr_len = 4;
    } else {
        au_size = ((int)raw[0] << 8) | raw[1];
        hdr_len = 2;
    }
    if (au_size < 0 || au_size > 2048)
        return -1;

    au = raw + hdr_len;
    data_len = raw_len - hdr_len;
    if (data_len < au_size || data_len > out_cap - 7)
        return -1;

    /* Craft a minimal 7-byte ADTS-LC header consistent with the renderer
     * sample rate / mono config. Hardcoding is correct here because the
     * fixed-rate ADDA308 render path must see the same rate we bound. */
    memset(out, 0, 7);
    out[0] = 0xff;
    out[1] = 0xf1;   /* MPEG-4, layer 0, protection_absent (no CRC) */
    out[2] = (uint8_t)(((profile & 0x03) << 6) | ((sfi & 0x0f) << 2) |
                       ((channel >> 2) & 0x01));
    out[3] = (uint8_t)((channel & 0x03) << 6);

    total = au_size + 7;                   /* full ADTS frame length */
    out[3] |= (uint8_t)((total >> 11) & 0x03);
    out[4]  = (uint8_t)((total >> 3) & 0xff);
    out[5]  = (uint8_t)((total & 0x07) << 5);

    /* copy the ADTS-stripped payload */
    memcpy(out + 7, au, au_size);
    *out_len = total;
    return 0;
}

static int aac_render_frame(const void *bindfd, const uint8_t *frame, int len,
                            void *descriptor)
{
    memset(descriptor, 0, GM_STREAM_DESC_SIZE);
    put_ptr32(descriptor, STREAM_BIND_OFF, bindfd);
    put_ptr32(descriptor, STREAM_DATA_OFF, frame);
    put_u32(descriptor, STREAM_LENGTH_OFF, (uint32_t)len);
    return gm_send_multi_bitstreams(descriptor, 1, GM_SEND_TIMEOUT_MS);
}

static void ulaw_to_pcm(int16_t *out, const uint8_t *in, int n)
{
    /* standard mu-law -> linear PCM */
    static const int16_t seg_end[8] = {
        0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF
    };
    int i;
    for (i = 0; i < n; i++) {
        uint8_t u = (uint8_t)in[i];
        int t = ((int)u ^ 0x55) + 0x84;
        int seg = (t >> 7) & 0x0F;
        int mant = (t >> 4) & 0x0F;
        int v = ((mant << 3) + seg_end[seg]) << (seg + 2);
        v += 0x84;
        out[i] = (u & 0x80) ? -v : v;
    }
}

static void alaw_to_pcm(int16_t *out, const uint8_t *in, int n)
{
    /* standard A-law -> linear PCM */
    int i;
    for (i = 0; i < n; i++) {
        uint8_t a = (uint8_t)in[i] ^ 0x55;
        int t  = ((int)a & 0x0F) << 4;
        int seg = ((int)a >> 4) & 0x07;
        int v;
        if (seg == 0)
            v = t + 8;
        else
            v = ((t + 0x108) << (seg - 1)) - 132;
        out[i] = (a & 0x80) ? -v : v;
    }
}

static int open_rtp_socket(int port)
{
    int fd;
    struct sockaddr_in addr;
    int one = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dl_err("[rtsp_audio_in] socket: %s\n", strerror(errno));
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dl_err("[rtsp_audio_in] bind %d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int init_render(dl_cfg_t *cfg, void **group_out, void **bind_out)
{
    uint8_t file_attr[GM_FILE_ATTR_SIZE];
    uint8_t render_attr[GM_RENDER_ATTR_SIZE];
    void *groupfd = NULL;
    void *file_object = NULL;
    void *render_object = NULL;
    void *bindfd = NULL;

    if (gm_init_private(GM_ABI_VERSION) < 0) {
        dl_err("[rtsp_audio_in] gm_init_private failed\n");
        return -1;
    }

    groupfd = gm_new_groupfd();
    file_object = gm_new_obj(GM_FILE_OBJECT);
    render_object = gm_new_obj(GM_AUDIO_RENDER_OBJECT);
    if (groupfd == NULL || file_object == NULL || render_object == NULL) {
        dl_err("[rtsp_audio_in] GM object allocation failed\n");
        goto fail;
    }

    memset(file_attr, 0, sizeof(file_attr));
    memset(render_attr, 0, sizeof(render_attr));
    if (gm_init_attr(file_attr, "gm_file_attr_t",
                     GM_FILE_ATTR_SIZE, GM_ABI_VERSION) < 0 ||
        gm_init_attr(render_attr, "gm_audio_render_attr_t",
                     GM_RENDER_ATTR_SIZE, GM_ABI_VERSION) < 0) {
        dl_err("[rtsp_audio_in] gm_init_attr failed\n");
        goto fail;
    }

    put_u16(file_attr, FILE_SAMPLE_RATE_OFF, (uint16_t)cfg->sample_rate);
    put_u16(file_attr, FILE_SAMPLE_SIZE_OFF, 16);
    put_u32(file_attr, FILE_CHANNEL_TYPE_OFF, cfg->channels);

    /* Exact values and offsets used by the vendor player. RENDER_VALUE1_OFF
     * is the audio encode type the renderer decodes: 2 = AAC (aac_play),
     * 1 = PCM (for G.711 decoded). RENDER_VALUE2_OFF is the render frame
     * size (samples). */
    put_u32(render_attr, RENDER_VALUE0_OFF, 0);
    put_u32(render_attr, RENDER_VALUE1_OFF,
            (cfg->codec == DL_CODEC_AAC) ? 2 : 1);
    put_u32(render_attr, RENDER_VALUE2_OFF, 1024);

    if (gm_set_attr(file_object, file_attr) < 0) {
        dl_err("[rtsp_audio_in] gm_set_attr(file) failed; sample_rate=%d\n",
               cfg->sample_rate);
        goto fail;
    }
    if (gm_set_attr(render_object, render_attr) < 0) {
        dl_err("[rtsp_audio_in] gm_set_attr(audio render) failed\n");
        goto fail;
    }

    bindfd = gm_bind(groupfd, file_object, render_object);
    if (bindfd == NULL) {
        dl_err("[rtsp_audio_in] gm_bind failed\n");
        goto fail;
    }
    if (gm_apply(groupfd) < 0) {
        dl_err("[rtsp_audio_in] gm_apply failed\n");
        goto fail;
    }

    *group_out = groupfd;
    *bind_out = bindfd;
    return 0;

fail:
    if (groupfd != NULL)
        gm_delete_groupfd(groupfd);
    if (file_object != NULL)
        gm_delete_obj(file_object);
    if (render_object != NULL)
        gm_delete_obj(render_object);
    return -1;
}

static int send_g711(const void *bindfd, dl_cfg_t *cfg, const uint8_t *pyld,
                     int plen, void *descriptor)
{
    static int16_t pcm[PCM_BLOCK_MAX];
    int block = (plen > (int)(sizeof(pcm) / sizeof(pcm[0])))
                ? (int)(sizeof(pcm) / sizeof(pcm[0])) : plen;

    if (block <= 0)
        return -1;

    if (cfg->codec == DL_CODEC_ULAW)
        ulaw_to_pcm(pcm, (const uint8_t *)pyld, block);
    else
        alaw_to_pcm(pcm, (const uint8_t *)pyld, block);

    memset(descriptor, 0, GM_STREAM_DESC_SIZE);
    put_ptr32(descriptor, STREAM_BIND_OFF, bindfd);
    put_ptr32(descriptor, STREAM_DATA_OFF, pcm);
    put_u32(descriptor, STREAM_LENGTH_OFF, (uint32_t)(block * 2));
    return gm_send_multi_bitstreams(descriptor, 1, GM_SEND_TIMEOUT_MS);
}

static int feed_packet(const void *bindfd, dl_cfg_t *cfg,
                       const uint8_t *pkt, int pkt_len, void *descriptor)
{
    uint8_t adts_buf[ADTS_FRAME_MAX];
    const uint8_t *pyld;
    int plen;
    int skip;

    /* RTP header is at least 12 bytes. Extract the payload. */
    if (pkt_len < 12)
        return -1;
    skip = 12 + ((pkt[0] & 0x10) ? 4 : 0);   /* + CSRC list, ignored here */
    if (skip > pkt_len)
        return -1;
    pyld = pkt + skip;
    plen = pkt_len - skip;

    if (cfg->codec == DL_CODEC_AAC) {
        int frame_len;
        if (aac_from_rfc3640(cfg, pyld, plen, adts_buf,
                             ADTS_FRAME_MAX, &frame_len) == 0) {
            return aac_render_frame(bindfd, adts_buf, frame_len, descriptor);
        }
        /* fall through: if the payload is already a full ADTS frame, send it
         * directly (some senders stream raw ADTS). */
        if (plen >= 7 && adts_frame_length(pyld) > 0) {
            return aac_render_frame(bindfd, pyld, plen, descriptor);
        }
        return -1;
    }

    /* G.711 μ/A-law or raw PCM */
    if (cfg->codec == DL_CODEC_ULAW || cfg->codec == DL_CODEC_ALAW)
        return send_g711(bindfd, cfg, pyld, plen, descriptor);

    /* raw PCM over RTP */
    memset(descriptor, 0, GM_STREAM_DESC_SIZE);
    put_ptr32(descriptor, STREAM_BIND_OFF, bindfd);
    put_ptr32(descriptor, STREAM_DATA_OFF, pyld);
    put_u32(descriptor, STREAM_LENGTH_OFF, (uint32_t)plen);
    return gm_send_multi_bitstreams(descriptor, 1, GM_SEND_TIMEOUT_MS);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-p port] [-c {aac|ulaw|alaw|pcm}] [-r samplerate]\n"
        "          [-g channels] [-v]\n"
        "\n"
        "Listen for client RTP audio and render it to the camera speaker.\n"
        "  -p  UDP/RTP receive port (default %d)\n"
        "  -c  payload codec: aac (RFC 3640 MPEG4-GENERIC, default),\n"
        "      ulaw (RFC 3551 G.711 u-law), alaw (A-law), pcm (raw 16-bit)\n"
        "  -r  sample rate Hz (default 16000)\n"
        "  -g  channels (default 1)\n"
        "  -v  verbose per-packet logging\n"
        "\n"
        "The ADDA308 speaker path accepts AAC-LC ADTS 16 kHz mono by\n"
        "default; the G.711/PCM modes need on-camera validation.\n",
        prog, RTP_DEF_PORT);
}

int main(int argc, char **argv)
{
    dl_cfg_t cfg;
    void *groupfd = NULL;
    void *bindfd = NULL;
    void *descriptor = NULL;
    int fd = -1;
    int opt;
    uint8_t *recv_buf = NULL;
    unsigned long st_pkts = 0, st_bytes = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.recv_port = RTP_DEF_PORT;
    cfg.codec = DL_CODEC_AAC;
    cfg.sample_rate = 16000;
    cfg.channels = 1;

    while ((opt = getopt(argc, argv, "p:c:r:g:vh")) != -1) {
        switch (opt) {
        case 'p':
            cfg.recv_port = atoi(optarg);
            break;
        case 'c':
            if (!strcmp(optarg, "aac"))            cfg.codec = DL_CODEC_AAC;
            else if (!strcmp(optarg, "ulaw"))      cfg.codec = DL_CODEC_ULAW;
            else if (!strcmp(optarg, "alaw"))      cfg.codec = DL_CODEC_ALAW;
            else if (!strcmp(optarg, "pcm"))       cfg.codec = DL_CODEC_PCM;
            else { usage(argv[0]); return 1; }
            break;
        case 'r':
            cfg.sample_rate = atoi(optarg);
            break;
        case 'g':
            cfg.channels = atoi(optarg);
            break;
        case 'v':
            cfg.verbose = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (cfg.sample_rate <= 0 || cfg.channels <= 0 || cfg.recv_port <= 0) {
        usage(argv[0]);
        return 1;
    }

    if (init_render(&cfg, &groupfd, &bindfd) < 0)
        return 1;

    fd = open_rtp_socket(cfg.recv_port);
    if (fd < 0)
        goto out;

    descriptor = malloc(GM_STREAM_DESC_SIZE);
    recv_buf = malloc(RECV_BUF_SIZE);
    if (descriptor == NULL || recv_buf == NULL) {
        dl_err("[rtsp_audio_in] alloc failed\n");
        goto out;
    }

    dl_log("[rtsp_audio_in] listening on udp/%d codec=%d rate=%d ch=%d\n",
           cfg.recv_port, (int)cfg.codec, cfg.sample_rate, cfg.channels);

    for (;;) {
        ssize_t n = recv(fd, recv_buf, RECV_BUF_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            dl_err("[rtsp_audio_in] recv: %s\n", strerror(errno));
            break;
        }
        st_pkts++;
        st_bytes += (unsigned long)n;
        if (feed_packet(bindfd, &cfg, recv_buf, (int)n, descriptor) < 0) {
            if (cfg.verbose)
                dl_log("[rtsp_audio_in] dropped packet len=%d\n", (int)n);
        }
        if (cfg.verbose && (st_pkts % 100 == 0))
            dl_log("[rtsp_audio_in] pkts=%lu bytes=%lu\n", st_pkts, st_bytes);
    }

    dl_log("[rtsp_audio_in] exiting pkts=%lu bytes=%lu\n", st_pkts, st_bytes);

out:
    if (fd >= 0)
        close(fd);
    if (descriptor != NULL)
        free(descriptor);
    if (recv_buf != NULL)
        free(recv_buf);
    if (bindfd != NULL)
        gm_unbind(bindfd);
    if (groupfd != NULL)
        gm_delete_groupfd(groupfd);
    gm_release();
    return 0;
}
