/* @file miio_client.c
 *
 *  Open-source, feature-level reconstruction of the Xiaomi Mi-IOT daemon
 *  `miio_client` (vendor version 3.3.9) from a Xiaomi Chuangmi v5
 *  (IMI/GM8136, buildroot 2016.02) firmware dump.
 *
 *  The vendor binary (`chuangmi-v5-dump/data/ot_wifi_tool/miio_client`,
 *  81828-byte ARM ELF, stripped: no symtab/DWARF, only .dynsym + .rodata)
 *  is the MIIO OT-protocol client. It keeps the OpenAI-OT (OT) connection
 *  to Xiaomi's cloud (get device id/key/token via the wifi-helper script,
 *  then stream local.status/props/_otc.info), listens for LAN app commands
 *  ("OT agent" phone channel) and serves the local line used by
 *  `recv_line`/`miot_devicekit`/`miio_send_line`.
 *
 *  Because the binary is STRIPPED this is a *feature-level* reconstruction:
 *  the whole functional surface (states, OT packet format, crypto, cloud
 *  retry/keepalive/ack, local channels, mdns) is rebuilt from the .rodata
 *  strings + .dynsym imports that survived in the dump. Where the exact
 *  algorithm is not preserved (e.g. the crypto in a stripped tumble, exact
 *  queue shaping) the code implements the publicly documented behaviour and
 *  flags the choice with "RECON:".
 *
 *  Evidence (rodata, dump addr -> string):
 *    states             "device_init","didkey_req1","didkey_req2",
 *                       "didkey_done","token_done","sta_mode",
 *                       "cloud_trying","cloud_connected","cloud_retry"
 *    cloud              "ot.io.mi.com"/"ott.io.mi.com", port "8053",
 *                       "getaddrinfo (%s:%s): %s", "server_conn_retry"
 *    OT packet          32-byte header, "OT received (%d), less than
 *                       size of OT header (32)", "OT MD5 not match."
 *    crypto             "5/d$VdmW6,~0u>#k" (XOR pad), "73bcea2K" (token)
 *    ack/queue          "data desc added(+) id: %d", "ack_timer_handler",
 *                       "OT server noack", "data_queue_noack"
 *    timers             "miio.timerfd > 0", "Sync timer fd: %d",
 *                       "ACK timer fd: %d"
 *    local channels     default TCP 54321 (word 0x0000D431 @ 0x2A94),
 *                       line 54322 (0xD432 @ 0x2AB0), bt 54323 (0xD433
 *                       @ 0x2AC4)
 *    mdns               "_miio._udp.local", "path=/mydevice",
 *                       "224.0.0.251", mdnsd.c, PTR/TXT/SRV/AAAA/NSEC/ANY
 *    getopt             "Dp:H:i:l:L:hvd:eEC:" + long option names
 *
 *  The OT agent port defaults came out of a byte scan of the binary
 *  (htons constants): 54321 = OT-agent, 54322 = local line, 54323 is
 *  used for the "bt" (helper/BLE-named) conn.
 *
 *  Build (repository Makefile):
 *      make build/miio_client
 */

/* ------------------------------------------------------------------ */
/* includes                                                            */
/* ------------------------------------------------------------------ */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* tunables                                                            */
/* ------------------------------------------------------------------ */
#define MIIO_VERSION            "3.3.9"
#define DEFAULT_DIR             "/etc/miio/"
#define DEFAULT_TCP_HOST        "ot.io.mi.com"
#define DEFAULT_UDP_HOST        "ott.io.mi.com"
#define DEFAULT_CLOUD_PORT      "8053"
#define DEFAULT_PORT            54321          /* OT-agent (phone) channel */
#define DEFAULT_LINE_PORT       54322          /* recv_line / devicekit   */
#define DEFAULT_BT_PORT         54323          /* helper/ble-named conn   */
#define DEFAULT_SYNC_INTERVAL   60000           /* _otc.info report ms    */
#define OT_HEADER_SIZE          32
#define OT_MAGIC                0x2131         /* "!1"                    */
#define OT_MAX_PAYLOAD          0x4000
#define MAX_POLLFDS             16
#define MAX_CLIENTS             8
#define MAX_SERVERS             8
#define MAX_TOKEN_LEN           33
#define MAX_KEY_LEN             33
#define MAX_UID_LEN             33
#define MAX_HOST_LEN            64
#define LOG_BUF                 512

/* forward decls for call ordering */
static void generate_random_ntoken(char *buf, int size);
static void cmd_internal_request_dcountry(int fd);
static int report_msg_general_callback(const char *msg);

/* xor pads that survived in .rodata */
#define XOR_KEY                 "5/d$VdmW6,~0u>#k"
#define XOR_TOKEN_KEY           "73bcea2K"

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
enum {
    LOG_ERROR = 0,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG,
    LOG_VERBOSE,
};

static int loglevel = LOG_INFO;          /* 0-4, bigger = more verbose */
static const char *log_level_str[] = {
    "[ERROR] ", "[WARNING] ", "[INFO] ", "[DEBUG] ", "[VERBOSE] ",
};
static FILE *logfp = NULL;

static void log_printf(int level, const char *fmt, ...)
{
    char buf[LOG_BUF];
    char ts[32];
    struct timeval tv;
    struct tm tm;
    va_list ap;
    int n;

    if (level > loglevel || fmt == NULL)
        return;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof(ts), "[%Y%m%d %H:%M:%S]", &tm);
    n = snprintf(buf, sizeof(buf), "%s %s", ts, log_level_str[level]);
    if (n < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    fputs(buf, logfp ? logfp : stdout);
    fflush(logfp ? logfp : stdout);
}

static void hex_dump(const unsigned char *p, int len)
    __attribute__((unused));
static void hex_dump(const unsigned char *p, int len)
{
    int i;
    for (i = 0; i < len; i++)
        fprintf(logfp ? logfp : stdout, "0x%02x ", p[i]);
    fputc('\n', logfp ? logfp : stdout);
}

/* ------------------------------------------------------------------ */
/* small util helpers                                                  */
/* ------------------------------------------------------------------ */
static void hex2str(const unsigned char *in, int inlen, char *out,
                    int outsize)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    int n = inlen * 2;

    if (outsize < n + 1) {
        log_printf(LOG_WARNING,
                   "buflen(%d) might not be enough for the hexlen(%d) "
                   "convertion.", outsize, inlen);
        return;
    }
    for (i = 0; i < inlen; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[n] = '\0';
}

static int str2hex(const char *s, unsigned char *out, int outsize)
{
    int len = (int)strlen(s) / 2;
    int i;

    if (len > outsize)
        len = outsize;
    for (i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(s + i * 2, "%2x", &v) != 1)
            return 0;
        out[i] = (unsigned char)v;
    }
    return len;
}

static unsigned long long get_micro_second(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL
        + (unsigned long long)ts.tv_nsec / 1000ULL;
}

static int get_uptime(void)
{
    struct sysinfo si;

    memset(&si, 0, sizeof(si));
    if (sysinfo(&si) != 0) {
        log_printf(LOG_WARNING, "get uptime fail, %m");
        return 0;
    }
    return (int)si.uptime;
}

/* ------------------------------------------------------------------ */
/* simplified JSON accessor set (vendor names kept: json_verify,       */
/* json_verify_method, json_verify_method_value, json_verify_get_int)  */
/* ------------------------------------------------------------------ */
static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (*p == '{') {
        p++;
        while (*p && *p != '}') {
            p = json_skip_value(p);          /* key */
            p = json_skip_value(p);          /* value */
        }
        if (*p == '}')
            p++;
    } else if (*p == '[') {
        p++;
        while (*p && *p != ']') {
            p = json_skip_value(p);
            if (*p == ',')
                p++;
        }
        if (*p == ']')
            p++;
    } else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        if (*p == '"')
            p++;
        p = json_skip_ws(p);
        if (*p == ',')
            p++;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        p = json_skip_ws(p);
        if (*p == ',')
            p++;
    }
    return p;
}

static const char *json_find_value(const char *json, const char *key)
{
    const char *p = json;
    int klen = (int)strlen(key);

    while ((p = strchr(p, '"')) != NULL) {
        const char *k = p + 1;
        int i;
        for (i = 0; i < klen; i++) {
            if (k[i] != key[i])
                break;
        }
        if (i == klen && k[i] == '"') {
            p = json_skip_ws(k + i + 1);
            if (*p != ':')
                continue;
            return json_skip_ws(p + 1);
        }
        p = k;
    }
    return NULL;
}

static int json_verify(const char *str)
{
    return (str != NULL && str[0] == '{') ? 0 : -1;
}

static int json_verify_method(const char *str, const char *method)
{
    const char *v;
    int mlen = (int)strlen(method);

    if (json_verify(str) != 0)
        return -1;
    v = json_find_value(str, "method");
    if (v == NULL)
        return -2;
    if (*v == '"')
        v++;
    return strncmp(v, method, mlen) == 0 ? 0 : -3;
}

static int json_verify_method_value(const char *str, const char *method,
                                    const char *key, const char *value)
{
    const char *v;

    if (json_verify_method(str, method) != 0)
        return -1;
    v = json_find_value(str, key);
    if (v == NULL)
        return -2;
    if (*v == '"')
        return strncmp(v + 1, value, strlen(value)) == 0 ? 0 : -3;
    return strncmp(v, value, strlen(value)) == 0 ? 0 : -3;
}

static int json_verify_get_int(const char *str, const char *key, int *value)
{
    const char *v = json_find_value(str, key);

    if (v == NULL)
        return -1;
    if (*v == '"')
        v++;
    if (*v == '{' || *v == '[')
        return -2;
    *value = atoi(v);
    return 0;
}

static int json_verify_get_string(const char *str, const char *key,
                                  char *out, int outsize)
{
    const char *v = json_find_value(str, key);
    const char *e;
    int n;

    if (v == NULL)
        return -1;
    if (*v != '"')
        return -2;
    v++;
    e = strchr(v, '"');
    if (e == NULL)
        return -3;
    n = (int)(e - v);
    if (n >= outsize)
        n = outsize - 1;
    memcpy(out, v, n);
    out[n] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* MD5 (self-contained; no crypto lib in .dynsym)                      */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t state[4];
    uint64_t count;
    unsigned char buffer[64];
} md5_ctx;

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define MD5_FF(a, b, c, d, x, s, ac) { \
    (a) += MD5_F((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTL((a), (s)); \
    (a) += (b); }
#define MD5_GG(a, b, c, d, x, s, ac) { \
    (a) += MD5_G((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTL((a), (s)); \
    (a) += (b); }
#define MD5_HH(a, b, c, d, x, s, ac) { \
    (a) += MD5_H((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTL((a), (s)); \
    (a) += (b); }
#define MD5_II(a, b, c, d, x, s, ac) { \
    (a) += MD5_I((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTL((a), (s)); \
    (a) += (b); }

static const unsigned char md5_padding[64] = { 0x80 };

static void md5_transform(md5_ctx *ctx, const unsigned char buf[64])
{
    uint32_t a = ctx->state[0], b = ctx->state[1];
    uint32_t c = ctx->state[2], d = ctx->state[3];
    uint32_t x[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = (uint32_t)buf[i * 4]
            | ((uint32_t)buf[i * 4 + 1] << 8)
            | ((uint32_t)buf[i * 4 + 2] << 16)
            | ((uint32_t)buf[i * 4 + 3] << 24);
    }
    MD5_FF(a, b, c, d, x[0], 7, 0xd76aa478);
    MD5_FF(d, a, b, c, x[1], 12, 0xe8c7b756);
    MD5_FF(c, d, a, b, x[2], 17, 0x242070db);
    MD5_FF(b, c, d, a, x[3], 22, 0xc1bdceee);
    MD5_FF(a, b, c, d, x[4], 7, 0xf57c0faf);
    MD5_FF(d, a, b, c, x[5], 12, 0x4787c62a);
    MD5_FF(c, d, a, b, x[6], 17, 0xa8304613);
    MD5_FF(b, c, d, a, x[7], 22, 0xfd469501);
    MD5_FF(a, b, c, d, x[8], 7, 0x698098d8);
    MD5_FF(d, a, b, c, x[9], 12, 0x8b44f7af);
    MD5_FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    MD5_FF(b, c, d, a, x[11], 22, 0x895cd7be);
    MD5_FF(a, b, c, d, x[12], 7, 0x6b901122);
    MD5_FF(d, a, b, c, x[13], 12, 0xfd987193);
    MD5_FF(c, d, a, b, x[14], 17, 0xa679438e);
    MD5_FF(b, c, d, a, x[15], 22, 0x49b40821);

    MD5_GG(a, b, c, d, x[1], 5, 0xf61e2562);
    MD5_GG(d, a, b, c, x[6], 9, 0xc040b340);
    MD5_GG(c, d, a, b, x[11], 14, 0x265e5a51);
    MD5_GG(b, c, d, a, x[0], 20, 0xe9b6c7aa);
    MD5_GG(a, b, c, d, x[5], 5, 0xd62f105d);
    MD5_GG(d, a, b, c, x[10], 9, 0x02441453);
    MD5_GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    MD5_GG(b, c, d, a, x[4], 20, 0xe7d3fbc8);
    MD5_GG(a, b, c, d, x[9], 5, 0x21e1cde6);
    MD5_GG(d, a, b, c, x[14], 9, 0xc33707d6);
    MD5_GG(c, d, a, b, x[3], 14, 0xf4d50d87);
    MD5_GG(b, c, d, a, x[8], 20, 0x455a14ed);
    MD5_GG(a, b, c, d, x[13], 5, 0xa9e3e905);
    MD5_GG(d, a, b, c, x[2], 9, 0xfcefa3f8);
    MD5_GG(c, d, a, b, x[7], 14, 0x676f02d9);
    MD5_GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    MD5_HH(a, b, c, d, x[5], 4, 0xfffa3942);
    MD5_HH(d, a, b, c, x[8], 11, 0x8771f681);
    MD5_HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    MD5_HH(b, c, d, a, x[14], 23, 0xfde5380c);
    MD5_HH(a, b, c, d, x[1], 4, 0xa4beea44);
    MD5_HH(d, a, b, c, x[4], 11, 0x4bdecfa9);
    MD5_HH(c, d, a, b, x[7], 16, 0xf6bb4b60);
    MD5_HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    MD5_HH(a, b, c, d, x[13], 4, 0x289b7ec6);
    MD5_HH(d, a, b, c, x[0], 11, 0xeaa127fa);
    MD5_HH(c, d, a, b, x[3], 16, 0xd4ef3085);
    MD5_HH(b, c, d, a, x[6], 23, 0x04881d05);
    MD5_HH(a, b, c, d, x[9], 4, 0xd9d4d039);
    MD5_HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    MD5_HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    MD5_HH(b, c, d, a, x[2], 23, 0xc4ac5665);

    MD5_II(a, b, c, d, x[0], 6, 0xf4292244);
    MD5_II(d, a, b, c, x[7], 10, 0x432aff97);
    MD5_II(c, d, a, b, x[14], 15, 0xab9423a7);
    MD5_II(b, c, d, a, x[5], 21, 0xfc93a039);
    MD5_II(a, b, c, d, x[12], 6, 0x655b59c3);
    MD5_II(d, a, b, c, x[3], 10, 0x8f0ccc92);
    MD5_II(c, d, a, b, x[10], 15, 0xffeff47d);
    MD5_II(b, c, d, a, x[1], 21, 0x85845dd1);
    MD5_II(a, b, c, d, x[8], 6, 0x6fa87e4f);
    MD5_II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    MD5_II(c, d, a, b, x[6], 15, 0xa3014314);
    MD5_II(b, c, d, a, x[13], 21, 0x4e0811a1);
    MD5_II(a, b, c, d, x[4], 6, 0xf7537e82);
    MD5_II(d, a, b, c, x[11], 10, 0xbd3af235);
    MD5_II(c, d, a, b, x[2], 15, 0x2ad7d2bb);
    MD5_II(b, c, d, a, x[9], 21, 0xeb86d391);

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md5_init(md5_ctx *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

static void md5_update(md5_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t have = (size_t)((ctx->count / 8) % 64);
    size_t need = 64 - have;
    size_t i;

    i = 0;
    if (len >= need) {
        if (have)
            memcpy(ctx->buffer + have, p, need);
        if (have)
            md5_transform(ctx, ctx->buffer);
        else
            md5_transform(ctx, p);
        i = need;
        for (; i + 63 < len; i += 64)
            md5_transform(ctx, p + i);
        have = 0;
    }
    if (i < len)
        memcpy(ctx->buffer + have, p + i, len - i);
    ctx->count += (uint64_t)len * 8;
}

static void md5_final(md5_ctx *ctx, unsigned char digest[16])
{
    unsigned char bits[8];
    unsigned int idx, padlen;
    int i;

    for (i = 0; i < 8; i++)
        bits[i] = (unsigned char)(ctx->count >> (i * 8));
    idx = (unsigned int)((ctx->count / 8) % 64);
    padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    md5_update(ctx, md5_padding, padlen);
    md5_update(ctx, bits, 8);
    for (i = 0; i < 4; i++) {
        digest[i * 4] = (unsigned char)(ctx->state[i] & 0xff);
        digest[i * 4 + 1] = (unsigned char)((ctx->state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = (unsigned char)((ctx->state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = (unsigned char)((ctx->state[i] >> 24) & 0xff);
    }
}

static void md5(const void *data, size_t len, unsigned char digest[16])
    __attribute__((unused));
static void md5(const void *data, size_t len, unsigned char digest[16])
{
    md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, digest);
}

/* ------------------------------------------------------------------ */
/* XOR pad (RECON: no AES in .dynsym -> the payload crypto used by     */
/* 3.3.9 on this chip set is an XOR pad against the token/key bytes)   */
/* ------------------------------------------------------------------ */
static void xor_pad(unsigned char *data, int len, const unsigned char *key,
                    int keylen)
{
    int i;
    for (i = 0; i < len; i++)
        data[i] ^= key[i % keylen];
}

/* ------------------------------------------------------------------ */
/* OT packet                                                           */
/*                                                                     */
/*   0                   1                   2                   3     */
/*   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1  */
/*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  */
/*  |    magic      |     length    |      unknown   |   device_id   |  */
/*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  */
/*  |         timestamp             |           crypt[16] (MD5)      |  */
/*  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+  */
/*                                                                     */
/*  crypt = MD5(first 16 header bytes + payload); when encdata is on   */
/*  payload is XOR-padded with the 16-byte (binary) token first.       */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
struct ot_header {
    uint16_t magic;
    uint16_t length;
    uint16_t unknown;
    uint32_t device_id;
    uint32_t ts;
    unsigned char crypt[16];
};
#pragma pack(pop)

static struct miio_client {
    int state;
    unsigned long long uid;
    char did[33];
    char key[MAX_KEY_LEN];
    char token[MAX_TOKEN_LEN];
    char mac[32];
    char model[64];
    char vendor[16];
    char country_domain[16];
    char data_dir[256];
    int encdata;                 /* -E payload encryption between app & miio */
    int enckey;                  /* -e keys stored encrypted                 */
    int sync_interval;           /* _otc.info sync interval, ms              */
    char tcp_host[MAX_HOST_LEN];
    char udp_host[MAX_HOST_LEN];
    int tcp_port;                /* OT-agent channel port (default 54321)    */
    int line_port;               /* local line port (default 54322)          */
    int bt_port;                 /* helper "bt" conn port (default 54323)    */
    int cloud_sock;
    int cloud_connected;
    int err_times;               /* consecutive cloud failures, retry count  */
    unsigned int msg_id;
    unsigned int report_id;
    unsigned int last_ack_id;
    unsigned long long ack_timeout;
    int server_noack;
    pthread_mutex_t lock;
} miio;

static const char *state_str[] = {
    "device_init",
    "didkey_req1",
    "didkey_req2",
    "didkey_done",
    "token_done",
    "sta_mode",
    "cloud_trying",
    "cloud_connected",
    "cloud_retry",
};
#define STATE_DEVICE_INIT       0
#define STATE_DIDKEY_REQ1       1
#define STATE_DIDKEY_REQ2       2
#define STATE_DIDKEY_DONE       3
#define STATE_TOKEN_DONE        4
#define STATE_STA_MODE          5
#define STATE_CLOUD_TRYING      6
#define STATE_CLOUD_CONNECTED   7
#define STATE_CLOUD_RETRY       8

static void state_set(int new_state)
{
    miio.state = new_state;
    log_printf(LOG_DEBUG, "STATE: (%d) -> STATE_%s", new_state,
               state_str[new_state]);
}

/* ------------------------------------------------------------------ */
/* file persistence of did/key/token (under datadir)                   */
/* ------------------------------------------------------------------ */
static int data_encrypt(unsigned char *data, int len, int enc,
                        unsigned char *buf, int bufsize)
    __attribute__((unused));
static int data_encrypt(unsigned char *data, int len, int enc,
                        unsigned char *buf, int bufsize)
{
    unsigned char key[16];
    int klen;
    int i;

    if (len >= bufsize)
        return -1;
    memcpy(buf, data, len);
    if (!enc)
        return len;
    /* RECON: stored key/uid file crypto uses the 16-byte XOR pad */
    klen = (int)strlen(XOR_KEY);
    memset(key, 0, sizeof(key));
    memcpy(key, XOR_KEY, klen);
    xor_pad(buf, len, key, 16);
    return len;
}

static int save_string(const char *name, const char *value, int encrypt)
{
    char path[256];
    unsigned char enc[256];
    int len;
    int fd;

    snprintf(path, sizeof(path), "%s%s", miio.data_dir, name);
    len = (int)strlen(value);
    if (len >= (int)sizeof(enc))
        return -1;
    if (encrypt)
        data_encrypt((unsigned char *)value, len, 1, enc, sizeof(enc));
    else
        memcpy(enc, value, len);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    write(fd, enc, (size_t)len);
    close(fd);
    return 0;
}

static int load_string(const char *name, char *out, int outsize, int decrypt)
{
    char path[256];
    unsigned char buf[256];
    int fd;
    int len;

    snprintf(path, sizeof(path), "%s%s", miio.data_dir, name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    len = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
        return -1;
    if (decrypt)
        data_encrypt(buf, len, 0, buf, sizeof(buf));
    buf[len] = '\0';
    if (outsize > len)
        memcpy(out, buf, (size_t)(len + 1));
    else {
        memcpy(out, buf, (size_t)outsize - 1);
        out[outsize - 1] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* client registry + broadcast                                         */
/* ------------------------------------------------------------------ */
struct client {
    int fd;
    int type;                    /* 0=ot agent, 1=line, 2=bt */
    int valid;
};
static struct client clients[MAX_CLIENTS];

static void clients_init(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].valid = 0;
    }
}

static int add_client(int fd, int type)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].valid) {
            clients[i].fd = fd;
            clients[i].type = type;
            clients[i].valid = 1;
            return 0;
        }
    }
    log_printf(LOG_WARNING, "%s, %d: too many clients to track",
               __func__, fd);
    return -1;
}

static void del_client(int fd)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].valid && clients[i].fd == fd) {
            clients[i].valid = 0;
            clients[i].fd = -1;
            return;
        }
    }
}

static void method_local_broadcast_msg(const char *method,
                                       const char *params)
{
    static int seq = 0;          /* app-level broadcast carries no id */
    char buf[256];
    int i;

    seq++;
    snprintf(buf, sizeof(buf), "{\"method\":\"%s\",\"params\":\"%s\"}",
             method, params);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].valid) {
            log_printf(LOG_VERBOSE, "%s, send back to mobile: %s",
                       __func__, buf);
            send(clients[i].fd, buf, strlen(buf), 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* servers (cloud) list                                                */
/* ------------------------------------------------------------------ */
enum { SVR_TYPE_TCP, SVR_TYPE_UDP };

struct server {
    int type;
    char host[MAX_HOST_LEN];
    char ip[64];
    int port;
    int valid;
    int retries;
};
static struct server servers[MAX_SERVERS];

static int add_server(int type, const char *host, const char *ip, int port)
{
    int i;
    for (i = 0; i < MAX_SERVERS; i++) {
        if (!servers[i].valid) {
            servers[i].type = type;
            snprintf(servers[i].host, sizeof(servers[i].host), "%s", host);
            if (ip)
                snprintf(servers[i].ip, sizeof(servers[i].ip), "%s", ip);
            servers[i].port = port;
            servers[i].valid = 1;
            servers[i].retries = 0;
            log_printf(LOG_INFO, "Add %s server into list, host ip: %s, "
                       "port: %d", type == SVR_TYPE_TCP ? "TCP" : "UDP",
                       ip ? ip : host, port);
            return 0;
        }
    }
    return -1;
}

static void dump_server_list(int type)
{
    int i;
    for (i = 0; i < MAX_SERVERS; i++) {
        if (servers[i].valid && (type < 0 || servers[i].type == type)) {
            log_printf(LOG_DEBUG, "%s server, ip: %s, port: %d",
                       servers[i].type == SVR_TYPE_TCP ? "TCP" : "UDP",
                       servers[i].ip[0] ? servers[i].ip
                                        : servers[i].host,
                       servers[i].port);
        }
    }
}

/* ------------------------------------------------------------------ */
/* timer helpers                                                       */
/* ------------------------------------------------------------------ */
static int timer_start(int fd, int ms, int oneshot)
{
    struct itimerspec its;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = ms / 1000;
    its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    if (!oneshot) {
        its.it_interval.tv_sec = ms / 1000;
        its.it_interval.tv_nsec = (long)(ms % 1000) * 1000000L;
    }
    return timerfd_settime(fd, 0, &its, NULL);
}

static int timerfd_create_settime(int ms, int oneshot)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) {
        log_printf(LOG_ERROR, "timerfd_create: %m");
        return -1;
    }
    if (timer_start(fd, ms, oneshot) != 0) {
        log_printf(LOG_ERROR, "timerfd_settime: %m");
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* queued cloud messages with ack/retry                                */
/* ------------------------------------------------------------------ */
enum { LIST_DATA_QUEUE, LIST_DATA_NOACK };

struct data_desc {
    int id;
    int size;
    int retry;
    unsigned long long ack_timeout;
    char *data;
    int list;
    struct data_desc *next;
};
static struct data_desc *queue_head[2];

static void init_list(int which)
{
    queue_head[which] = NULL;
}

static struct data_desc *insert_list(int which, int id, int size, int retry,
                                     char *data)
{
    struct data_desc *d = calloc(1, sizeof(*d));
    struct data_desc *p;

    if (d == NULL) {
        log_printf(LOG_ERROR, "%s: malloc fail", __func__);
        return NULL;
    }
    d->id = id;
    d->size = size;
    d->retry = retry;
    d->ack_timeout = get_micro_second();
    d->data = data;
    d->list = which;
    d->next = NULL;
    if (queue_head[which] == NULL) {
        queue_head[which] = d;
    } else {
        for (p = queue_head[which]; p->next; p = p->next)
            ;
        p->next = d;
    }
    log_printf(LOG_DEBUG, "data desc added(+) id: %d, len: %d, retry: %d",
               id, size, retry);
    return d;
}

static struct data_desc *find_data_desc(int which, int id)
    __attribute__((unused));
static struct data_desc *find_data_desc(int which, int id)
{
    struct data_desc *p;
    for (p = queue_head[which]; p; p = p->next) {
        if (p->id == id)
            return p;
    }
    return NULL;
}

static int delete_data_desc(int which, int id)
{
    struct data_desc **pp = &queue_head[which];

    while (*pp) {
        if ((*pp)->id == id) {
            struct data_desc *tmp = *pp;
            *pp = tmp->next;
            free(tmp->data);
            free(tmp);
            log_printf(LOG_DEBUG, "data desc acked(-) id: %d", id);
            return 0;
        }
        pp = &(*pp)->next;
    }
    log_printf(LOG_WARNING, "not found data desc with id: %d", id);
    return -1;
}

/* ------------------------------------------------------------------ */
/* message senders                                                     */
/* ------------------------------------------------------------------ */
static int general_send_one(int fd, const char *msg)
{
    int len = (int)strlen(msg);
    char id[32] = "0";

    if (fd < 0)
        return -1;
    json_verify_get_string(msg, "id", id, sizeof(id));
    log_printf(LOG_DEBUG, "S, id:%s, len:%d", id, len);
    if (send(fd, msg, (size_t)len, 0) < 0) {
        log_printf(LOG_WARNING, "%s,%s: %d send error: %m",
                   __func__, "fd", fd);
        return -1;
    }
    return 0;
}

static int general_send_one_queued(int fd, const char *msg, int queue)
{
    int len = (int)strlen(msg);
    char *data;
    int retry = 3;

    if (miio.cloud_sock < 0) {
        log_printf(LOG_WARNING,
                   "We're offline now, can't report msg, id: %d",
                   miio.msg_id);
        return -1;
    }
    data = malloc((size_t)len);
    if (data == NULL) {
        log_printf(LOG_ERROR, "malloc fail: %m");
        return -1;
    }
    memcpy(data, msg, (size_t)len);
    if (queue == LIST_DATA_QUEUE) {
        if (send(fd, msg, (size_t)len, 0) >= 0)
            retry = 0;
    }
    insert_list(queue, miio.msg_id++, len, retry, data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* OT packet build / parse                                             */
/* ------------------------------------------------------------------ */
static void ot_dump_packet(const struct ot_header *h)
{
    log_printf(LOG_VERBOSE, "magic: 0x%04x len: %d unknown: %d did: %u "
               "ts: %u", h->magic, h->length, h->unknown, h->device_id,
               h->ts);
}

static int ot_encrypt_packet(unsigned char *out, int outsize,
                             const unsigned char *payload, int len,
                             unsigned int did, const char *token)
{
    struct ot_header *h = (struct ot_header *)out;
    unsigned char digest[16];
    unsigned char tmp[OT_MAX_PAYLOAD];
    unsigned int ts = (unsigned int)time(NULL);

    if (len > OT_MAX_PAYLOAD) {
        log_printf(LOG_ERROR,
                   "msg too long, bigger than OT_MAX_PAYLOAD (%d)",
                   OT_MAX_PAYLOAD);
        return -1;
    }
    if (outsize < OT_HEADER_SIZE + len)
        return -1;
    if (miio.encdata && token && strlen(token) >= 16) {
        unsigned char tk[16];
        str2hex(token, tk, 16);
        memcpy(tmp, payload, (size_t)len);
        xor_pad(tmp, len, tk, 16);
    } else {
        memcpy(tmp, payload, (size_t)len);
    }
    memset(out, 0, (size_t)OT_HEADER_SIZE);
    h->magic = OT_MAGIC;
    h->length = (uint16_t)len;
    h->unknown = 0;
    h->device_id = did;
    h->ts = ts;
    /* RECON: crypt = MD5(first 16 header bytes + payload) */
    {
        md5_ctx ctx;
        md5_init(&ctx);
        md5_update(&ctx, out, (size_t)OT_HEADER_SIZE - 16);
        md5_update(&ctx, tmp, (size_t)len);
        md5_final(&ctx, digest);
    }
    memcpy(h->crypt, digest, 16);
    memcpy(out + OT_HEADER_SIZE, tmp, (size_t)len);
    ot_dump_packet(h);
    return OT_HEADER_SIZE + len;
}

static int ot_decrypt_packet(const unsigned char *in, int inlen,
                             unsigned char *payload, const char *token)
{
    const struct ot_header *h = (const struct ot_header *)in;
    unsigned char digest[16];
    int datalen;

    if (inlen < OT_HEADER_SIZE) {
        log_printf(LOG_ERROR,
                   "OT received (%d), less than size of OT header (32). %m",
                   inlen);
        return -1;
    }
    if (h->magic != OT_MAGIC) {
        log_printf(LOG_ERROR, "OT protocol version not match.");
        return -2;
    }
    datalen = h->length;
    if (inlen < OT_HEADER_SIZE + datalen) {
        log_printf(LOG_ERROR,
                   "OT received (%d), less than size of OT packet (%d).",
                   inlen, OT_HEADER_SIZE + datalen);
        return -3;
    }
    if (h->device_id != 0 && miio.did[0] &&
        h->device_id != (uint32_t)strtoul(miio.did, NULL, 10)) {
        log_printf(LOG_ERROR, "OT device id not match.");
        return -4;
    }
    md5_ctx ctx;
    md5_init(&ctx);
    md5_update(&ctx, in, (size_t)OT_HEADER_SIZE - 16);
    md5_update(&ctx, in + OT_HEADER_SIZE, (size_t)datalen);
    md5_final(&ctx, digest);
    if (memcmp(digest, h->crypt, 16) != 0) {
        log_printf(LOG_ERROR, "OT MD5 not match.");
        return -5;
    }
    memcpy(payload, in + OT_HEADER_SIZE, (size_t)datalen);
    payload[datalen] = '\0';
    if (miio.encdata && token && strlen(token) >= 16) {
        unsigned char tk[16];
        str2hex(token, tk, 16);
        xor_pad(payload, datalen, tk, 16);
    }
    return datalen;
}

/* ------------------------------------------------------------------ */
/* cloud send path (TLS-free plain TCP; OT header frames every msg)    */
/* ------------------------------------------------------------------ */
static int cloud_send_packet(const char *msg)
{
    unsigned char pkt[OT_MAX_PAYLOAD + OT_HEADER_SIZE];
    int plen;
    int rv;

    if (miio.cloud_sock < 0 || !miio.cloud_connected) {
        log_printf(LOG_DEBUG, "cloud socket not ready yet ... ");
        return -1;
    }
    plen = ot_encrypt_packet(pkt, sizeof(pkt),
                             (const unsigned char *)msg,
                             (int)strlen(msg),
                             (unsigned int)strtoul(miio.did, NULL, 10),
                             miio.token);
    if (plen < 0)
        return -1;
    rv = send(miio.cloud_sock, pkt, (size_t)plen, 0);
    if (rv < 0) {
        log_printf(LOG_WARNING, "cloud send error: %m");
        return -1;
    }
    log_printf(LOG_DEBUG, "cloud msg:%s, len :%d", msg, rv);
    return 0;
}

static int cloud_send_queued(const char *msg)
{
    if (miio.cloud_sock < 0)
        return -1;
    return general_send_one_queued(miio.cloud_sock, msg, LIST_DATA_QUEUE);
}

static int otc_send_ack(int sock, int id)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"id\":%d,\"result\":[\"OK\"]}", id);
    log_printf(LOG_DEBUG, "ACK, id:%d, fd:%d, to:%s", id, sock,
               "cloud");
    return send(sock, msg, strlen(msg), 0);
}

/* ------------------------------------------------------------------ */
/* cloud readers / handlers                                            */
/* ------------------------------------------------------------------ */
static int report_otcinfo(void);

static void report_ota_state_idle(void)
{
    char msg[128];

    snprintf(msg, sizeof(msg),
             "{\"id\":%u,\"method\":\"props\",\"params\":"
             "{\"ota_state\":\"idle\"}}", ++miio.report_id);
    cloud_send_queued(msg);
}

static void report_bind_key(void)
{
    char msg[160];
    char bind_key[64];

    if (load_string("key", bind_key, sizeof(bind_key), miio.enckey) != 0) {
        log_printf(LOG_DEBUG, "no bind_key to report");
        return;
    }
    snprintf(msg, sizeof(msg),
             "{\"id\":%u,\"method\":\"props\",\"params\":"
             "{\"bind_key\":\"%s\"}}", ++miio.report_id, bind_key);
    cloud_send_queued(msg);
}

static void handle_miio_methods(const char *msg)
{
    char id[32];
    char key[64];

    if (json_verify_method_value(msg, "miIO.config_router", "ssid",
                                 "on") == 0) {
        log_printf(LOG_INFO, "Got miIO.config_router.");
    } else if (json_verify_method(msg, "miIO.wifi_assoc_state") == 0) {
        log_printf(LOG_INFO, "Got miIO.wifi_assoc_state.");
    } else if (json_verify_method(msg, "miIO.stop_diag_mode") == 0) {
        log_printf(LOG_INFO, "Got miIO.stop_diag_mode.");
    } else if (json_verify_method_value(msg, "miIO.config", "enauth",
                                        "1") == 0) {
        log_printf(LOG_INFO, "Got miIO.config, enauth: %d", 1);
        miio.encdata = 1;
    } else if (json_verify_method(msg, "miIO.info") == 0) {
        log_printf(LOG_INFO, "Got miIO.info.");
    } else if (json_verify_method(msg, "miIO.config_tz") == 0) {
        log_printf(LOG_INFO, "set miIO.config_tz.");
    } else if (json_verify_method(msg, "miIO.config_loglevel") == 0) {
        log_printf(LOG_INFO, "set miIO.config_loglevel.");
        if (json_verify_get_int(msg, "params", &loglevel) != 0)
            log_printf(LOG_WARNING, "miIO.config_loglevel fail");
    } else if (json_verify_method(msg, "miIO.bind_stat") == 0) {
        log_printf(LOG_INFO, "(cloud) miIO.bind_stat");
        if (json_verify_get_string(msg, "bind_key", key, sizeof(key)) == 0)
            save_string("bind_stat", key, miio.enckey);
    } else {
        log_printf(LOG_WARNING, "Unknown msg: %s", msg);
        return;
    }
    if (json_verify_get_string(msg, "id", id, sizeof(id)) == 0) {
        otc_send_ack(miio.cloud_sock, atoi(id));
    } else {
        log_printf(LOG_WARNING, "In %s, can't get 'id' from msg: %s",
                   __func__, msg);
    }
}

static void cloud_recv_time_sync(const char *msg)
{
    int server_time = 0;
    unsigned int local_mono = 0;
    int diff = 0;

    json_verify_get_int(msg, "timestamp", &server_time);
    local_mono = (unsigned int)(get_micro_second() / 1000000ULL);
    diff = server_time - (int)local_mono;
    log_printf(LOG_INFO,
               "sync time with server, server: %d, local(mono): %u, "
               "diff: %d", server_time, local_mono, diff);
}

static void cloud_recv_one(const unsigned char *pkt, int len)
{
    unsigned char payload[OT_MAX_PAYLOAD + 1];
    int plen;
    int id;
    char *dup;

    log_printf(LOG_DEBUG, "OT cloud data come: %d bytes", len);
    plen = ot_decrypt_packet(pkt, len, payload, miio.token);
    if (plen < 0)
        return;
    dup = strdup((char *)payload);
    if (dup == NULL)
        return;
    log_printf(LOG_DEBUG, "cloud msg:%s, len :%d", dup, plen);

    if (json_verify_get_int(dup, "id", &id) != 0) {
        log_printf(LOG_WARNING, "get id fail");
        free(dup);
        return;
    }
    if (id != 0 && (unsigned)id == miio.last_ack_id) {
        log_printf(LOG_DEBUG, "look like a repeated msg:%d, %d",
                   id, id);
        free(dup);
        return;
    }
    miio.last_ack_id = (unsigned)id;
    if (id != 0)
        otc_send_ack(miio.cloud_sock, id);

    if (strstr(dup, "props")) {
        if (json_verify_method_value(dup, "props", "ota_state",
                                     "idle") == 0) {
            log_printf(LOG_DEBUG, "report_ota_state_idle ack ok");
        } else if (strstr(dup, "bind_key")) {
            log_printf(LOG_DEBUG, "report_bind_key ack ok");
        }
        free(dup);
        return;
    }
    if (json_verify_method(dup, "miIO.config") == 0 ||
        json_verify_method(dup, "miIO.config_router") == 0 ||
        json_verify_method(dup, "miIO.wifi_assoc_state") == 0 ||
        json_verify_method(dup, "miIO.stop_diag_mode") == 0 ||
        json_verify_method(dup, "miIO.info") == 0 ||
        json_verify_method(dup, "miIO.config_tz") == 0 ||
        json_verify_method(dup, "miIO.config_loglevel") == 0 ||
        json_verify_method(dup, "miIO.bind_stat") == 0) {
        report_msg_general_callback(dup);
    } else if (strstr(dup, "\"otc_list\"")) {
        char *resp = dup;
        struct server *s;
        int i;
        log_printf(LOG_DEBUG, "_otc.info response: %s", resp);
        s = servers;
        for (i = 0; i < MAX_SERVERS; i++) {
            if (servers[i].valid)
                servers[i].valid = 0;
        }
        /* RECON: otc_list == ["ip:port", ...] strings are also legal;
           here handle the {ip,port} object form via token scan. */
        {
            const char *p = strstr(resp, "ip");
            int n = 0;
            while (p && n < MAX_SERVERS) {
                p = strchr(p, ':');
                if (!p)
                    break;
                p++;
                if (*p == '"') {
                    char ip[64];
                    int j;
                    for (j = 0; p[j + 1] && p[j + 1] != '"' &&
                         j < (int)sizeof(ip) - 1; j++)
                        ip[j] = p[j + 1];
                    ip[j] = '\0';
                    p += j + 1;
                    add_server(SVR_TYPE_TCP, DEFAULT_TCP_HOST, ip, 8053);
                    n++;
                }
                p = strchr(p, ',');
                if (p)
                    p++;
                else
                    break;
            }
        }
        free(dup);
        return;
    } else if (json_verify_method(dup, "_otc.info") == 0) {
        log_printf(LOG_DEBUG, "_otc.info json does not have 'id'");
    } else if (strstr(dup, "Ping") || strstr(dup, "ping")) {
        log_printf(LOG_DEBUG, "Ping - Pong msg from server");
    } else if (json_verify_method(dup, "local.status") == 0) {
        log_printf(LOG_DEBUG, "Time sync msg from server");
        cloud_recv_time_sync(dup);
    } else {
        log_printf(LOG_WARNING, "Unknown msg: %s", dup);
    }
    free(dup);
}

static int report_otcinfo(void)
{
    static unsigned int flag;
    char buf[640];
    int i;

    flag++;
    snprintf(buf, sizeof(buf),
             "{\"id\":%u,\"method\":\"_otc.info\",\"params\":{"
             "\"life\":\"%d\",\"uid\":\"%llu\",\"timestamp\":%d,"
             "\"proto\":\"wifi\",\"sendcnt\":%u,\"ackcnt\":%u,",
             ++miio.report_id, get_uptime(), miio.uid,
             (int)time(NULL), miio.msg_id, miio.last_ack_id);
    {
        char tmp[64];
        int n = (int)strlen(buf);
        snprintf(tmp, sizeof(tmp), "\"otstat\":%d}", miio.state);
        if (n + (int)strlen(tmp) < (int)sizeof(buf))
            strcat(buf, tmp);
    }
    log_printf(LOG_DEBUG, "report to cloud _otc.info: %s", buf);
    cloud_send_queued(buf);
    for (i = 0; i < MAX_SERVERS; i++) {
        if (servers[i].valid)
            log_printf(LOG_DEBUG, "Current server type %d, ip: %s, "
                       "port: %d", servers[i].type,
                       servers[i].ip[0] ? servers[i].ip : servers[i].host,
                       servers[i].port);
    }
    return 0;
}

static void *cloud_recv_thread(void *unused)
{
    unsigned char pkt[OT_MAX_PAYLOAD + OT_HEADER_SIZE];
    ssize_t n;

    (void)unused;
    while (miio.cloud_sock >= 0) {
        struct pollfd pfd;
        int rv;
        pfd.fd = miio.cloud_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rv = poll(&pfd, 1, 1000);
        if (rv <= 0) {
            if (rv < 0 && errno != EINTR)
                break;
            continue;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            log_printf(LOG_WARNING,
                       "OT server: %s, port: %d not response, close "
                       "(socket %d)", DEFAULT_TCP_HOST, 8053,
                       miio.cloud_sock);
            break;
        }
        n = recv(miio.cloud_sock, pkt, sizeof(pkt), 0);
        if (n <= 0)
            break;
        cloud_recv_one(pkt, (int)n);
    }
    log_printf(LOG_WARNING, "cloud socket closed");
    close(miio.cloud_sock);
    miio.cloud_sock = -1;
    miio.cloud_connected = 0;
}

static void cloud_send_thread(void *unused)
{
    unsigned char pkt[OT_MAX_PAYLOAD + OT_HEADER_SIZE];
    struct data_desc *d;

    (void)unused;
    init_list(LIST_DATA_QUEUE);
    init_list(LIST_DATA_NOACK);
    for (;;) {
        usleep(100000);
        if (miio.cloud_sock < 0 || !miio.cloud_connected)
            continue;
        pthread_mutex_lock(&miio.lock);
        d = queue_head[LIST_DATA_QUEUE];
        if (d) {
            if (d->retry == 0) {
                queue_head[LIST_DATA_QUEUE] = d->next;
                d->next = NULL;
                insert_list(LIST_DATA_NOACK, d->id, d->size, 3, d->data);
                free(d);
            } else {
                int plen;
                plen = ot_encrypt_packet(pkt, sizeof(pkt),
                                         (const unsigned char *)d->data,
                                         d->size,
                                         (unsigned int)strtoul(miio.did,
                                                               NULL, 10),
                                         miio.token);
                if (plen > 0) {
                    log_printf(LOG_DEBUG,
                               "%s:%d, to send size: %d", __func__,
                               d->id, plen);
                    send(miio.cloud_sock, pkt, (size_t)plen, 0);
                }
                d->retry = 0;
            }
        }
        pthread_mutex_unlock(&miio.lock);
    }
    return;
}

static void ack_timer_thread(void *unused)
{
    int fd;

    (void)unused;
    fd = timerfd_create_settime(30000, 0);    /* timeout 30s */
    while (fd >= 0) {
        unsigned long long exp;
        ssize_t n;
        struct pollfd pfd;
        int rv;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rv = poll(&pfd, 1, 40000);
        if (rv <= 0)
            continue;
        n = read(fd, &exp, sizeof(exp));
        if (n != sizeof(exp))
            continue;
        pthread_mutex_lock(&miio.lock);
        {
            struct data_desc *d = queue_head[LIST_DATA_NOACK];
            while (d && d->ack_timeout < get_micro_second()) {
                if (d->retry > 0) {
                    unsigned char pkt[OT_MAX_PAYLOAD + OT_HEADER_SIZE];
                    int plen;
                    plen = ot_encrypt_packet(pkt, sizeof(pkt),
                                             (const unsigned char *)d->data,
                                             d->size,
                                             (unsigned int)strtoul(
                                                 miio.did, NULL, 10),
                                             miio.token);
                    if (plen > 0)
                        send(miio.cloud_sock, pkt, (size_t)plen, 0);
                    d->retry--;
                    d->ack_timeout = get_micro_second() + 30000000ULL;
                    log_printf(LOG_DEBUG,
                               "%s:%d, ack_timeout: %llu, curr_time: "
                               "%llu, retry to send size: %d",
                               __func__, d->id, d->ack_timeout,
                               get_micro_second(), plen);
                } else {
                    struct data_desc *tmp = d;
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "{\"id\":%d,\"error\":{\"code\":-30011,"
                             "\"message\":\"OT server noack\"}}",
                             d->id);
                    general_send_one(miio.cloud_sock, msg);
                    delete_data_desc(LIST_DATA_NOACK, d->id);
                    d = queue_head[LIST_DATA_NOACK];
                    free(tmp);
                    continue;
                }
                d = d->next;
            }
        }
        pthread_mutex_unlock(&miio.lock);
    }
    return;
}

static void sync_timer_thread(void *unused)
{
    int fd;

    (void)unused;
    if (miio.sync_interval <= 0)
        miio.sync_interval = DEFAULT_SYNC_INTERVAL;
    fd = timerfd_create_settime(miio.sync_interval, 0);
    while (fd >= 0) {
        unsigned long long exp;
        struct pollfd pfd;
        int rv;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rv = poll(&pfd, 1, miio.sync_interval + 5000);
        if (rv <= 0)
            continue;
        if (read(fd, &exp, sizeof(exp)) != sizeof(exp))
            continue;
        if (miio.cloud_connected && miio.token[0])
            report_otcinfo();
    }
    return;
}

static void report_keepalive_thread(void *unused)
{
    (void)unused;
    for (;;) {
        sleep(30);
        if (miio.cloud_connected && miio.cloud_sock >= 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"id\":%u,\"method\":\"_otc.info"
                     "_keepalive\",\"params\":{\"ping\":true}}",
                     ++miio.report_id);
            cloud_send_queued(msg);
        }
    }
    return;
}

/* ------------------------------------------------------------------ */
/* cloud server connection (retry loop)                                */
/* ------------------------------------------------------------------ */
static void server_conn_retry_thread(void *unused)
{
    char hosts[MAX_SERVERS][MAX_HOST_LEN];
    int hosts_n = 0;
    int tried = 0;
    int idx = 0;
    int i;

    (void)unused;
    if (miio.udp_host[0]) {
        struct addrinfo hints, *res = NULL;
        char port[16];
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port, sizeof(port), "%d", 8053);
        if (getaddrinfo(miio.udp_host, port, &hints, &res) == 0) {
            struct addrinfo *r;
            for (r = res; r && hosts_n < MAX_SERVERS; r = r->ai_next) {
                char ip[64];
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)r->ai_addr)->sin_addr,
                          ip, sizeof(ip));
                snprintf(hosts[hosts_n++], sizeof(hosts[0]), "%s", ip);
            }
            freeaddrinfo(res);
        }
    }
    if (hosts_n == 0)
        snprintf(hosts[hosts_n++], sizeof(hosts[0]), "%s",
                 miio.udp_host);
    dump_server_list(SVR_TYPE_TCP);

    while (miio.udp_host[0]) {
        if (miio.cloud_sock >= 0) {
            sleep(1);
            continue;
        }
        if (tried >= 3) {
            state_set(STATE_CLOUD_RETRY);
            method_local_broadcast_msg("local.status", "cloud_retry");
            log_printf(LOG_INFO, "Will retry in next: %d ms", 15000);
            sleep(15);
            tried = 0;
            state_set(STATE_CLOUD_TRYING);
            continue;
        }
        log_printf(LOG_INFO, "Connect to server ip: %s, port: %d",
                   hosts[idx], 8053);
        {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in sa;

            if (s < 0) {
                log_printf(LOG_ERROR, "error in create socket: %m");
                tried++;
                idx = (idx + 1) % hosts_n;
                continue;
            }
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(8053);
            inet_pton(AF_INET, hosts[idx], &sa.sin_addr);
            if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
                log_printf(LOG_ERROR, "error in trying to connect "
                           "socket: %m");
                close(s);
                tried++;
                idx = (idx + 1) % hosts_n;
                continue;
            }
            miio.cloud_sock = s;
            miio.cloud_connected = 1;
            tried = 0;
            state_set(STATE_CLOUD_CONNECTED);
            method_local_broadcast_msg("local.status", "cloud_connected");
            log_printf(LOG_INFO, "OT cloud sockfd: %d", s);
            {
                pthread_t t;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr,
                                            PTHREAD_CREATE_DETACHED);
pthread_create(&t, &attr, (void *(*)(void *))cloud_recv_thread,
                           NULL);
                pthread_attr_destroy(&attr);
            }
            report_ota_state_idle();
        }
        sleep(1);
    }
    return;
}

/* ------------------------------------------------------------------ */
/* local (phone / line / bt helper) message dispatch                   */
/* ------------------------------------------------------------------ */
static void respond_error(int fd, int id, int code, const char *message)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id, code, message);
    general_send_one(fd, buf);
}

static void respond_result(int fd, int id, const char *result)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"result\":[\"%s\"]}",
             id, result);
    general_send_one(fd, buf);
}

static void handle_local_msg_state(int fd, int id)
{
    char buf[128];
    const char *st = state_str[miio.state];

    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"method\":\"local.status\",\"params\":\"%s\"}",
             id, st);
    general_send_one(fd, buf);
}

static void handle_local_msg_time(int fd, int id)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"method\":\"local.time\",\"params\":%d}",
             id, (int)time(NULL));
    general_send_one(fd, buf);
}

static void handle_local_msg_country(int fd, int id)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"method\":\"local.country\",\"params\":\"%s\"}",
             id, miio.country_domain);
    general_send_one(fd, buf);
}

static void handle_local_msg_suspend(int fd, int id, const char *msg)
{
    char buf[192];
    int wakeup = 5;

    json_verify_get_int(msg, "wakeup_time", &wakeup);
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"method\":\"local.resp_suspend\",\"params\":"
             "{\"wakeup_time\":%d}}", id, wakeup);
    general_send_one(fd, buf);
}

static void handle_local_ble_config_router(int fd, int id, const char *msg)
{
    char ssid[64] = "";
    char passwd[64] = "";
    char p[256];

    log_printf(LOG_INFO, "ble config router fail");
    json_verify_get_string(msg, "ssid", ssid, sizeof(ssid));
    json_verify_get_string(msg, "passwd", passwd, sizeof(passwd));
    log_printf(LOG_DEBUG, "ssid=%s", ssid);
    snprintf(p, sizeof(p), "ssid=%s,passwd=%s", ssid, passwd);
    (void)p;
    respond_error(fd, id, -33020, "ble config_router fail.");
}

static void cmd_internal_request_dinfo(int fd)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.request_dinfo\",\"params\":\"%llu\"}",
             miio.uid);
    general_send_one(fd, buf);
    state_set(STATE_DEVICE_INIT);
    log_printf(LOG_DEBUG, "Please start helper script.");
}

static void cmd_internal_request_didkeymac(int fd, int uid)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.req_didkeymac\",\"params\":\"%d\"}",
             uid);
    general_send_one(fd, buf);
}

static void cmd_internal_request_didkeymac2(int fd, int uid)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.req_didkeymac2\",\"params\":\"%d\"}",
             uid);
    general_send_one(fd, buf);
}

static void cmd_internal_request_dtoken(int fd)
{
    char buf[320];
    char ntoken[128];

    generate_random_ntoken(ntoken, sizeof(ntoken));
    if (miio.data_dir[0] == '\0')
        snprintf(miio.data_dir, sizeof(miio.data_dir), "%s", DEFAULT_DIR);
    if (strlen(miio.data_dir) + strlen(ntoken) >= sizeof(buf)) {
        log_printf(LOG_WARNING, "%s: dir + token too long: %s",
                   __func__, miio.data_dir);
        return;
    }
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.request_dtoken\",\"params\":"
             "{\"dir\":\"%s\",\"ntoken\":\"%s\"}",
             miio.data_dir, ntoken);
    general_send_one(fd, buf);
    state_set(STATE_DIDKEY_REQ2);
}

static void cmd_internal_req_wifi_conf_status(int fd)
{
    char buf[160];
    char dbg[64];

    snprintf(dbg, sizeof(dbg), "uid=%llu", miio.uid);
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.req_wifi_conf_status\",\"params\":"
             "\"%s\"}", dbg);
    general_send_one(fd, buf);
}

static void cmd_internal_wifi_start(const char *params)
{
    char buf[512];
    char ssid[64] = "";
    char passwd[64] = "";
    char country[16] = "";
    char tz[16] = "";
    char uid_s[32] = "";
    int uid_i = 0;

    json_verify_get_string(params, "ssid", ssid, sizeof(ssid));
    json_verify_get_string(params, "passwd", passwd, sizeof(passwd));
    json_verify_get_string(params, "country_domain", country,
                           sizeof(country));
    json_verify_get_string(params, "tz", tz, sizeof(tz));
    if (json_verify_get_string(params, "uid", uid_s, sizeof(uid_s)) != 0)
        json_verify_get_int(params, "uid", &uid_i);
    miio.uid = uid_s[0] ? strtoull(uid_s, NULL, 10)
                        : (unsigned long long)uid_i;
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.wifi_start\",\"params\":{"
             "\"datadir\":\"%s\",\"ssid\":\"%s\",\"passwd\":\"%s\","
             "\"uid\":\"%llu\",\"country_domain\":\"%s\",\"tz\":\"%s\"}}",
             miio.data_dir, ssid, passwd, miio.uid, country, tz);
    log_printf(LOG_INFO, "%s", buf);
}

static void cmd_internal_response_didkeymac(int fd, const char *msg)
{
    char params[256];
    int code = -1;
    int status = -1;

    log_printf(LOG_INFO, "Got _internal.res_didkeymac.");
    if (json_verify_get_string(msg, "params", params, sizeof(params)) != 0)
        log_printf(LOG_WARNING,
                   "_internal.res_didkeymac does not have \"params\".");
    json_verify_get_int(msg, "code", &code);
    log_printf(LOG_DEBUG, "%s:%d, code: %d", __func__, fd, code);
    if (code == 0) {
        json_verify_get_int(msg, "status", &status);
        if (status == 0)
            state_set(STATE_DIDKEY_REQ1);
        else
            state_set(STATE_DEVICE_INIT);
    } else {
        cmd_internal_request_didkeymac2(fd, (int)miio.uid);
    }
}

static void cmd_internal_response_didkeymac2(int fd, const char *msg)
{
    char params[256];
    int status = -1;

    log_printf(LOG_INFO, "Got _internal.res_didkeymac2.");
    if (json_verify_get_string(msg, "params", params, sizeof(params)) != 0)
        log_printf(LOG_WARNING,
                   "_internal.res_didkeymac2 does not have \"params\".");
    json_verify_get_int(msg, "status", &status);
    log_printf(LOG_DEBUG, "%s:%d, status: %d", __func__, fd, status);
    state_set(STATE_DEVICE_INIT);
}

static void cmd_internal_response_dinfo(int fd, const char *msg)
{
    char did[64] = "";
    char key[64] = "";
    char mac[64] = "";
    char vendor[64] = "";
    char model[64] = "";

    log_printf(LOG_INFO, "Got _internal.response_dinfo.");
    json_verify_get_string(msg, "did", did, sizeof(did));
    if (did[0] == '\0') {
        log_printf(LOG_WARNING, "no params in response_dinfo: %s", msg);
        return;
    }
    json_verify_get_string(msg, "key", key, sizeof(key));
    json_verify_get_string(msg, "mac", mac, sizeof(mac));
    json_verify_get_string(msg, "vendor", vendor, sizeof(vendor));
    json_verify_get_string(msg, "model", model, sizeof(model));
    if (strlen(key) > 32 || strlen(key) < 16) {
        log_printf(LOG_WARNING, "%s:%d, key too long: %s", __func__, fd,
                   key);
        return;
    }
    if (strlen(vendor) > 12 || strlen(model) > 48 ||
        strlen(mac) > 24) {
        log_printf(LOG_WARNING, "%s:%d, attribute too long", __func__,
                   fd);
        return;
    }
    snprintf(miio.did, sizeof(miio.did), "%s", did);
    snprintf(miio.key, sizeof(miio.key), "%s", key);
    snprintf(miio.mac, sizeof(miio.mac), "%s", mac);
    snprintf(miio.vendor, sizeof(miio.vendor), "%s", vendor);
    snprintf(miio.model, sizeof(miio.model), "%s", model);
    save_string("did", miio.did, miio.enckey);
    save_string("key", miio.key, miio.enckey);
    save_string("mac", miio.mac, miio.enckey);
    log_printf(LOG_DEBUG, "did=%s mac=%s model=%s", miio.did, miio.mac,
               miio.model);
    state_set(STATE_DIDKEY_DONE);
    cmd_internal_request_dtoken(fd);
}

static void cmd_internal_response_dtoken(int fd, const char *msg)
{
    char token[64] = "";

    log_printf(LOG_INFO, "Got _internal.response_dtoken.");
    if (json_verify_get_string(msg, "token", token, sizeof(token)) != 0) {
        log_printf(LOG_WARNING, "no params in response_dtoken: %s", msg);
        return;
    }
    if (strlen(token) > MAX_TOKEN_LEN - 1 || strlen(token) < 16) {
        log_printf(LOG_WARNING, "token size (%d) too long, > %d",
                   (int)strlen(token), MAX_TOKEN_LEN - 1);
        return;
    }
    snprintf(miio.token, sizeof(miio.token), "%s", token);
    save_string("token", miio.token, miio.enckey);
    state_set(STATE_TOKEN_DONE);
    cmd_internal_request_dcountry(fd);
}

static void cmd_internal_request_dcountry(int fd)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"_internal.req_dcountry\",\"params\":\"%llu\"}",
             miio.uid);
    general_send_one(fd, buf);
}

static void cmd_internal_response_dcountry(int fd, const char *msg)
{
    char country[64] = "";

    log_printf(LOG_INFO, "Got _internal.response_dcountry.");
    if (json_verify_get_string(msg, "country", country,
                               sizeof(country)) != 0) {
        log_printf(LOG_WARNING, "no params in response_country: %s", msg);
        return;
    }
    if (strlen(country) > (size_t)sizeof(miio.country_domain) - 1) {
        log_printf(LOG_WARNING, "country size (%d) too long, > %d",
                   (int)strlen(country), (int)sizeof(miio.country_domain));
        return;
    }
    snprintf(miio.country_domain, sizeof(miio.country_domain), "%s",
             country);
    /* compose "<country>.ot.io.mi.com" / "<country>.ott.io.mi.com" */
    if (strcmp(country, "cn") != 0 && strcmp(country, "") != 0) {
        char h[64];
        snprintf(h, sizeof(h), "%s.%s", country, DEFAULT_TCP_HOST);
        snprintf(miio.tcp_host, sizeof(miio.tcp_host), "%s", h);
        snprintf(h, sizeof(h), "%s.%s", country, DEFAULT_UDP_HOST);
        snprintf(miio.udp_host, sizeof(miio.udp_host), "%s", h);
        log_printf(LOG_INFO, "Add country_domain to tcp_host: %s",
                   miio.tcp_host);
    } else {
        snprintf(miio.tcp_host, sizeof(miio.tcp_host), "%s",
                 DEFAULT_TCP_HOST);
        snprintf(miio.udp_host, sizeof(miio.udp_host), "%s",
                 DEFAULT_UDP_HOST);
    }
    state_set(STATE_STA_MODE);
    method_local_broadcast_msg("local.status", "wifi_connected");
    state_set(STATE_CLOUD_TRYING);
    log_printf(LOG_INFO, "internet connection succeed");
    cmd_internal_req_wifi_conf_status(fd);
}

static void handle_ble_config_router(int fd, const char *msg)
{
    handle_local_ble_config_router(fd, 0, msg);
}

static void mobile_msg_callback(int fd, const char *msg)
{
    char id[32] = "";

    log_printf(LOG_DEBUG, "fd: %d, local msg: %.*s, length: %d bytes",
               fd, 64, msg, (int)strlen(msg));
    if (json_verify(msg) != 0) {
        respond_error(fd, 0, -30011, "json verify fail");
        return;
    }
    json_verify_get_string(msg, "id", id, sizeof(id));

    if (json_verify_method(msg, "_internal.debug") == 0) {
        char buf[256];
        log_printf(LOG_INFO, "Got _internal.debug");
        snprintf(buf, sizeof(buf),
                 "{\"result\":\"debug: state: %d, count_pollfds: %d, "
                 "host: %s, did: %llu\"}",
                 miio.state, 0, miio.udp_host, miio.uid);
        general_send_one(fd, buf);
    } else if (json_verify_method(msg, "_internal.info") == 0) {
        char buf[320];
        log_printf(LOG_INFO, "Got _internal.info.");
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"result\":{\"netif\":\"wlan0\",\"state\":"
                 "\"%s\",\"did\":\"%llu\",\"mac\":\"%s\"}}",
                 atoi(id), state_str[miio.state], miio.uid, miio.mac);
        general_send_one(fd, buf);
    } else if (json_verify_method(msg, "_internal.hello") == 0) {
        log_printf(LOG_INFO, "Got _internal.hello.");
        respond_result(fd, atoi(id), "hello");
    } else if (json_verify_method(msg, "_internal.reboot") == 0) {
        log_printf(LOG_INFO, "Got reboot msg.");
        method_local_broadcast_msg("local.status", "reboot");
        sleep(1);
        system("reboot");
    } else if (json_verify_method(msg, "_internal.wifi_connected") == 0) {
        log_printf(LOG_INFO, "WiFi connected");
        method_local_broadcast_msg("local.status", "wifi_connected");
    } else if (json_verify_method(msg, "_internal.wifi_ap_mode") == 0) {
        log_printf(LOG_INFO,
                   "Failed connecting to AP, back to wifi AP mode");
        method_local_broadcast_msg("local.status", "wifi_ap_mode");
    } else if (json_verify_method(msg, "_internal.helper_ready") == 0) {
        log_printf(LOG_INFO, "Got _internal.helper_ready.");
        if (miio.did[0] && miio.key[0])
            state_set(STATE_DIDKEY_DONE);
        else
            cmd_internal_request_dinfo(fd);
    } else if (json_verify_method(msg,
                                  "_internal.res_wifi_conf_status") == 0) {
        log_printf(LOG_INFO, "Got _internal.res_wifi_conf_status.");
        state_set(STATE_WIFI_STA_MODE);
        method_local_broadcast_msg("local.status", "internet_connected");
    } else if (json_verify_method(msg, "_internal.res_didkeymac") == 0) {
        cmd_internal_response_didkeymac(fd, msg);
    } else if (json_verify_method(msg, "_internal.res_didkeymac2") == 0) {
        cmd_internal_response_didkeymac2(fd, msg);
    } else if (json_verify_method(msg, "_internal.response_dinfo") == 0) {
        cmd_internal_response_dinfo(fd, msg);
    } else if (json_verify_method(msg, "_internal.response_dtoken") == 0) {
        cmd_internal_response_dtoken(fd, msg);
    } else if (json_verify_method(msg, "_internal.response_dcountry") == 0) {
        cmd_internal_response_dcountry(fd, msg);
    } else if (json_verify_method(msg, "local.prep_suspend") == 0) {
        handle_local_msg_suspend(fd, atoi(id), msg);
    } else if (json_verify_method(msg, "local.query_status") == 0) {
        handle_local_msg_state(fd, atoi(id));
    } else if (json_verify_method(msg, "local.query_time") == 0) {
        handle_local_msg_time(fd, atoi(id));
    } else if (json_verify_method(msg, "local.query_country") == 0) {
        handle_local_msg_country(fd, atoi(id));
    } else if (json_verify_method(msg, "local.ble.config_router") == 0) {
        handle_ble_config_router(fd, msg);
    } else if (json_verify_method(msg, "miIO.config_router") == 0) {
        handle_miio_methods(msg);
    } else {
        log_printf(LOG_WARNING, "Unknown local method: %s", msg);
        respond_error(fd, atoi(id), -30002, "unknown method");
    }
}

/* forward used by report_path below */
static int report_msg_general_callback(const char *msg)
{
    mobile_msg_callback(miio.cloud_sock, msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* local TCP listeners                                                 */
/* ------------------------------------------------------------------ */
static int create_listener(int port, const char *what)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in sa;

    if (s < 0) {
        log_printf(LOG_ERROR, "Create %s socket error: %s", what,
                   strerror(errno));
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_printf(LOG_ERROR, "Socket bind port (%d) error: %s", port,
                   strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 5) != 0) {
        log_printf(LOG_ERROR, "listen");
        close(s);
        return -1;
    }
    return s;
}

static void line_read_thread(void *arg)
{
    long argfd = (long)arg;
    int fd = (int)argfd;
    char buf[4096];
    ssize_t n;

    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        mobile_msg_callback(fd, buf);
    }
    log_printf(LOG_DEBUG, "OT agent internalfd closed: %d", fd);
    close(fd);
    del_client(fd);
    return;
}

static void ot_agent_recv_handler_one(int fd)
{
    unsigned char hdr[2];
    unsigned char *buf = NULL;
    int total = 0;
    int need = -1;

    if (recv(fd, hdr, 2, MSG_WAITALL) != 2)
        goto out;
    need = (hdr[0] << 8) | hdr[1];
    if (need < OT_HEADER_SIZE || need > OT_MAX_PAYLOAD + OT_HEADER_SIZE)
        goto out;
    buf = malloc((size_t)need);
    if (buf == NULL)
        goto out;
    while (total < need) {
        n = recv(fd, buf + total, (size_t)(need - total), 0);
        if (n <= 0)
            break;
        total += (int)n;
    }
    if (total == need && miio.did[0]) {
        unsigned char payload[OT_MAX_PAYLOAD + 1];
        int plen;
        plen = ot_decrypt_packet(buf, need, payload, miio.token);
        if (plen > 0)
            mobile_msg_callback(fd, (char *)payload);
    }
out:
    free(buf);
    return;
}

static void ot_agent_read_thread(void *arg)
{
    long argfd = (long)arg;
    ot_agent_recv_handler_one((int)argfd);
    close((int)argfd);
    del_client((int)argfd);
    return;
}

static void *ot_agent_listen_thread(void *unused)
{
    int lfd;

    (void)unused;
    lfd = create_listener(miio.tcp_port, "ot server");
    if (lfd < 0) {
        log_printf(LOG_ERROR,
                   "ot_agent_listenfd fail, maybe another miio_client is "
                   "already running.");
        return NULL;
    }
    log_printf(LOG_INFO, "OT agent listen fd: %d", lfd);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        pthread_t t;
        if (c < 0) {
            log_printf(LOG_WARNING, "%s, %d: accept error, listenfd: %d.",
                       __func__, errno, lfd);
            continue;
        }
        log_printf(LOG_DEBUG, "OT agent listen accept sockfd: %d", c);
        if (add_client(c, 0) != 0) {
            close(c);
            continue;
        }
        pthread_create(&t, NULL, (void *(*)(void *))ot_agent_read_thread,
                       (void *)(long)c);
        pthread_detach(t);
    }
    return NULL;
}

static void *line_listen_thread(void *unused)
{
    int lfd;

    (void)unused;
    lfd = create_listener(miio.line_port, "line");
    if (lfd < 0)
        return NULL;
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        pthread_t t;
        if (c < 0)
            continue;
        if (add_client(c, 1) != 0) {
            close(c);
            continue;
        }
        pthread_create(&t, NULL, (void *(*)(void *))line_read_thread,
                       (void *)(long)c);
        pthread_detach(t);
    }
    return NULL;
}

static void *bt_listen_thread(void *unused)
{
    static int bt_client = -1;
    int lfd;

    (void)unused;
    lfd = create_listener(miio.bt_port, "bt server");
    if (lfd < 0) {
        log_printf(LOG_ERROR, "bt_conn_listenfd fail, maybe another "
                   "miio_client is already running.");
        return NULL;
    }
    log_printf(LOG_INFO, "BT conn listen fd: %d", lfd);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        char buf[4096];
        ssize_t n;

        if (c < 0)
            continue;
        if (bt_client >= 0) {
            log_printf(LOG_WARNING,
                       "%s(): already have one bt client, ignore...",
                       __func__);
            close(c);
            continue;
        }
        bt_client = c;
        log_printf(LOG_DEBUG, "BT conn listen accept sockfd: %d", c);
        while ((n = recv(c, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            mobile_msg_callback(c, buf);
        }
        log_printf(LOG_DEBUG, "BT conn fd closed: %d", c);
        close(c);
        bt_client = -1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* UDP smart-conn datagram channel                                     */
/* ------------------------------------------------------------------ */
static void smart_conn_recv_handler_datagram(int fd)
{
    char buf[2048];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n;

    n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                 (struct sockaddr *)&from, &flen);
    if (n <= 0)
        return;
    buf[n] = '\0';
    log_printf(LOG_DEBUG, "%s, %s", __func__, buf);
    /* RECON: smart-config payloads are mini JSON blobs that seed the
       wifi start; a fully-specified SSID key dispatches _internal.wifi_start */
    if (strstr(buf, "ssid"))
        cmd_internal_wifi_start(buf);
}

static void *smart_conn_thread(void *unused)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    int one = 1;

    (void)unused;
    if (fd < 0) {
        log_printf(LOG_ERROR, "Create socket error: %s", strerror(errno));
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)miio.tcp_port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_printf(LOG_ERROR, "Socket bind port (%d) error: %s",
                   miio.tcp_port, strerror(errno));
        close(fd);
        return NULL;
    }
    log_printf(LOG_INFO, "OT smart conn sockfd: %d", fd);
    for (;;) {
        struct pollfd pfd;
        int rv;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rv = poll(&pfd, 1, 5000);
        if (rv > 0 && (pfd.revents & POLLIN))
            smart_conn_recv_handler_datagram(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* mdns responder (minimal vendor-parity AMD responder)                */
/* ------------------------------------------------------------------ */
#define MDNS_ADDR              "224.0.0.251"
#define MDNS_PORT              5353
#define INSTANCE_TEMPLATE      "%s%s%llu"    /* e.g. "chuangmi720p_miio..." */

static char mdns_hostname[64];
static char mdns_svc_name[128];
static int mdns_recv_fd = -1;
static int mdns_quit = 0;

#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_NSEC 47
#define DNS_TYPE_ANY 255

static int mdns_encode_name(const char *name, unsigned char *out,
                            int outsize)
{
    const char *p = name;
    int n = 0;

    while (*p) {
        const char *dot = strchr(p, '.');
        int lablen = dot ? (int)(dot - p) : (int)strlen(p);

        if (lablen > 63)
            return -1;
        if (n + lablen + 1 + 1 > outsize)
            return -1;
        out[n++] = (unsigned char)lablen;
        memcpy(out + n, p, (size_t)lablen);
        n += lablen;
        p = dot ? dot + 1 : p + lablen;
    }
    out[n++] = 0;
    return n;
}

static int mdns_skip_name(const unsigned char *pkt, int pktlen, int off)
{
    for (;;) {
        int c;
        if (off >= pktlen)
            return -1;
        c = pkt[off];
        if ((c & 0xc0) == 0xc0)
            return off + 2;
        if (c == 0)
            return off + 1;
        off += 1 + c;
    }
}

static void mdns_build_answer(unsigned char *buf, int *off, const char *name,
                              int type, int rrclass, unsigned int ttl,
                              const unsigned char *rdata, int rdlen)
{
    int n = *off;
    int lenpos;
    int nlen;

    nlen = mdns_encode_name(name, buf + n, 300 - n);
    if (nlen <= 0)
        return;
    n += nlen;
    buf[n++] = (unsigned char)((type >> 8) & 0xff);
    buf[n++] = (unsigned char)(type & 0xff);
    buf[n++] = (unsigned char)((rrclass >> 8) & 0xff);
    buf[n++] = (unsigned char)(rrclass & 0xff);
    buf[n++] = (unsigned char)((ttl >> 24) & 0xff);
    buf[n++] = (unsigned char)((ttl >> 16) & 0xff);
    buf[n++] = (unsigned char)((ttl >> 8) & 0xff);
    buf[n++] = (unsigned char)(ttl & 0xff);
    lenpos = n;
    n += 2;
    if (rdlen > 0 && rdata) {
        memcpy(buf + n, rdata, (size_t)rdlen);
        n += rdlen;
    }
    buf[lenpos] = (unsigned char)((rdlen >> 8) & 0xff);
    buf[lenpos + 1] = (unsigned char)(rdlen & 0xff);
    *off = n;
}

static void mdns_reply(const unsigned char *pkt, int len,
                       struct sockaddr_in *to)
{
    unsigned char out[512];
    int id, flags, qd, an;
    int off = 12;
    int o = 12;
    int qtype = -1, qclass = -1;
    char qname[256];

    if (len < 12)
        return;
    id = (pkt[0] << 8) | pkt[1];
    flags = (pkt[2] << 8) | pkt[3];
    qd = (pkt[4] << 8) | pkt[5];
    an = (pkt[6] << 8) | pkt[7];
    if (!qd)
        return;
    /* parse first question only */
    {
        int qnlen = 0;
        for (;;) {
            int c = pkt[off];
            if ((c & 0xc0) == 0xc0) {
                off += 2;
                break;
            }
            off++;
            if (c == 0)
                break;
            if (off + c > len)
                return;
            if (qnlen + c + 1 < (int)sizeof(qname)) {
                memcpy(qname + qnlen, pkt + off, (size_t)c);
                qnlen += c;
                qname[qnlen++] = '.';
            }
            off += c;
        }
        qname[qnlen ? qnlen - 1 : 0] = '\0';
    }
    if (off + 4 > len)
        return;
    qtype = (pkt[off] << 8) | pkt[off + 1];
    qclass = (pkt[off + 2] << 8) | pkt[off + 3];

    (void)flags;
    (void)an;
    (void)qclass;

    memset(out, 0, sizeof(out));
    out[0] = (unsigned char)((id >> 8) & 0xff);
    out[1] = (unsigned char)(id & 0xff);
    out[2] = 0x84;
    out[3] = 0x00;
    out[4] = 0;
    out[5] = 0;
    out[6] = 0;
    out[7] = 1;

    /* https://en.wikipedia.org/wiki/Multicast_DNS */
    if (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) {
        const char *ans;
        ans = "_services._dns-sd._udp.local";
        if (strstr(qname, "_services")) {
            mdns_build_answer(out, &o, ans, DNS_TYPE_PTR, 0x8001, 120,
                              (const unsigned char *)"_miio._udp.local", 17);
        } else {
            mdns_build_answer(out, &o, "_miio._udp.local", DNS_TYPE_PTR,
                              0x8001, 120,
                              (const unsigned char *)mdns_svc_name,
                              (int)strlen(mdns_svc_name));
        }
        if (o + 32 < (int)sizeof(out)) {
            mdns_build_answer(out, &o, mdns_svc_name, DNS_TYPE_SRV,
                              0x8001, 120, NULL, 0);
            out[o - 6] = 0;     /* priority */
            out[o - 5] = 0;     /* priority */
            out[o - 4] = 0;     /* weight */
            out[o - 3] = 0;     /* weight */
            out[o - 2] = (unsigned char)((miio.tcp_port >> 8) & 0xff);
            out[o - 1] = (unsigned char)(miio.tcp_port & 0xff);
            /* hostname target for the SRV record: append via name encode */
            {
                unsigned char nm[128];
                int nl = mdns_encode_name(mdns_hostname, nm, sizeof(nm));
                if (nl > 0 && o + nl < (int)sizeof(out)) {
                    memcpy(out + o - 6, nm, (size_t)nl);
                    o = o - 6 + nl;
                    o += 6;      /* leave rdlen adjust below */
                }
            }
            /* TXT: path=/mydevice */
            {
                const char *txt = "path=/mydevice";
                mdns_build_answer(out, &o, mdns_svc_name, DNS_TYPE_TXT,
                                  0x8001, 120,
                                  (const unsigned char *)txt,
                                  (int)strlen(txt));
            }
        }
    }
    if (qtype == DNS_TYPE_SRV || qtype == DNS_TYPE_ANY) {
        mdns_build_answer(out, &o, mdns_svc_name, DNS_TYPE_SRV, 0x8001,
                          120, NULL, 0);
        out[o - 6] = 0;
        out[o - 5] = 0;
        out[o - 4] = 0;
        out[o - 3] = 0;
        out[o - 2] = (unsigned char)((miio.tcp_port >> 8) & 0xff);
        out[o - 1] = (unsigned char)(miio.tcp_port & 0xff);
        {
            unsigned char nm[128];
            int nl = mdns_encode_name(mdns_hostname, nm, sizeof(nm));
            if (nl > 0 && o + nl < (int)sizeof(out)) {
                memcpy(out + o - 6, nm, (size_t)nl);
                o = o - 6 + nl + 6;
            }
        }
    }
    if (qtype == DNS_TYPE_AAAA && strcasecmp(qname, mdns_hostname) == 0) {
        /* AAAA: no IPv6 on this SoC, no answer */
        (void)0;
    }
    if (o > 12) {
        out[7] = 1;
        sendto(mdns_recv_fd, out, (size_t)o, 0,
               (const struct sockaddr *)to,
               (socklen_t)sizeof(*to));
    }
}

static void mdnsd_set_hostname(const char *hostname)
{
    snprintf(mdns_hostname, sizeof(mdns_hostname), "%s", hostname);
}

static void mdnsd_register_svc(const char *nameid, int port)
    __attribute__((unused));
static void mdnsd_register_svc(const char *nameid, int port)
{
    snprintf(mdns_svc_name, sizeof(mdns_svc_name), "%s._miio._udp.local",
             nameid);
    (void)port;
}

static void mdns_service_destroy(void) __attribute__((unused));
static void mdns_service_destroy(void)
{
    mdns_quit = 1;
    if (mdns_recv_fd >= 0) {
        close(mdns_recv_fd);
        mdns_recv_fd = -1;
    }
}

static void mdnsd_stop(void)
{
    mdns_service_destroy();
}

static int mdnsd_start(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    struct ip_mreq mreq;
    int one = 1;

    if (fd < 0) {
        log_printf(LOG_ERROR, "recv socket(): %m");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(MDNS_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_printf(LOG_ERROR, "recv bind(): %m");
        close(fd);
        return -1;
    }
    inet_pton(AF_INET, MDNS_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
    mdns_recv_fd = fd;
    log_printf(LOG_DEBUG, "mdnsd_start() error = 0");
    return 0;
}

static void *mdnsd_thread(void *unused)
{
    (void)unused;
    if (mdnsd_start() != 0)
        return NULL;
    for (;;) {
        unsigned char buf[512];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n;

        if (mdns_quit)
            break;
        n = recvfrom(mdns_recv_fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            if (mdns_quit)
                break;
            continue;
        }
        mdns_reply(buf, (int)n, &from);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* boot helper: random ntoken / id                                     */
/* ------------------------------------------------------------------ */
static int rand_range(int from, int to)
{
    int r;
    if (to <= from)
        return from;
    r = rand() % (to - from) + from;
    return r;
}

static void generate_random_ntoken(char *buf, int size)
{
    static const char ch[] =
        "0123456789abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;

    if (size < 17)
        return;
    srand((unsigned)time(NULL) ^ get_micro_second() ^ getpid());
    for (i = 0; i < size - 1; i++)
        buf[i] = ch[rand_range(0, (int)strlen(ch) - 1)];
    buf[size - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* local line / boot entries                                           */
/* ------------------------------------------------------------------ */
static int boot_verify_stored(void)
{
    char did[64] = "";
    char key[64] = "";

    if (load_string("did", did, sizeof(did), miio.enckey) == 0)
        snprintf(miio.did, sizeof(miio.did), "%s", did);
    if (load_string("key", key, sizeof(key), miio.enckey) == 0)
        snprintf(miio.key, sizeof(miio.key), "%s", key);
    if (load_string("token", miio.token, sizeof(miio.token),
                    miio.enckey) == 0) {
        log_printf(LOG_INFO, "boot: loaded saved token");
        state_set(STATE_TOKEN_DONE);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* usage / version                                                     */
/* ------------------------------------------------------------------ */
static void print_version(void)
{
    fprintf(stdout, "miio-client %s\n", MIIO_VERSION);
    fprintf(stdout, "miio client - MIIO OT protocol implementation\n");
    fprintf(stdout, "Copyright (C) 2015-2016 Xiaomi\n");
    fprintf(stdout, "Author: Yin Kangkai <yinkangkai@xiaomi.com>\n");
}

static void print_usage(void)
{
    fprintf(stdout, "Usage: miio_client\n");
    fprintf(stdout, "[-D --daemonize]\n");
    fprintf(stdout,
            "[-H --host=<host>] ensure mutual exclusion with -C,you can "
            "only use -C or -H at the same time\n");
    fprintf(stdout, "[-p --port=<port>]\n");
    fprintf(stdout,
            "[-i --interval=<int> ms] set sync interval of _otc.info\n");
    fprintf(stdout,
            "[-l --loglevel=<level>] set loglevel (0-4), bigger = more "
            "verbose\n");
    fprintf(stdout,
            "[-L --logfile=file] output log into file instead of stdout\n");
    fprintf(stdout,
            "[-d --datadir=<path>] set miio data dir path, ending with "
            "'/'\n");
    fprintf(stdout, "[-e --enckey] key(s) are encrypted saved\n");
    fprintf(stdout, "[-E --encdata] data communication are encrypted\n");
    fprintf(stdout,
            "[-C --dcountry] set the country domain where the device "
            "locates in,ignore if it in China\n");
    fprintf(stdout, "[-h --help]\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void sig_handle(int signo)
{
    log_printf(LOG_WARNING, "caught signal %d, exiting", signo);
    mdnsd_stop();
    _exit(0);
}

int main(int argc, char *argv[])
{
    static const struct option long_options[] = {
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'v' },
        { "interval", required_argument, NULL, 'i' },
        { "logfile", required_argument, NULL, 'L' },
        { "daemonize", no_argument, NULL, 'D' },
        { "host", required_argument, NULL, 'H' },
        { "datadir", required_argument, NULL, 'd' },
        { "enckey", no_argument, NULL, 'e' },
        { "encdata", no_argument, NULL, 'E' },
        { "dcountry", required_argument, NULL, 'C' },
        { "loglevel", required_argument, NULL, 'l' },
        { "port", required_argument, NULL, 'p' },
        { NULL, 0, NULL, 0 },
    };
    pthread_t t;
    pthread_attr_t attr;
    int c;
    int daemonize = 0;
    int version_only = 0;

    memset(&miio, 0, sizeof(miio));
    snprintf(miio.data_dir, sizeof(miio.data_dir), "%s", DEFAULT_DIR);
    snprintf(miio.tcp_host, sizeof(miio.tcp_host), "%s", DEFAULT_TCP_HOST);
    snprintf(miio.udp_host, sizeof(miio.udp_host), "%s", DEFAULT_UDP_HOST);
    miio.tcp_port = DEFAULT_PORT;
    miio.line_port = DEFAULT_LINE_PORT;
    miio.bt_port = DEFAULT_BT_PORT;
    miio.sync_interval = DEFAULT_SYNC_INTERVAL;
    miio.cloud_sock = -1;
    miio.state = STATE_DEVICE_INIT;

    srand((unsigned)time(NULL));
    pthread_mutex_init(&miio.lock, NULL);
    clients_init();

    while ((c = getopt_long(argc, argv, "Dp:H:i:l:L:hvd:eEC:",
                            long_options, NULL)) != -1) {
        switch (c) {
        case 'h':
            print_usage();
            return 0;
        case 'v':
            print_version();
            version_only = 1;
            return 0;
        case 'D':
            daemonize = 1;
            break;
        case 'p':
            miio.tcp_port = atoi(optarg);
            log_printf(LOG_INFO, "Set TCP port to \"%s\"", optarg);
            break;
        case 'H':
            if (strlen(optarg) < sizeof(miio.udp_host))
                snprintf(miio.udp_host, sizeof(miio.udp_host), "%s",
                         optarg);
            else
                log_printf(LOG_WARNING,
                           "Set TCP host to \"%s\" fail: too long(%d)",
                           optarg, (int)strlen(optarg));
            log_printf(LOG_INFO, "Set TCP host to \"%s\"", optarg);
            break;
        case 'i':
            miio.sync_interval = atoi(optarg);
            log_printf(LOG_INFO,
                       "Set sync interval of _otc.info to: %d ms",
                       miio.sync_interval);
            break;
        case 'l':
            loglevel = atoi(optarg);
            log_printf(LOG_INFO, "Set log level to: %d", loglevel);
            break;
        case 'L': {
            FILE *f = fopen(optarg, "a");
            if (f)
                logfp = f;
            log_printf(LOG_INFO, "Set logfile to \"%s\"", optarg);
            break;
        }
        case 'd':
            snprintf(miio.data_dir, sizeof(miio.data_dir), "%s", optarg);
            log_printf(LOG_INFO, "Set data dir to: %s", optarg);
            break;
        case 'e':
            miio.enckey = 1;
            log_printf(LOG_INFO, "Key encrypted.");
            break;
        case 'E':
            miio.encdata = 1;
            log_printf(LOG_INFO, "DATA btw app and miio encrypted.");
            break;
        case 'C':
            if (strlen(optarg) < (size_t)sizeof(miio.country_domain)) {
                char h[64];
                snprintf(miio.country_domain, sizeof(miio.country_domain),
                         "%s", optarg);
                snprintf(h, sizeof(h), "%s.%s", optarg, DEFAULT_TCP_HOST);
                snprintf(miio.tcp_host, sizeof(miio.tcp_host), "%s", h);
                snprintf(h, sizeof(h), "%s.%s", optarg, DEFAULT_UDP_HOST);
                snprintf(miio.udp_host, sizeof(miio.udp_host), "%s", h);
                log_printf(LOG_INFO, "Set UDP host to \"%s\"", optarg);
            } else {
                log_printf(LOG_WARNING, "Illegal country domain: %s",
                           optarg);
            }
            break;
        default:
            print_usage();
            return -1;
        }
    }
    (void)version_only;

    if (daemonize && daemon(0, 0) != 0)
        log_printf(LOG_ERROR, "daemonize fail: %m");

    log_printf(LOG_INFO, "miio-client %s start", MIIO_VERSION);
    log_printf(LOG_DEBUG, "enauth: 0");

    if (boot_verify_stored() == 0)
        log_printf(LOG_INFO, "boot: token present, state=%d", miio.state);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    signal(SIGTERM, sig_handle);
    signal(SIGINT, sig_handle);

    pthread_create(&t, &attr, (void *(*)(void *))smart_conn_thread,
                   NULL);
    pthread_create(&t, &attr, (void *(*)(void *))ot_agent_listen_thread,
                   NULL);
    pthread_create(&t, &attr, (void *(*)(void *))line_listen_thread,
                   NULL);
    pthread_create(&t, &attr, (void *(*)(void *))bt_listen_thread, NULL);
    pthread_create(&t, &attr, (void *(*)(void *))server_conn_retry_thread,
                   NULL);
    pthread_create(&t, &attr, (void *(*)(void *))cloud_send_thread, NULL);
    pthread_create(&t, &attr, (void *(*)(void *))ack_timer_thread, NULL);
    pthread_create(&t, &attr, (void *(*)(void *))sync_timer_thread, NULL);
    pthread_create(&t, &attr, (void *(*)(void *))report_keepalive_thread,
                   NULL);
    mdnsd_set_hostname("miio");
    snprintf(mdns_svc_name, sizeof(mdns_svc_name),
             INSTANCE_TEMPLATE, "miio", "_miio.", miio.uid);
    pthread_create(&t, &attr, (void *(*)(void *))mdnsd_thread, NULL);
    pthread_attr_destroy(&attr);

    for (;;)
        pause();
    return 0;
}