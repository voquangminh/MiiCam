/* live_http.c - embedded live HTTP video streaming for rtspd
 *
 * The camera's lighttpd mod_cgi buffers the entire CGI stdout until the
 * process exits, so multipart/x-mixed-replace and progressive video can
 * never work through arm-php-cgi. This module is a tiny standalone HTTP
 * server (rtspd `-D <port>`) that re-serves the H.264 bitstream coming
 * out of the gmlib encoder while rtspd runs:
 *
 *   /live/init.mp4    fMP4 init segment (ftyp + moov, avcC from SPS/PPS)
 *   /live/fmp4        live fMP4 stream: init.mp4 followed by moof+mdat
 *                     fragments (Media Source Extensions)
 *   /live/index.m3u8  HLS media playlist (sliding window)
 *   /live/seg_N.m4s   HLS segment N (one moof+mdat per GOP)
 *
 * Video-only (H.264). Wired to the encoder pump in rtspd.c. C89-ish and
 * -Wall clean on the gcc 4.4 ARMv5 uclibc toolchain, no external deps.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "live_http.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Ring of encoded video frames (H.264 Annex-B). 1 slot per frame. */
#define LIVE_RING_FRAMES        72      /* ~4-5 s at 15 fps */
#define LIVE_MAX_AU             40      /* max frames copied per GOP */
#define LIVE_MAX_BYTES          (3*1024*1024)
#define LIVE_TSCALE             90000   /* MP4 timescale (RTP_HZ 90 kHz) */
#define LIVE_SEG_MS             2000    /* HLS target segment duration */
#define LIVE_HLS_SEGMENTS       6       /* sliding window size */
#define LIVE_RECV_TIMEOUT       8       /* request read timeout, seconds */
#define LIVE_POLL_US            50000   /* producer wait loop tick */
#define LIVE404 "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"

typedef struct {
    unsigned char *buf;         /* malloc'd payload */
    int            len;
    unsigned int   pts;         /* gmlib timestamp, ms */
    unsigned int   seq;         /* global monotonic frame sequence */
    int            keyframe;
} live_frame_t;

/* stable snapshot of one frame for the muxer */
typedef struct {
    const unsigned char *p;
    int    len;
    unsigned int pts;
} live_av_t;

static live_frame_t    live_ring[LIVE_RING_FRAMES];
static unsigned int    live_seq = 0;      /* next frame sequence to assign */
static int             live_count = 0;    /* valid frames in the ring */
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;

/* codec-config snapshot captured from the newest keyframe */
static unsigned char live_sps[64];
static unsigned char live_pps[64];
static int           live_sps_len = 0;
static int           live_pps_len = 0;

/* server state */
static int  live_enabled = 0;
static int  live_running = 0;
static int  live_port = 0;
static int  live_listen_fd = -1;
static pthread_t live_thread_id = (pthread_t)0;

/* stream geometry (encoder dims) */
static int live_width  = 1280;
static int live_height = 720;

/* ---------------------------------------------------------------- */
/* byte helpers                                                      */
/* ---------------------------------------------------------------- */

static void live_w32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void live_w16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void live_box_hdr(unsigned char *p, const char *type, unsigned int size)
{
    live_w32(p, size);
    p[4] = (unsigned char)type[0];
    p[5] = (unsigned char)type[1];
    p[6] = (unsigned char)type[2];
    p[7] = (unsigned char)type[3];
}

/* ---------------------------------------------------------------- */
/* Annex-B helpers                                                   */
/* ---------------------------------------------------------------- */

/* Find the first start code at or after `from` (a run of >=2 zero bytes
 * followed by 0x01). Returns the byte index of the FIRST zero of the
 * prefix; *codelen is the total prefix length including the 0x01. */
static int live_find_code(const unsigned char *b, int len, int from,
                          int *codelen)
{
    int i, z;

    for (i = from; i < len; i++) {
        if (b[i] == 0) {
            z = i;
            while (z < len && b[z] == 0)
                z++;
            if (z < len && b[z] == 1) {
                *codelen = z - i + 1;
                return i;
            }
            i = z;
        }
    }
    return -1;
}

/* Convert one Annex-B access unit into MP4 length-prefixed sample bytes
 * (4-byte BE length + payload, no start codes). Returns bytes needed (or
 * written to dst, which may be NULL for a sizing pass). Trailing padding
 * zeros at the end of the buffer are trimmed. */
static int live_annexb_to_lp(unsigned char *dst, const unsigned char *src,
                             int len)
{
    int pos = 0;
    int out = 0;

    while (pos < len) {
        int ci, cl, p, ni, nl, z;
        ci = live_find_code(src, len, pos, &cl);
        if (ci < 0)
            break;
        p = ci + cl;
        ni = live_find_code(src, len, p, &nl);
        z = (ni >= 0) ? ni : len;
        while (z > p && src[z - 1] == 0)      /* trim padding zeros */
            z--;
        if (dst != NULL) {
            live_w32(dst + out, (unsigned int)(z - p));
            memcpy(dst + out + 4, src + p, (size_t)(z - p));
        }
        out += 4 + (z - p);
        pos = (ni >= 0) ? ni : len;
    }
    return out;
}

/* Refresh the SPS/PPS snapshots from a keyframe access unit. H264 NAL
 * types: 7 = SPS, 8 = PPS. */
static void live_update_vps(const unsigned char *buf, int len)
{
    int pos = 0;

    while (pos < len) {
        int ci, cl, p, type, ln, ni;
        ci = live_find_code(buf, len, pos, &cl);
        if (ci < 0)
            break;
        p = ci + cl;
        type = buf[p] & 0x1f;
        ni = live_find_code(buf, len, p, &cl);
        ln = (ni >= 0) ? (ni - p) : (len - p);
        if (type == 7 && ln <= (int)sizeof(live_sps)) {
            memcpy(live_sps, buf + p, (size_t)ln);
            live_sps_len = ln;
        } else if (type == 8 && ln <= (int)sizeof(live_pps)) {
            memcpy(live_pps, buf + p, (size_t)ln);
            live_pps_len = ln;
        }
        pos = (ni >= 0) ? ni : len;
    }
}

/* ---------------------------------------------------------------- */
/* ring helpers (all with the ring lock held)                        */
/* ---------------------------------------------------------------- */

/* Copy a GOP starting at keyframe seq `from` into tmp. Returns the frame
 * count and sets *end_seq to the last copied frame seq. Stops before the
 * next keyframe, at ring gaps, or at memory/frame caps. */
static int live_grab_gop(unsigned int from, unsigned int *end_seq,
                         unsigned char *tmp, int tmpcap,
                         live_av_t *av, int maxn)
{
    int n = 0, bytes = 0;
    unsigned int s;

    pthread_mutex_lock(&live_lock);
    for (s = from; n < maxn && s < live_seq; s++) {
        live_frame_t *f = &live_ring[s % LIVE_RING_FRAMES];
        if (f->buf == NULL || f->seq != s)
            break;
        if (n > 0 && f->keyframe) {
            if (end_seq != NULL)
                *end_seq = s - 1;
            pthread_mutex_unlock(&live_lock);
            return n;
        }
        if (bytes + f->len > tmpcap)
            break;
        memcpy(tmp + bytes, f->buf, (size_t)f->len);
        av[n].p = tmp + bytes;
        av[n].len = f->len;
        av[n].pts = f->pts;
        bytes += f->len;
        n++;
    }
    if (end_seq != NULL)
        *end_seq = (s > 0) ? (s - 1) : 0;
    pthread_mutex_unlock(&live_lock);
    return n;
}

/* Newest keyframe seq in the ring (0 if none). */
static int live_latest_keyframe(unsigned int *seq)
{
    unsigned int c;
    int found = 0;

    pthread_mutex_lock(&live_lock);
    for (c = 0; c < LIVE_RING_FRAMES && c < live_seq; c++) {
        unsigned int s = live_seq - 1 - c;
        live_frame_t *f = &live_ring[s % LIVE_RING_FRAMES];
        if (f->buf == NULL || f->seq != s)
            continue;
        if (f->keyframe) {
            *seq = s;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&live_lock);
    return found;
}

/* Newest keyframe with seq > `after`. Returns 1 and sets *seq. */
static int live_next_keyframe(unsigned int after, unsigned int *seq)
{
    unsigned int c;
    int found = 0;

    pthread_mutex_lock(&live_lock);
    for (c = 0; c < LIVE_RING_FRAMES && c < live_seq; c++) {
        unsigned int s = live_seq - 1 - c;
        live_frame_t *f = &live_ring[s % LIVE_RING_FRAMES];
        if (f->buf == NULL || f->seq != s)
            continue;
        if (s <= after)
            break;
        if (f->keyframe) {
            *seq = s;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&live_lock);
    return found;
}

/* Ordered oldest->newest list of up to `max` keyframes (seq, pts ms). */
static int live_list_keyframes(unsigned int *seqs, unsigned int *pts, int max)
{
    int n = 0;
    unsigned int c;

    pthread_mutex_lock(&live_lock);
    for (c = 0; c < LIVE_RING_FRAMES && c < live_seq; c++) {
        unsigned int s = live_seq - 1 - c;
        live_frame_t *f = &live_ring[s % LIVE_RING_FRAMES];
        if (f->buf == NULL || f->seq != s || !f->keyframe)
            continue;
        memmove(&seqs[1], &seqs[0], sizeof(seqs[0]) * (size_t)n);
        memmove(&pts[1], &pts[0], sizeof(pts[0]) * (size_t)n);
        seqs[0] = s;
        pts[0] = f->pts;
        if (n < max) {
            n++;
            if (n >= max)
                break;
        }
    }
    pthread_mutex_unlock(&live_lock);
    return n;
}

/* Copy the SPS/PPS snapshot (under lock, in case a restart delivers a new
 * codec config mid-stream). */
static void live_grab_vps(unsigned char *sps, int *spslen,
                          unsigned char *pps, int *ppslen)
{
    pthread_mutex_lock(&live_lock);
    memcpy(sps, live_sps, (size_t)live_sps_len);
    memcpy(pps, live_pps, (size_t)live_pps_len);
    *spslen = live_sps_len;
    *ppslen = live_pps_len;
    pthread_mutex_unlock(&live_lock);
}

/* ---------------------------------------------------------------- */
/* MP4 box muxer (shared by fMP4 stream and HLS segments)            */
/* ---------------------------------------------------------------- */

static void live_mtx(unsigned char *p)
{
    memcpy(p, "\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
              "\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00"
              "\x00\x00\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00", 36);
}

/* ftyp + moov. Returns total size. */
static int live_build_init(unsigned char *out)
{
    unsigned char *p = out;
    int sps, pps, high, avcc, i;
    int avc1, stsd, stbl, minf, mdia, trak, moov;

    sps  = live_sps_len;
    pps  = live_pps_len;
    high = (sps > 0 && live_sps[1] == 100) ? 1 : 0;
    avcc = 8 + 6 + 2 + sps + 1 + 2 + pps + (high ? 4 : 0);
    avc1 = 8 + 78 + avcc;
    stsd = 8 + 8 + avc1;
    stbl = 8 + stsd + 16 + 16 + 20 + 16;
    minf = 8 + 20 + 36 + stbl;
    mdia = 8 + 32 + 45 + minf;
    trak = 8 + 92 + mdia;
    moov = 8 + 108 + trak;

    /* ---- ftyp ---- */
    live_box_hdr(p, "ftyp", 32);
    memcpy(p + 8, "isom", 4);
    live_w32(p + 12, 0x200);
    memcpy(p + 16, "isom", 4);
    memcpy(p + 20, "iso2", 4);
    memcpy(p + 24, "mp41", 4);
    memcpy(p + 28, "avc1", 4);
    p += 32;

    /* ---- moov ---- */
    live_box_hdr(p, "moov", (unsigned int)moov);
    p += 8;

    /* mvhd v0 */
    live_box_hdr(p, "mvhd", 108);
    p += 8;
    p += 4;                                   /* version/flags */
    live_w32(p, 0); p += 4;                   /* creation_time */
    live_w32(p, 0); p += 4;                   /* modification_time */
    live_w32(p, LIVE_TSCALE); p += 4;         /* timescale */
    live_w32(p, 0); p += 4;                   /* duration (live) */
    live_w32(p, 0x00010000); p += 4;          /* rate */
    live_w16(p, 0x0100); p += 2;              /* volume */
    live_w16(p, 0); p += 2;                   /* reserved */
    live_w32(p, 0); p += 4;                   /* reserved */
    live_w32(p, 0); p += 4;                   /* reserved */
    live_mtx(p); p += 36;                     /* matrix */
    for (i = 0; i < 6; i++) { live_w32(p, 0); p += 4; }
    live_w32(p, 1); p += 4;                   /* next_track_ID */

    /* ---- trak ---- */
    live_box_hdr(p, "trak", (unsigned int)trak);
    p += 8;
    /* tkhd v0 flags 0x7 */
    live_box_hdr(p, "tkhd", 92);
    p[8] = 0; p[9] = 0; p[10] = 0; p[11] = 0x07;
    p += 12;
    live_w32(p, 0); p += 4;                   /* creation_time */
    live_w32(p, 0); p += 4;                   /* modification_time */
    live_w32(p, 1); p += 4;                   /* track_ID */
    live_w32(p, 0); p += 4;                   /* reserved */
    live_w32(p, 0); p += 4;                   /* duration (live) */
    live_w32(p, 0); p += 4;                   /* reserved */
    live_w32(p, 0); p += 4;                   /* reserved */
    live_w16(p, 0); p += 2;                   /* layer */
    live_w16(p, 0); p += 2;                   /* alt_group */
    live_w16(p, 0); p += 2;                   /* volume */
    live_w16(p, 0); p += 2;                   /* reserved */
    live_mtx(p); p += 36;                     /* matrix */
    live_w32(p, (unsigned int)live_width << 16); p += 4;   /* fp16 width */
    live_w32(p, (unsigned int)live_height << 16); p += 4;  /* fp16 height */

    /* ---- mdia ---- */
    live_box_hdr(p, "mdia", (unsigned int)mdia);
    p += 8;
    /* mdhd v0 */
    live_box_hdr(p, "mdhd", 32);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 0); p += 4;
    live_w32(p, 0); p += 4;
    live_w32(p, LIVE_TSCALE); p += 4;
    live_w32(p, 0); p += 4;                   /* duration */
    live_w16(p, 0x55C4); p += 2;              /* language "und" */
    live_w16(p, 0); p += 2;
    /* hdlr */
    live_box_hdr(p, "hdlr", 45);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 0); p += 4;                   /* pre_defined */
    memcpy(p, "vide", 4); p += 4;             /* handler_type */
    for (i = 0; i < 12; i++) p[i] = 0;        /* reserved */
    p += 12;
    memcpy(p, "VideoHandler", 13); p += 13;

    /* ---- minf ---- */
    live_box_hdr(p, "minf", (unsigned int)(20 + 36 + stbl));
    p += 8;
    /* vmhd */
    live_box_hdr(p, "vmhd", 20);
    p[8] = 0; p[9] = 0; p[10] = 0; p[11] = 1;
    p += 12;
    live_w16(p, 0); p += 2;                   /* graphicsmode */
    live_w16(p, 0); p += 2;                   /* opcolor */
    live_w16(p, 0); p += 2;
    live_w16(p, 0); p += 2;
    /* dinf */
    live_box_hdr(p, "dinf", 36);
    p += 8;
    live_box_hdr(p, "dref", 28);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 1); p += 4;                   /* entry_count */
    live_box_hdr(p, "url ", 12);
    p[8] = 0; p[9] = 0; p[10] = 0; p[11] = 1; /* self-contained */
    p += 12;
    /* stbl */
    live_box_hdr(p, "stbl", (unsigned int)stbl);
    p += 8;
    /* stsd */
    live_box_hdr(p, "stsd", (unsigned int)stsd);
    p[8] = 0; p[9] = 0; p[10] = 0; p[11] = 0; /* version/flags */
    live_w32(p + 12, 1);                      /* entry_count */
    p += 16;
    /* avc1 visual sample entry */
    live_box_hdr(p, "avc1", (unsigned int)avc1);
    memset(p + 8, 0, 6);                      /* reserved */
    live_w16(p + 14, 1);                      /* data_reference_index */
    live_w16(p + 16, 0);                      /* pre_defined */
    live_w16(p + 18, 0);                      /* reserved */
    for (i = 20; i < 32; i++) p[i] = 0;       /* pre_defined[3] */
    live_w16(p + 32, (unsigned short)live_width);
    live_w16(p + 34, (unsigned short)live_height);
    live_w32(p + 36, 0x00480000);             /* horizresolution */
    live_w32(p + 40, 0x00480000);             /* vertresolution */
    live_w32(p + 44, 0);                      /* reserved */
    live_w16(p + 48, 1);                      /* frame_count */
    memset(p + 50, 0, 32);                    /* compressorname */
    memcpy(p + 50, "rtspd", 6);
    live_w16(p + 82, 0x0018);                 /* depth */
    live_w16(p + 84, 0xFFFF);                 /* pre_defined */
    p += 86;
    /* avcC */
    live_box_hdr(p, "avcC", (unsigned int)avcc);
    p += 8;
    p[0] = 1;                                 /* configurationVersion */
    p[1] = (sps > 0) ? live_sps[1] : 66;      /* AVCProfileIndication */
    p[2] = (sps > 0) ? live_sps[2] : 0;       /* profile_compatibility */
    p[3] = (sps > 0) ? live_sps[3] : 30;      /* AVCLevelIndication */
    p[4] = 0xFF;                              /* reserved + lengthSize=4 */
    p[5] = 0xE1;                              /* numOfSequenceParameterSets */
    live_w16(p + 6, (unsigned int)sps);
    if (sps > 0)
        memcpy(p + 8, live_sps, (size_t)sps);
    p += 8 + sps;
    p[0] = 1;                                 /* numOfPictureParameterSets */
    live_w16(p + 1, (unsigned int)pps);
    if (pps > 0)
        memcpy(p + 3, live_pps, (size_t)pps);
    p += 3 + pps;
    if (high) {
        p[0] = 0xFD;                          /* chroma_format_idc = 1 */
        p[1] = 0xF8;                          /* bit_depth_luma_minus8 = 0 */
        p[2] = 0xF8;                          /* bit_depth_chroma_minus8 = 0 */
        p[3] = 0;                             /* numOfSequenceParameterSetExt */
        p += 4;
    }
    /* stts / stsc / stsz / stco (empty sample tables) */
    live_box_hdr(p, "stts", 16);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 0); p += 4;                   /* entry_count */
    live_box_hdr(p, "stsc", 16);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 0); p += 4;
    live_box_hdr(p, "stsz", 20);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 0); p += 4;                   /* sample_size */
    live_w32(p, 0); p += 4;                   /* sample_count */
    live_box_hdr(p, "stco", 16);
    p += 8;
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;   /* version/flags */
    p += 4;
    live_w32(p, 0); p += 4;                   /* entry_count */

    return (int)(p - out);
}

/* Build one fragment (moof + mdat) from `n` access units. pts_base is the
 * decode time of the first sample in 90 kHz ticks. Returns bytes written
 * into out (0 on error). Capacity must be >= moof_size + 8 + sum(lp). */
static int live_build_frag(unsigned char *out, int outcap,
                           const live_av_t *av, int n,
                           unsigned int mfhd_seq, unsigned long long pts_base)
{
    unsigned long long prev_pts, this_pts;
    unsigned int data_off, moof_size;
    int i, lp, total, slen[LIVE_MAX_AU];
    unsigned char *q;

    if (n <= 0 || n > LIVE_MAX_AU)
        return 0;
    moof_size = 92 + 16 * n;                  /* mfhd16 + traf(12+16+20) + trun20 */
    data_off = moof_size + 8;
    total = 0;
    for (i = 0; i < n; i++) {
        lp = live_annexb_to_lp(NULL, av[i].p, av[i].len);
        if (lp < 0)
            return 0;
        slen[i] = lp;
        total += lp;
        if ((int)(moof_size + 8 + total) > outcap)
            return 0;
    }

    /* ---- moof ---- */
    live_box_hdr(out, "moof", moof_size);
    live_box_hdr(out + 8, "mfhd", 16);
    out[16] = 0; out[17] = 0; out[18] = 0; out[19] = 0;  /* version/flags */
    live_w32(out + 20, mfhd_seq);                         /* sequence_number */
    q = out + 24;
    live_box_hdr(q, "traf", (unsigned int)(12 + 16 + 20 + 20 + 16 * n));
    q[8] = 0; q[9] = 0; q[10] = 0; q[11] = 0;
    q += 12;
    /* tfhd: v0, flags = default-base-is-moof, track_ID 1 */
    live_box_hdr(q, "tfhd", 16);
    q[8] = 0; q[9] = 0; q[10] = 0x02; q[11] = 0;
    live_w32(q + 12, 1);
    q += 16;
    /* tfdt: v1, 64-bit baseMediaDecodeTime */
    live_box_hdr(q, "tfdt", 20);
    q[8] = 1; q[9] = 0; q[10] = 0; q[11] = 0;
    live_w32(q + 12, (unsigned int)(pts_base >> 32));
    live_w32(q + 16, (unsigned int)pts_base);
    q += 20;
    /* trun: flags = data-offset | duration | size | flags | cto */
    live_box_hdr(q, "trun", (unsigned int)(20 + 16 * n));
    q[8] = 0; q[9] = 0; q[10] = 0x07; q[11] = 0x01;
    live_w32(q + 12, (unsigned int)n);
    live_w32(q + 16, data_off);
    q += 20;
    prev_pts = pts_base;
    for (i = 0; i < n; i++) {
        this_pts = (unsigned long long)av[i].pts * LIVE_TSCALE / 1000ull;
        if (this_pts < prev_pts)               /* clock wrap / out-of-order */
            this_pts = prev_pts;
        live_w32(q, (unsigned int)(this_pts - prev_pts));
        live_w32(q + 4, (unsigned int)slen[i]);
        live_w32(q + 8, 0);                    /* sample flags */
        live_w32(q + 12, 0);                   /* composition offset */
        q += 16;
        prev_pts = this_pts;
    }
    if (n == 1)
        live_w32(q - 16, LIVE_TSCALE / 20);    /* guard zero-duration sole sample */

    /* ---- mdat ---- */
    live_box_hdr(q, "mdat", (unsigned int)(8 + total));
    q += 8;
    for (i = 0; i < n; i++) {
        int w = live_annexb_to_lp(q, av[i].p, av[i].len);
        q += w;
    }
    return (int)(q - out);
}

/* ---------------------------------------------------------------- */
/* HTTP helpers                                                      */
/* ---------------------------------------------------------------- */

static int live_send(int fd, const void *buf, int len)
{
    int off = 0;

    while (off < len) {
        int w = (int)send(fd, (const char *)buf + off, (size_t)(len - off),
                          MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += w;
    }
    return 0;
}

static int live_send_head(int fd, const char *ctype, long len)
{
    char h[256];
    int m;

    if (len >= 0) {
        m = snprintf(h, sizeof(h),
                     "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
                     "Content-Length: %ld\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Cache-Control: no-store, no-cache\r\n"
                     "Connection: close\r\n\r\n", ctype, len);
    } else {
        m = snprintf(h, sizeof(h),
                     "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Cache-Control: no-store, no-cache\r\n"
                     "Connection: close\r\n\r\n", ctype);
    }
    if (m < 0)
        return -1;
    if (m >= (int)sizeof(h))
        m = (int)sizeof(h) - 1;
    return live_send(fd, h, m);
}

static void live_send_404(int fd)
{
    live_send(fd, LIVE404, (int)strlen(LIVE404));
}

/* ---------------------------------------------------------------- */
/* request handlers                                                  */
/* ---------------------------------------------------------------- */

/* /live/init.mp4 */
static int live_serve_init(int fd)
{
    static unsigned char init[1024];
    unsigned char sps[64], pps[64];
    int spslen = 0, ppslen = 0, n;

    live_grab_vps(sps, &spslen, pps, &ppslen);
    if (spslen <= 0 || ppslen <= 0)
        return -1;
    n = live_build_init(init);
    if (n <= 0)
        return -1;
    if (live_send_head(fd, "video/mp4", n) != 0)
        return -1;
    return live_send(fd, init, n);
}

/* /live/fmp4 : live fMP4 stream (MSE). init + fragments, no content-length. */
static int live_serve_fmp4(int fd)
{
    static unsigned char init[1024];
    unsigned char *tmp, *frag;
    live_av_t av[LIVE_MAX_AU];
    unsigned char sps[64], pps[64];
    int spslen = 0, ppslen = 0, n;
    unsigned int cursor, kf, segseq = 0;
    unsigned long long base;
    int ok;

    tmp = (unsigned char *)malloc(LIVE_MAX_BYTES);
    frag = (unsigned char *)malloc(LIVE_MAX_BYTES);
    if (tmp == NULL || frag == NULL) {
        free(tmp);
        free(frag);
        return -1;
    }

    live_grab_vps(sps, &spslen, pps, &ppslen);
    if (spslen <= 0 || ppslen <= 0) {
        free(tmp);
        free(frag);
        return -1;
    }
    n = live_build_init(init);
    if (n <= 0) {
        free(tmp);
        free(frag);
        return -1;
    }

    /* start from the newest full GOP */
    if (!live_latest_keyframe(&kf)) {
        free(tmp);
        free(frag);
        return -1;
    }
    cursor = kf - 1;

    if (live_send_head(fd, "video/mp4", -1) != 0 ||
        live_send(fd, init, n) != 0) {
        free(tmp);
        free(frag);
        return -1;
    }

    while (live_running) {
        unsigned int end;
        ok = live_next_keyframe(cursor, &kf);
        if (!ok) {
            usleep(LIVE_POLL_US);
            continue;
        }
        end = 0;
        n = live_grab_gop(kf, &end, tmp, LIVE_MAX_BYTES, av, LIVE_MAX_AU);
        if (n <= 0) {
            cursor = kf;                 /* skip the broken boundary */
            continue;
        }
        cursor = end;
        base = (unsigned long long)av[0].pts * LIVE_TSCALE / 1000ull;
        n = live_build_frag(frag, LIVE_MAX_BYTES, av, n, segseq++, base);
        if (n <= 0)
            continue;
        if (live_send(fd, frag, n) != 0)
            break;
    }

    free(tmp);
    free(frag);
    return 0;
}

/* /live/index.m3u8 */
static int live_serve_playlist(int fd)
{
    unsigned int seqs[LIVE_HLS_SEGMENTS], pts[LIVE_HLS_SEGMENTS];
    double dur[LIVE_HLS_SEGMENTS];
    char txt[2048];
    int targ = 2, n, i, m;
    size_t len = 0;

    n = live_list_keyframes(seqs, pts, LIVE_HLS_SEGMENTS);
    if (n <= 0)
        return -1;
    for (i = 0; i < n; i++) {
        dur[i] = (i + 1 < n) ? (double)(pts[i + 1] - pts[i]) / 1000.0
                             : (double)LIVE_SEG_MS / 1000.0;
        if (dur[i] <= 0.0)
            dur[i] = (double)LIVE_SEG_MS / 1000.0;
        if ((int)(dur[i] + 0.999) > targ)
            targ = (int)(dur[i] + 0.999);
    }
    len += (size_t)snprintf(txt + len, sizeof(txt) - len,
                    "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n"
                    "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:%u\n"
                    "#EXT-X-MAP:URI=\"init.mp4\"\n", targ, seqs[0]);
    for (i = 0; i < n; i++) {
        m = snprintf(txt + len, sizeof(txt) - len,
                     "#EXTINF:%.6f,\nseg_%u.m4s\n", dur[i], seqs[i]);
        if (m <= 0)
            break;
        len += (size_t)m;
    }
    if (len <= 0)
        return -1;
    if (live_send_head(fd, "application/vnd.apple.mpegurl", (int)len) != 0)
        return -1;
    return live_send(fd, txt, (int)len);
}

/* /live/seg_N.m4s */
static int live_serve_segment(int fd, unsigned int seg)
{
    unsigned char *tmp, *frag;
    live_av_t av[LIVE_MAX_AU];
    unsigned long long base;
    int n, flen;

    tmp = (unsigned char *)malloc(LIVE_MAX_BYTES);
    frag = (unsigned char *)malloc(LIVE_MAX_BYTES);
    if (tmp == NULL || frag == NULL) {
        free(tmp);
        free(frag);
        return -1;
    }
    n = live_grab_gop(seg, NULL, tmp, LIVE_MAX_BYTES, av, LIVE_MAX_AU);
    if (n <= 0) {
        free(tmp);
        free(frag);
        return -1;
    }
    base = (unsigned long long)av[0].pts * LIVE_TSCALE / 1000ull;
    flen = live_build_frag(frag, LIVE_MAX_BYTES, av, n, seg, base);
    if (flen <= 0) {
        free(tmp);
        free(frag);
        return -1;
    }
    if (live_send_head(fd, "video/mp4", flen) != 0) {
        free(tmp);
        free(frag);
        return -1;
    }
    live_send(fd, frag, flen);
    free(tmp);
    free(frag);
    return 0;
}

/* ---------------------------------------------------------------- */
/* connection handling                                               */
/* ---------------------------------------------------------------- */

static int live_http_read_req(int fd, char *buf, int cap)
{
    int n = 0;
    fd_set fs;
    struct timeval tv;

    while (n < cap - 1) {
        int r;
        FD_ZERO(&fs);
        FD_SET(fd, &fs);
        tv.tv_sec = LIVE_RECV_TIMEOUT;
        tv.tv_usec = 0;
        r = select(fd + 1, &fs, NULL, NULL, &tv);
        if (r <= 0)
            break;
        r = (int)recv(fd, buf + n, (size_t)(cap - 1 - n), 0);
        if (r <= 0)
            break;
        n += r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL)
            break;
    }
    return n;
}

static int live_parse_path(const char *req, char *path, int cap)
{
    const char *sp, *ep;
    int n;

    sp = strchr(req, ' ');
    if (sp == NULL)
        return -1;
    sp++;
    ep = strchr(sp, ' ');
    if (ep == NULL)
        return -1;
    n = (int)(ep - sp);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(path, sp, (size_t)n);
    path[n] = '\0';
    {
        char *q = strchr(path, '?');
        if (q != NULL)
            *q = '\0';
    }
    return 0;
}

static void *live_http_conn_thread(void *arg)
{
    int fd = *(int *)arg;
    char req[1024];
    char path[160];
    unsigned int seg = 0;
    int n;

    n = live_http_read_req(fd, req, (int)sizeof(req));
    if (n > 0 && live_parse_path(req, path, (int)sizeof(path)) == 0) {
        if (strcmp(path, "/live/init.mp4") == 0) {
            if (live_serve_init(fd) != 0)
                live_send_404(fd);
        } else if (strcmp(path, "/live/fmp4") == 0) {
            if (live_serve_fmp4(fd) != 0)
                live_send_404(fd);
        } else if (strcmp(path, "/live/index.m3u8") == 0) {
            if (live_serve_playlist(fd) != 0)
                live_send_404(fd);
        } else if (sscanf(path, "/live/seg_%u.m4s", &seg) == 1) {
            if (live_serve_segment(fd, seg) != 0)
                live_send_404(fd);
        } else {
            live_send_404(fd);
        }
    } else {
        live_send_404(fd);
    }

    close(fd);
    free(arg);
    return NULL;
}

static void *live_http_server_thread(void *arg)
{
    struct sockaddr_in sa;
    socklen_t al;
    int on = 1, cfd;

    (void)arg;
    pthread_mutex_lock(&live_lock);
    live_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (live_listen_fd < 0) {
        live_enabled = 0;
        pthread_mutex_unlock(&live_lock);
        return NULL;
    }
    setsockopt(live_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)live_port);
    if (bind(live_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(live_listen_fd, 4) != 0) {
        close(live_listen_fd);
        live_listen_fd = -1;
        live_enabled = 0;
        pthread_mutex_unlock(&live_lock);
        return NULL;
    }
    live_running = 1;
    pthread_mutex_unlock(&live_lock);

    while (live_running) {
        al = sizeof(sa);
        cfd = (int)accept(live_listen_fd, (struct sockaddr *)&sa, &al);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            if (!live_running)
                break;
            usleep(10000);
            continue;
        }
        {
            int *afd = (int *)malloc(sizeof(int));
            pthread_t t;
            pthread_attr_t attr;
            if (afd != NULL) {
                *afd = cfd;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                if (pthread_create(&t, &attr, &live_http_conn_thread, afd) != 0) {
                    close(cfd);
                    free(afd);
                }
                pthread_attr_destroy(&attr);
            } else {
                close(cfd);
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* public API                                                        */
/* ---------------------------------------------------------------- */

int live_http_enabled(void)
{
    return live_listen_fd >= 0;
}

void live_http_feed_video(const unsigned char *buf, int len,
                          unsigned int pts_ms, int keyframe)
{
    live_frame_t *f;
    unsigned int seq;

    if (live_listen_fd < 0 || buf == NULL || len <= 0)
        return;

    if (keyframe) {
        live_update_vps(buf, len);
        if (live_sps_len <= 0)
            return;               /* nothing muxable yet */
    }

    pthread_mutex_lock(&live_lock);
    seq = live_seq++;
    f = &live_ring[seq % LIVE_RING_FRAMES];
    if (f->buf != NULL)
        free(f->buf);
    f->buf = (unsigned char *)malloc((size_t)len);
    if (f->buf == NULL) {
        f->len = 0;
        f->seq = 0;
        live_seq = seq;           /* roll back */
        pthread_mutex_unlock(&live_lock);
        return;
    }
    memcpy(f->buf, buf, (size_t)len);
    f->len = len;
    f->pts = pts_ms;
    f->seq = seq;
    f->keyframe = keyframe ? 1 : 0;
    if (live_count < LIVE_RING_FRAMES)
        live_count++;
    pthread_mutex_unlock(&live_lock);
}

int live_http_init(int port, int width, int height, int fps)
{
    pthread_attr_t attr;

    (void)fps;
    if (port <= 0)
        return 0;
    live_port = port;
    live_width = width;
    live_height = height;
    live_enabled = 1;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&live_thread_id, &attr, &live_http_server_thread,
                       NULL) != 0) {
        live_enabled = 0;
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void live_http_stop(void)
{
    int fd;

    live_running = 0;
    pthread_mutex_lock(&live_lock);
    fd = live_listen_fd;
    live_listen_fd = -1;
    pthread_mutex_unlock(&live_lock);
    if (fd >= 0)
        close(fd);
}