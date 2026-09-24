/* ================================================================== */
/* miot_devicekit.c - local reimplementation of the Xiaomi "miot_     */
/* devicekit" (Mi IOT device kit) daemon for the Mi Home ecosystem.   */
/*                                                                    */
/* Rebuilt from the ARM binary                                        */
/*   `chuangmi-v5-dump/data/miot/miot_devicekit`                      */
/* (70795 bytes, NOT stripped, full DWARF + symtab available).        */
/*                                                                    */
/* The vendor daemon is the on-device partner of `miio_client`: it    */
/* keeps a TCP line connection to miio_client's OTD port (54322,      */
/* `htons(54322)` = bytes D4 32 found in the binary), handles         */
/* `miIO.restore` / `local.time` / `local.status` / `set_wdr` /       */
/* `miIO.shell` / `set_power` messages, drives the ISP carve-out      */
/* (day/night/WDR via /proc/isp328/command + /sys/class/timed_output/ */
/* ir-filter), the NTC photodetector / ADC auto day-night logic, the  */
/* hardware reset button (/dev/input/event0 key restore to factory),  */
/* a house-keeping timerfd (cleans nvram housekeep-* keys), factory   */
/* mode (ftmode) and the ring-buffer log that `logtf.sh` flushes on   */
/* reboot. It has NO threads: a single poll() loop drives everything. */
/*                                                                    */
/* Function names and every log/format string are copied verbatim     */
/* from the vendor binary's symtab/rodata.  Vendor used json-c for    */
/* parsing (json_tokener_*, json_object_*); libjson-c.so.2 is NOT     */
/* present in this firmware, so a self-contained JSON walker replaces  */
/* it (same behaviour, no dependency), like miio_avstreamer.c.        */
/* Layout of the `kit` struct and the /dev/isp328 ioctl numbers are   */
/* reconstructed (driver headers were not dumped).                    */
/*                                                                    */
/*      make build/miot_devicekit                                     */
/* ================================================================== */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/input.h>

/* ------------------------------------------------------------------ */
/* constants & vendor identifiers                                      */
/* ------------------------------------------------------------------ */

#define OTD_DEFAULT_HOST        "127.0.0.1"
#define OTD_DEFAULT_PORT        54322          /* miio_client OTD line port */
#define OTD_RECONNECT_DELAY_MS  1000
#define OTD_LINE_TIMEOUT_MS     3000           /* blocking sub-read timeout */

#define LIGHT_MODE              0
#define NIGHT_MODE              1
#define WDR_MODE                2

#define LOG_DEBUG               0
#define LOG_WARNING             1
#define LOG_INFO                2
#define LOG_ERROR               3

#define SOUND_FIFO              "/tmp/sound_fifo"
#define LOGTF_SCRIPT            "/mnt/data/miot/logtf.sh"
#define P2PID_DIR               "/mnt/data/p2pid"
#define SOUND_BOOT              "/mnt/data/sound/booting.aac"
#define SOUND_RESET             "/mnt/data/sound/reset_success.aac"

#define KEY_EVENT_DEV           "/dev/input/event0"
#define ISP_DEV                 "/dev/isp328"
#define ISP_PROC_CMD            "/proc/isp328/command"
#define MLAN_OPERSTATE          "/sys/class/net/mlan0/operstate"
#define HPKE_KEY_OFFSET         3               /* housekeep slot id range */

#define RING_SIZE               384
#define RING_LEN                (RING_SIZE / 6)   /* ~64 lines capped */

/* ------------------------------------------------------------------ */
/* log level / timestamp / ring buffer                                 */
/* ------------------------------------------------------------------ */

static int g_loglevel = LOG_INFO;                /* data@g 0x19CB8 = 3 */
static int g_logtimestamp;                       /* -t option */
static FILE *log_file;                           /* -L option */
static char RingBuffer[RING_SIZE];               /* bss self-check ring */
static int ring_end;                             /* next write pos */
static int bRingFull;
static int logCnt;
static char OTD_SEND_BUFF[2048];

#define HPKE_KEYS                                             \
    "housekeep-time", "housekeep-area-ptz-x",                  \
    "housekeep-area-ptz-y", "housekeep-switch", "housekeep-tag"

static void log_printf(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void log_printf(int level, const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    char prefix[8];
    int n = 0;

    if (level < g_loglevel)
        return;

    switch (level) {
    case LOG_DEBUG:   strcpy(prefix, "[DEBUG]");   break;
    case LOG_WARNING: strcpy(prefix, "[WARNING]"); break;
    case LOG_INFO:    strcpy(prefix, "[INFO]");    break;
    case LOG_ERROR:   strcpy(prefix, "[ERROR]");   break;
    default:          strcpy(prefix, "[INFO]");    break;
    }

    if (g_logtimestamp) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        if (tm && strftime(ts, sizeof(ts), "[%Y%m%d %H:%M:%S]", tm))
            n += snprintf(buf + n, sizeof(buf) - n, "%s ", ts);
    }

    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    if (log_file)
        fprintf(log_file, "%s %s\n", prefix, buf);
    else
        fprintf(stdout, "%s %s\n", prefix, buf);
    fflush(log_file ? log_file : stdout);

    /* keep a small tail in the ring buffer; logtf.sh flushes it out */
    {
        int len = strlen(buf);
        int i;
        if (len > RING_SIZE)
            len = RING_SIZE;
        for (i = 0; i < len; i++) {
            RingBuffer[ring_end] = buf[i];
            ring_end = (ring_end + 1) % RING_SIZE;
            if (ring_end == 0) {
                bRingFull = 1;
            }
        }
        logCnt++;
    }
}

static int logfile_init(const char *path)
{
    FILE *f = fopen(path, "a");
    if (!f)
        return -1;
    log_file = f;
    return 0;
}

/* ------------------------------------------------------------------ */
/* mini JSON walker (json-c replacement; keeps vendor helper names)    */
/* ------------------------------------------------------------------ */

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *json_skip_value(const char *p)
{
    int depth;
    if (!p)
        return p;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\')
                p++;
            p++;
        }
        if (*p)
            p++;
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        depth = 0;
        do {
            if (*p == open)
                depth++;
            else if (*p == close)
                depth--;
            if (*p)
                p++;
        } while (*p && depth > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/* return pointer at the START of the value for `key` at depth 1 */
static const char *json_find_value(const char *json, const char *key)
{
    const char *p = json_skip_ws(json);
    size_t want = strlen(key);

    if (*p != '{')
        return NULL;
    p++;
    for (;;) {
        const char *ks;
        int klen;
        p = json_skip_ws(p);
        if (*p == '}')
            return NULL;
        if (*p != '"')
            return NULL;
        p++;
        ks = p;
        while (*p && *p != '"') {
            if (*p == '\\')
                p++;
            p++;
        }
        if (!*p)
            return NULL;
        klen = (int)(p - ks);
        p++;
        if ((size_t)klen == want && strncmp(ks, key, want) == 0) {
            p = json_skip_ws(p);
            if (*p != ':')
                return NULL;
            return json_skip_ws(p + 1);
        }
        p = json_skip_ws(p);
        if (*p != ':')
            return NULL;
        p = json_skip_value(json_skip_ws(p + 1));
        p = json_skip_ws(p);
        if (*p == ',')
            p++;
        else if (*p == '}')
            return NULL;
        else
            return NULL;
    }
}

static int json_verify(const char *str)
{
    const char *p = json_skip_ws(str);
    if (*p != '{')
        return -1;
    p = json_skip_value(p);
    p = json_skip_ws(p);
    if (*p != '\0')
        return -1;
    return 0;
}

static int json_verify_method(const char *str, const char *method)
{
    char m[128];
    const char *v, *p;
    int len;

    if (json_verify(str) != 0)
        return -1;
    v = json_find_value(str, "method");
    if (!v || *v != '"')
        return -1;
    v++;
    p = v;
    while (*p && *p != '"' && (p - v) < (int)sizeof(m) - 1) {
        m[p - v] = *p;
        p++;
    }
    len = (int)(p - v);
    m[len] = '\0';
    return (strcmp(m, method) == 0) ? 0 : -1;
}

/* verify method AND that `key`'s string value == `value` */
static int json_verify_method_value(const char *str, const char *method,
                                    const char *key, const char *value)
{
    char v[128];
    const char *p;
    int len;

    if (json_verify_method(str, method) != 0)
        return -1;
    p = json_find_value(str, key);
    if (!p)
        return -1;
    if (*p == '"')
        p++;
    len = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' &&
           *p != ']' && len < (int)sizeof(v) - 1)
        v[len++] = *p++;
    v[len] = '\0';
    return (strcmp(v, value) == 0) ? 0 : -1;
}

static int json_verify_get_string(const char *str, const char *key,
                                  char *out, int outsize)
{
    const char *p;
    int len = 0;

    if (json_verify(str) != 0)
        return -1;
    p = json_find_value(str, key);
    if (!p)
        return -1;
    if (*p == '"')
        p++;
    while (*p && *p != '"' && *p != ',' && *p != '}' &&
           *p != ']' && len < outsize - 1)
        out[len++] = *p++;
    out[len] = '\0';
    return 0;
}

static int json_verify_get_int(const char *str, const char *key, int *out)
{
    const char *p;

    if (json_verify(str) != 0)
        return -1;
    p = json_find_value(str, key);
    if (!p)
        return -1;
    *out = atoi(p);
    return 0;
}

static int json_verify_get_array(const char *str, const char *key,
                                 int *arr, int *n)
{
    const char *p;
    int cnt = 0;

    if (json_verify(str) != 0)
        return -1;
    p = json_find_value(str, key);
    if (!p || *p != '[')
        return -1;
    p++;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == ']')
            break;
        arr[cnt++] = atoi(p);
        p = json_skip_value(p);
        p = json_skip_ws(p);
        if (*p == ',')
            p++;
        else if (*p == ']')
            break;
    }
    *n = cnt;
    return 0;
}

/* ------------------------------------------------------------------ */
/* random / time / string utils (vendor util.c)                        */
/* ------------------------------------------------------------------ */

static unsigned long long get_micro_second(void) __attribute__((unused));
static unsigned long long get_micro_second(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static int get_uptime(char *buf, int size)
{
    struct sysinfo si;
    if (sysinfo(&si) != 0) {
        log_printf(LOG_ERROR, "get uptime fail, %m");
        return -1;
    }
    snprintf(buf, size, "%lu", (unsigned long)si.uptime);
    return 0;
}

static int get_port(void)
{
    /* Reads the OTD port.  Vendor compiles the default into rodata
     * (htons(54322)); we honour an optional nvram override. */
    char *r = NULL;
    int port = OTD_DEFAULT_PORT;
    FILE *f = popen("/usr/sbin/nvram get miio_otd_port", "r");
    if (f) {
        char line[64];
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strlen(line))
                r = line;
        }
        pclose(f);
    }
    if (r && atoi(r) > 0)
        port = atoi(r);
    return port;
}

static void get_ip_str(char *out, int size)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    if (fd < 0) {
        strncpy(out, "", size);
        return;
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "mlan0", sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        strncpy(out, "", size);
        close(fd);
        return;
    }
    if (inet_ntop(AF_INET,
                  &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr,
                  out, size) == NULL) {
        snprintf(out, size, "Unknown AF");
    }
    close(fd);
}

static const char rand_chars[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void rand_str(char *buf, int len) __attribute__((unused));
static void rand_str(char *buf, int len)
{
    int i;
    for (i = 0; i < len; i++)
        buf[i] = rand_chars[rand() % (sizeof(rand_chars) - 1)];
    buf[len] = '\0';
}

static int rand_range(int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + rand() % (hi - lo + 1);
}

static void generate_random_id(unsigned id)
{
    srand((unsigned)time(NULL) ^ id);
}

/* hex <-> binary */
static int str2hex(const char *s, unsigned char *out, int outsize)
    __attribute__((unused));
static int str2hex(const char *s, unsigned char *out, int outsize)
{
    int n = 0;
    while (*s && n < outsize) {
        unsigned v = 0;
        const char *c = s;
        if (*c >= '0' && *c <= '9') v = *c - '0';
        else if (*c >= 'a' && *c <= 'f') v = *c - 'a' + 10;
        else if (*c >= 'A' && *c <= 'F') v = *c - 'A' + 10;
        else break;
        s++;
        if ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
            (*s >= 'A' && *s <= 'F')) {
            unsigned v2;
            c = s;
            if (*c >= '0' && *c <= '9') v2 = *c - '0';
            else if (*c >= 'a' && *c <= 'f') v2 = *c - 'a' + 10;
            else v2 = *c - 'A' + 10;
            s++;
            out[n++] = (unsigned char)((v << 4) | v2);
        } else {
            out[n++] = (unsigned char)v;
        }
    }
    return n;
}

static void hex2str(const unsigned char *in, int inlen, char *out, int outsize)
    __attribute__((unused));
static void hex2str(const unsigned char *in, int inlen, char *out, int outsize)
{
    int i, o = 0;
    for (i = 0; i < inlen && o < outsize - 3; i++)
        o += snprintf(out + o, outsize - o, "%02x", in[i]);
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* XOR crypto (vendor `data_encrypt`/`data_decrypt`/`xor_encrypt`).    */
/* The rodata key "Dl:L:hvm:t" (0x111C0) is preserved verbatim.        */
/* ------------------------------------------------------------------ */

static void xor_encrypt(unsigned char *data, int len, const char *key)
{
    int klen = strlen(key);
    int i;
    for (i = 0; i < len; i++)
        data[i] ^= key[i % klen];
}

static int data_encrypt(unsigned char *data, int len, int enc,
                        unsigned char *buf, int bufsize)
    __attribute__((unused));
static int data_encrypt(unsigned char *data, int len, int enc,
                        unsigned char *buf, int bufsize)
{
    if (!data || !buf || bufsize < len) {
        log_printf(LOG_ERROR, "outsize (%d) too small", bufsize);
        return -1;
    }
    memcpy(buf, data, len);
    if (enc)
        xor_encrypt(buf, len, "Dl:L:hvm:t");
    return len;
}

static int data_decrypt(unsigned char *data, int len, int dec,
                        unsigned char *buf, int bufsize)
    __attribute__((unused));
static int data_decrypt(unsigned char *data, int len, int dec,
                        unsigned char *buf, int bufsize)
{
    return data_encrypt(data, len, dec, buf, bufsize);
}

static int write_path(const char *path, const char *s, int len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int n;
    if (fd < 0)
        return -1;
    n = write(fd, s, len);
    if (n != len) {
        log_printf(LOG_ERROR, "Partial written");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* shell / nvram helpers                                               */
/* ------------------------------------------------------------------ */

static int run_cmd(const char *cmd)
{
    char buf[512];
    char *p;
    FILE *fp;

    log_printf(LOG_INFO, "run cmd: %s", cmd);
    fp = popen(cmd, "r");
    if (!fp) {
        log_printf(LOG_ERROR, "%s(), popen error: %m", __func__);
        return -1;
    }
    buf[0] = '\0';
    if (fread(buf, 1, sizeof(buf) - 1, fp) > 0) {
        buf[sizeof(buf) - 1] = '\0';
        p = buf + strlen(buf);
        while (p > buf && (p[-1] == '\n' || p[-1] == '\r'))
            *--p = '\0';
        if (strlen(buf))
            log_printf(LOG_DEBUG, "%s() => %s", __func__, buf);
    }
    pclose(fp);
    return 0;
}

static char *nvram_get(const char *key)
{
    static char buf[512];
    FILE *f;
    char cmd[256];
    char *nl;

    snprintf(buf, sizeof(buf), "unknown");
    snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram get %s", key);
    f = popen(cmd, "r");
    if (!f)
        return buf;
    if (fgets(buf, sizeof(buf), f)) {
        nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
    }
    pclose(f);
    return buf;
}

static void nvram_set(const char *k, const char *v)
{
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram set %s=%s", k, v);
    run_cmd(cmd);
}

static void nvram_unset(const char *k)
{
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram unset %s", k);
    run_cmd(cmd);
}

/* ------------------------------------------------------------------ */
/* kit state                                                           */
/* ------------------------------------------------------------------ */

struct kit {
    int otd_sock;                 /* TCP to miio_client (54322) */
    int port;
    int otd_ready;
    int timer_fd;                 /* housekeeping one-shot */
    int ft_timer_fd;              /* factory mode timer */
    int key_fd;                   /* /dev/input/event0 */
    int current_mode;             /* LIGHT_MODE/NIGHT_MODE/WDR_MODE */
    int ftmode;
    int last_msg_id;              /* rpc id for query replies */
    int nfds;
    struct pollfd pollfds[8];
    struct sockaddr_in servaddr;
    unsigned int last_adc_switch; /* monotonic ms of last NTC day/night */
};

static struct kit kit;

static void device_normal_process(void);
static void device_sleep_process(void);

/* ZIMI_RT_Table (rodata @0xFE24, 0x4C0 = 152 x u32 pairs), exact copy
 * of the key->value LUT from the binary; `compare()` serves bsearch. */
static const unsigned int ZIMI_RT_Table[152][2] = {
    {0xE2,0x1D2E0},{0xE3,0x1BA94},{0xE4,0x1A3BA},{0xE5,0x18E2A},{0xE6,0x179DA},
    {0xE7,0x166B6},{0xE8,0x154AA},{0xE9,0x143A2},{0xEA,0x1338A},{0xEB,0x12462},
    {0xEC,0x1160C},{0xED,0x1087E},{0xEE,0x0FBAE},{0xEF,0x0EF92},{0xF0,0x0E420},
    {0xF1,0x0D944},{0xF2,0x0CF08},{0xF3,0x0C54E},{0xF4,0x0BC20},{0xF5,0x0B36A},
    {0xF6,0x0AB22},{0xF7,0x0A352},{0xF8,0x09BE6},{0xF9,0x094DE},{0xFA,0x08E30},
    {0xFB,0x087D2},{0xFC,0x081CE},{0xFD,0x07C1A},{0xFE,0x076AC},{0xFF,0x0717A},
    {0x00,0x06C98},{0x01,0x067E8},{0x02,0x06374},{0x03,0x05F3C},{0x04,0x05B36},
    {0x05,0x05762},{0x06,0x053C0},{0x07,0x05046},{0x08,0x04CF4},{0x09,0x049D4},
    {0x0A,0x046D2},{0x0B,0x043EE},{0x0C,0x04132},{0x0D,0x03E94},{0x0E,0x03C1E},
    {0x0F,0x039BC},{0x10,0x03778},{0x11,0x03548},{0x12,0x03336},{0x13,0x03142},
    {0x14,0x02F58},{0x15,0x02D8C},{0x16,0x02BD4},{0x17,0x02A26},{0x18,0x02896},
    {0x19,0x02710},{0x1A,0x0259E},{0x1B,0x02436},{0x1C,0x022E2},{0x1D,0x021A2},
    {0x1E,0x0206C},{0x1F,0x01F40},{0x20,0x01E1E},{0x21,0x01D06},{0x22,0x01C02},
    {0x23,0x01B08},{0x24,0x01A0E},{0x25,0x01928},{0x26,0x0184C},{0x27,0x01770},
    {0x28,0x016A8},{0x29,0x015E0},{0x2A,0x01522},{0x2B,0x0146E},{0x2C,0x013BA},
    {0x2D,0x01310},{0x2E,0x01270},{0x2F,0x011DA},{0x30,0x01144},{0x31,0x010AE},
    {0x32,0x01022},{0x33,0x00FA0},{0x34,0x00F1E},{0x35,0x00EA6},{0x36,0x00E2E},
    {0x37,0x00DB6},{0x38,0x00D48},{0x39,0x00CE4},{0x3A,0x00C76},{0x3B,0x00C12},
    {0x3C,0x00BB8},{0x3D,0x00B5E},{0x3E,0x00B04},{0x3F,0x00AAA},{0x40,0x00A5A},
    {0x41,0x00A0A},{0x42,0x009BA},{0x43,0x00974},{0x44,0x0092E},{0x45,0x008E8},
    {0x46,0x008A2},{0x47,0x00866},{0x48,0x0082A},{0x49,0x007EE},{0x4A,0x007B2},
    {0x4B,0x00776},{0x4C,0x00744},{0x4D,0x00708},{0x4E,0x006D6},{0x4F,0x006A4},
    {0x50,0x0067C},{0x51,0x0064A},{0x52,0x00622},{0x53,0x005F0},{0x54,0x005C8},
    {0x55,0x005A0},{0x56,0x00578},{0x57,0x0055A},{0x58,0x00532},{0x59,0x0050A},
    {0x5A,0x004EC},{0x5B,0x004CE},{0x5C,0x004A6},{0x5D,0x00488},{0x5E,0x0046A},
    {0x5F,0x0044C},{0x60,0x0042E},{0x61,0x0041A},{0x62,0x003FC},{0x63,0x003DE},
    {0x64,0x003CA},{0x65,0x003B6},{0x66,0x00398},{0x67,0x00384},{0x68,0x00370},
    {0x69,0x00352},{0x6A,0x0033E},{0x6B,0x0032A},{0x6C,0x00316},{0x6D,0x00302},
    {0x6E,0x002F8},{0x6F,0x002E4},{0x70,0x002D0},{0x71,0x002BC},{0x72,0x002B2},
    {0x73,0x0029E},{0x74,0x0028A},{0x75,0x00280},{0x76,0x0026C},{0x77,0x00262},
    {0x78,0x00258},{0x7D,0x00212},
};

static int compare(const void *a, const void *b)
{
    const unsigned int *p = a;
    const unsigned int *q = b;
    if (*p < q[0])
        return -1;
    if (*p > q[0])
        return 1;
    return 0;
}

static unsigned int zimi_lookup(unsigned int key)
{
    /* hand-rolled bsearch over the sorted LUT */
    int lo = 0, hi = (int)(sizeof(ZIMI_RT_Table) / sizeof(ZIMI_RT_Table[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (compare(&key, ZIMI_RT_Table[mid]) == 0)
            return ZIMI_RT_Table[mid][1];
        if (compare(&key, ZIMI_RT_Table[mid]) < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* /sys + /proc writers (ISP carve-out)                                */
/* ------------------------------------------------------------------ */

static void proc_write(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_printf(LOG_ERROR, "can't open %s: %m", path);
        return;
    }
    if (write(fd, s, strlen(s)) < 0)
        log_printf(LOG_ERROR, "write %s: %m", path);
    close(fd);
}

static void isp_cmd(const char *cmd)
{
    proc_write(ISP_PROC_CMD, cmd);
}

/* ------------------------------------------------------------------ */
/* voice / LED                                                         */
/* ------------------------------------------------------------------ */

static void voice_play(const char *path)
{
    struct stat st;
    int fd;
    char msg[256];
    int n;

    if (stat(path, &st) != 0) {
        log_printf(LOG_ERROR, "devicekit: stat (%s) error: %m", path);
        return;
    }
    if (!S_ISFIFO(st.st_mode)) {
        log_printf(LOG_ERROR, "devicekit: %s not FIFO", path);
        return;
    }
    fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        log_printf(LOG_ERROR, "devicekit: %s open fail: %m", path);
        return;
    }
    n = snprintf(msg, sizeof(msg), "%s\n", path);
    if (write(fd, msg, n) < 0) {
        log_printf(LOG_ERROR, "devicekit: %s write fail: %m", path);
    }
    close(fd);
}

static void ledctl(const char *args)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/mnt/data/miot/ledctl %s", args);
    run_cmd(cmd);
}

/* ------------------------------------------------------------------ */
/* ISP day/night/WDR switching                                         */
/* ------------------------------------------------------------------ */

static int ircut_switch(int new_mode)
{
    int cur = kit.current_mode;

    if (cur == new_mode)
        return 0;

    if (cur == NIGHT_MODE && new_mode == WDR_MODE) {
        log_printf(LOG_WARNING,
                   "current ircut mode is NIGHT_MODE, "
                   "do not allowed to change to WDR_MODE");
        return -1;
    }
    if (cur == WDR_MODE && new_mode == LIGHT_MODE) {
        log_printf(LOG_WARNING,
                   "WDR is enabled, do not allowed to change to LIGHT_MODE, "
                   "just set to WDR MODE");
        new_mode = WDR_MODE;
    }

    log_printf(LOG_INFO, "ircut_switch(%d->%d, wdr=%d)",
               cur, new_mode, nvram_get("wdr")[0] == 'o' ? 0 : 1);

    if (new_mode == NIGHT_MODE) {
        proc_write("/proc/isp328/ae/max_gain", "8192");
        proc_write("/proc/isp328/ae/max_isp_gain", "512");
        proc_write("/proc/isp328/ae/max_sensor_gain", "2048");
        proc_write("/proc/vcap300/input_module/isp/data_range", "2 0 1 2 3 0");
        isp_cmd("w daynight 1");
        isp_cmd("w sen_fps 15");
        isp_cmd("w dr_mode 0");
        isp_cmd("w reload_cfg /gm/config/isp328_ov2718_night.cfg");
        log_printf(LOG_INFO, "Enter NIGHT MODE");
    } else if (new_mode == LIGHT_MODE) {
        proc_write("/proc/isp328/ae/max_gain", "4096");
        proc_write("/proc/isp328/ae/max_isp_gain", "128");
        proc_write("/proc/vcap300/input_module/isp/data_range", "2 0 1 2 3 0");
        isp_cmd("w daynight 0");
        isp_cmd("w sen_fps 20");
        isp_cmd("w reload_cfg /gm/config/isp328_ov2718.cfg");
        log_printf(LOG_INFO, "Enter LIGHT MODE");
    } else if (new_mode == WDR_MODE) {
        isp_cmd("w dr_mode 1");
        isp_cmd("w reload_cfg /gm/config/isp328_ov2718_wdr.cfg");
        log_printf(LOG_INFO, "Enter WDR MODE");
    } else {
        log_printf(LOG_ERROR, "ircut_switch(), Unknown mode");
        return -1;
    }

    kit.current_mode = new_mode;
    return 0;
}

static void SaveWDRStatustoDisk(int state)
{
    if (state == WDR_MODE)
        run_cmd("/usr/sbin/nvram set wdr=on");
    else
        run_cmd("/usr/sbin/nvram set wdr=off");
    run_cmd("/usr/sbin/nvram commit");
}

static void EnableWDR(int on)
{
    int target = on ? WDR_MODE : LIGHT_MODE;
    if (target == WDR_MODE || kit.current_mode != WDR_MODE)
        ircut_switch(target);
}

static void unset_motion_alarm_param(void)
{
    /* de-activate alarm state that a child reboot may have left */
    run_cmd("/usr/sbin/nvram unset motion_alarm");
    run_cmd("/usr/sbin/nvram commit");
}

/* ------------------------------------------------------------------ */
/* NTC / photodetector auto day-night (vendor reads back ADC + ISP     */
/* gain/EV through /dev/isp328 ioctls; numbers reconstructed).         */
/* ------------------------------------------------------------------ */

#define _ISP_IOC_MAGIC         'Y'
#define ISP328_IOC_GET_GAIN    _IOR(_ISP_IOC_MAGIC, 0x01, unsigned int*)
#define ISP328_IOC_GET_EV      _IOR(_ISP_IOC_MAGIC, 0x02, unsigned int*)
#define ISP328_IOC_ADC0_READ   _IOR(_ISP_IOC_MAGIC, 0x10, unsigned int*)
#define ISP328_IOC_ADC1_READ   _IOR(_ISP_IOC_MAGIC, 0x11, unsigned int*)

static int adc_monitor(int isp_fd, int *gain, int *ev,
                       unsigned int *adc0, unsigned int *adc1, int *C)
{
    if (ioctl(isp_fd, ISP328_IOC_GET_GAIN, gain) != 0) {
        log_printf(LOG_ERROR, "isp328 get current gain error(%s): %m",
                   nvram_get("gain"));
        return -1;
    }
    if (ioctl(isp_fd, ISP328_IOC_GET_EV, ev) != 0) {
        log_printf(LOG_ERROR, "isp328 get current EV error(%s): %m",
                   nvram_get("gain"));
        return -1;
    }
    if (ioctl(isp_fd, ISP328_IOC_ADC0_READ, adc0) != 0) {
        log_printf(LOG_ERROR, "read ADC0 error! = %d: %m", errno);
        return -1;
    }
    if (ioctl(isp_fd, ISP328_IOC_ADC1_READ, adc1) != 0) {
        log_printf(LOG_ERROR, "DIRECT_READ ADC1 error! = %d, (%m)", errno);
        return -1;
    }
    if (*adc1) {
        /* photoresistor resistance from ADC1, RGB-ish lux via LUT */
        *C = (int)zimi_lookup((unsigned int)(*adc1 & 0xFF));
        if (*C != 0) {
            log_printf(LOG_DEBUG,
                       "(%d)ADC status =0x%x val = 0x%x(->0x%x); "
                       "Gain=%u; EV=%u; ADC1=0x%x, R=%d, C=%f",
                       0, *adc0, *adc1, (unsigned int)(*C), *gain, *ev,
                       *adc1, (int)((double)*adc1 * 3.3 / 4096.0),
                       (double)*C);
        }
    }
    return 0;
}

static void adc_daynight_check(int isp_fd)
{
    int gain = 0, ev = 0, C = 0;
    unsigned int adc0 = 0, adc1 = 0;
    struct timespec ts;
    unsigned long now_ms;

    if (adc_monitor(isp_fd, &gain, &ev, &adc0, &adc1, &C) != 0)
        return;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_ms = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;

    if (C > 6000 && kit.current_mode == LIGHT_MODE) {
        if (now_ms - kit.last_adc_switch < 60000UL) {
            log_printf(LOG_WARNING, "switch rate is too fast (%u)",
                       (unsigned)(now_ms - kit.last_adc_switch));
            return;
        }
        log_printf(LOG_INFO, "photodetector switch to night mode. "
                   "ADC0=0x%x; Gain=%u; EV=%u; ADC1=0x%x, C=%f",
                   adc0, gain, ev, adc1, (double)C);
        kit.last_adc_switch = now_ms;
        ircut_switch(NIGHT_MODE);
    } else if (C != 0 && C < 6000 && kit.current_mode == NIGHT_MODE) {
        kit.last_adc_switch = now_ms;
        log_printf(LOG_INFO, "photodetector switch to linear mode. "
                   "ADC0=0x%x; Gain=%u; EV=%u; ADC1=0x%x, C=%f",
                   adc0, gain, ev, adc1, (double)C);
        ircut_switch(LIGHT_MODE);
    } else if (kit.current_mode == LIGHT_MODE && adc0 == 0 && adc1 == 0) {
        log_printf(LOG_WARNING, "Maybe something masked ur lens...");
    }
}

/* ------------------------------------------------------------------ */
/* OTD line socket (TCP to miio_client on port 54322)                  */
/* ------------------------------------------------------------------ */

static int wait_sock_timeout(int fd, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    return poll(&pfd, 1, timeout_ms);
}

static void otd_close_retry(int fd)
{
    if (fd >= 0) {
        close(fd);
        kit.otd_sock = -1;
        kit.otd_ready = 0;
    }
    log_printf(LOG_WARNING, "Retry connecting to miio_client...");
}

static int otd_sock_init(void)
{
    int fd = -1;

    if (kit.otd_ready && kit.otd_sock >= 0)
        return 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_printf(LOG_ERROR, "Create socket error: %m");
        return -1;
    }
    memset(&kit.servaddr, 0, sizeof(kit.servaddr));
    kit.servaddr.sin_family = AF_INET;
    kit.servaddr.sin_addr.s_addr = inet_addr(OTD_DEFAULT_HOST);
    kit.servaddr.sin_port = htons((unsigned short)kit.port);

    if (connect(fd, (struct sockaddr *)&kit.servaddr,
                sizeof(kit.servaddr)) < 0) {
        log_printf(LOG_WARNING, "Connect to otd error: %s:%d",
                   OTD_DEFAULT_HOST, kit.port);
        close(fd);
        kit.otd_sock = -1;
        kit.otd_ready = 0;
        return -1;
    }

    kit.otd_sock = fd;
    kit.otd_ready = 1;
    log_printf(LOG_INFO, "Connected to miio_client.");
    log_printf(LOG_INFO, "OTD sockfd: %d", kit.otd_sock);
    return 0;
}

static void otd_reconnect(void)
{
    otd_sock_init();
}

/* ------------------------------------------------------------------ */
/* one-line send + blocking receive helpers                            */
/* ------------------------------------------------------------------ */

struct msgs {
    char *line;               /* full line (json-c msg in vendor) */
    int id;
};

static int general_send_one(struct msgs *msg)
{
    int len;

    if (kit.otd_sock < 0 || !kit.otd_ready) {
        log_printf(LOG_ERROR, "%s(), otd_sock not ready: %d",
                   __func__, kit.otd_sock);
        return -1;
    }
    len = strlen(msg->line);
    if (send(kit.otd_sock, msg->line, len, MSG_NOSIGNAL) < 0) {
        log_printf(LOG_ERROR, "%s,%s: send error: %m", "devicekit.c",
                   __func__);
        return -1;
    }
    return 0;
}

/* read a single JSON line from the OTD socket (blocking, with timeout) */
static int recv_line_fd(int fd, char *buf, int bufsize, int timeout_ms)
{
    int rd, total = 0;
    struct timeval tv;
    fd_set rfds;

    memset(buf, 0, bufsize);
    while (total < bufsize - 1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        rd = recv(fd, buf + total, bufsize - 1 - total, 0);
        if (rd < 0)
            break;
        if (rd == 0)
            break;
        total += rd;
        if (buf[total - 1] == '\n')
            break;
    }
    buf[total] = '\0';
    return total;
}

/* ------------------------------------------------------------------ */
/* time / status query round-trips with miio_client                    */
/* ------------------------------------------------------------------ */

static void time_sync(void)
{
    char req[256];
    char rsp[1024];
    struct msgs m;
    int ts = 0;

    kit.last_msg_id = rand_range(1, 0x7fffffff);
    snprintf(req, sizeof(req),
             "{\"id\":%d,\"method\":\"local.query_time\",\"params\":null}",
             kit.last_msg_id);
    m.line = req;
    if (general_send_one(&m) < 0)
        return;

    if (recv_line_fd(kit.otd_sock, rsp, sizeof(rsp), OTD_LINE_TIMEOUT_MS) <= 0)
        return;
    if (json_verify_get_int(rsp, "result", &ts) != 0)
        json_verify_get_int(rsp, "params", &ts);
    if (ts > 0) {
        struct timeval tv;
        tv.tv_sec = ts;
        tv.tv_usec = 0;
        if (settimeofday(&tv, NULL) == 0)
            log_printf(LOG_INFO, "time_sync OK: %ld", (long)tv.tv_sec);
    }
}

static void otd_status_check(void)
{
    char req[256];
    char rsp[1024];
    struct msgs m;

    kit.last_msg_id = rand_range(1, 0x7fffffff);
    snprintf(req, sizeof(req),
             "{\"id\":%d,\"method\":\"local.query_status\",\"params\":null}",
             kit.last_msg_id);
    m.line = req;
    if (general_send_one(&m) < 0)
        return;
    if (recv_line_fd(kit.otd_sock, rsp, sizeof(rsp), OTD_LINE_TIMEOUT_MS) <= 0)
        return;
    log_printf(LOG_INFO, "otd_status_check: %s", rsp);
}

/* ------------------------------------------------------------------ */
/* factory reset / reboot                                              */
/* ------------------------------------------------------------------ */

static void kit_reboot(void)
{
    char req[128];
    struct msgs m;
    char uptime[64];

    log_printf(LOG_INFO, "%s(), reset done and reboot 5s later...", __func__);
    get_uptime(uptime, sizeof(uptime));
    run_cmd(LOGTF_SCRIPT);
    ledctl("0 0  1 0 0 2");
    voice_play(SOUND_RESET);

    snprintf(req, sizeof(req),
             "{\"method\":\"_internal.reboot\",\"params\":null}");
    m.line = req;
    general_send_one(&m);

    sleep(5);
    run_cmd("rm -rf " P2PID_DIR);
    run_cmd("sync");
    exit(0);                    /* reboot is applied by inittab/rcK */
}

/* unset/set tables: data.data.rel.ro @0x199B8 / @0x19A08 (verbatim) */
static const char *unset_keys_list[] = {
    "miio_key_mgmt", "miio_passwd", "miio_ssid", "miio_token", "miio_uid",
    "miio_country", "smb_dir", "smb_server_name", "smb_display_name",
    "smb_username", "smb_password", "smb_location", "led_owner",
    "mible_evtrule", "ibeacon_uuid", "sleep_ptz-y", "sleep_ptz-x",
    "ptz-x", "ptz-y", NULL
};

static const char *set_keyvalue_list[] = {
    "light=on", "sdcard_status=0", "band_nearby=off", "motion_record=on",
    "alarm_time_interval=5", "motion_alarm=[0,0,0,0,0,0,1]", "flip=off",
    "watermark=on", "alarmsensitivity=low", "power=on", "wdr=off",
    "night_mode=0", NULL
};

static void key_handler_restore(struct input_event *ev)
{
    int i;
    char cmd[128];

    log_printf(LOG_INFO, "time: %lu, Key: 0x%x, value: %d, %s",
               (unsigned long)ev->time.tv_sec, ev->code, ev->value,
               ev->value ? "pressed" : "released");

    if (!ev->value)
        return;                 /* ignore key release */

    log_printf(LOG_WARNING, "Reset key detected");

    /* clear bind/credentials state kept in nvram */
    for (i = 0; unset_keys_list[i]; i++) {
        snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram unset %s",
                 unset_keys_list[i]);
        run_cmd(cmd);
    }
    /* reset per-device defaults */
    for (i = 0; set_keyvalue_list[i]; i++) {
        snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram set %s",
                 set_keyvalue_list[i]);
        run_cmd(cmd);
    }

    /* led + ir-cut blink during reset */
    ledctl("0 0  1 0 0 2");
    proc_write("/sys/class/leds/IR/brightness", "255");
    proc_write("/sys/class/timed_output/ir-filter/direction", "1");
    proc_write("/sys/class/timed_output/ir-filter/enable", "200");
    voice_play(SOUND_RESET);

    /* persist the reset timestamp (write_num.txt, vendor util path) */
    {
        char stamp[32];
        int slen = snprintf(stamp, sizeof(stamp), "%lu",
                            (unsigned long)time(NULL));
        write_path("/mnt/media/mmcblk0p1/write_num/write_num.txt",
                   stamp, slen);
    }

    run_cmd("rm -rf " P2PID_DIR);
    kit_reboot();
}

/* ------------------------------------------------------------------ */
/* timers                                                              */
/* ------------------------------------------------------------------ */

static int timer_setup(struct itimerspec *its, int flags)
{
    kit.timer_fd = timerfd_create(CLOCK_MONOTONIC,
                                  (flags & O_NONBLOCK) ? TFD_NONBLOCK : 0);
    if (kit.timer_fd < 0)
        return -1;
    if (timerfd_settime(kit.timer_fd, 0, its, NULL) != 0)
        return -1;
    return 0;
}

static int timer_start(int fd, unsigned long long period_ms, int oneshot)
{
    struct itimerspec its;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = period_ms / 1000;
    its.it_value.tv_nsec = (period_ms % 1000) * 1000000;
    if (!oneshot) {
        its.it_interval.tv_sec = period_ms / 1000;
        its.it_interval.tv_nsec = (period_ms % 1000) * 1000000;
    }
    return timerfd_settime(fd, 0, &its, NULL);
}

/* housekeep: clear stale area records + re-arm next night */
static void timer_handler_housekeep(void)
{
    static const char *hk[] = {
        "housekeep-time", "housekeep-area-ptz-x", "housekeep-area-ptz-y",
        "housekeep-switch", "housekeep-tag"
    };
    char cmd[128];
    int i, id;
    unsigned long long expiry;
    struct tm *now;

    (void)read(kit.timer_fd, &expiry, sizeof(expiry));

    for (id = HPKE_KEY_OFFSET; id <= HPKE_KEY_OFFSET + 3; id++)
        for (i = 0; i < 5; i++) {
            snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram unset %s-%d",
                     hk[i], id);
            run_cmd(cmd);
        }
    run_cmd("/usr/sbin/nvram commit");

    /* day/night follow wall clock when no photodetector data */
    time_t tt = time(NULL);
    now = localtime(&tt);
    if (now) {
        if (now->tm_hour >= 18 || now->tm_hour < 6) {
            if (kit.current_mode != NIGHT_MODE)
                ircut_switch(NIGHT_MODE);
        } else if (kit.current_mode != LIGHT_MODE) {
            ircut_switch(LIGHT_MODE);
        }
    }

    /* re-arm for next hour boundary */
    timer_start(kit.timer_fd, 3600 * 1000ULL, 0);
}

static void timer_handler(void)
{
    unsigned long long exp;

    if (kit.timer_fd >= 0 && read(kit.timer_fd, &exp, sizeof(exp)) > 0)
        timer_handler_housekeep();
}

/* ------------------------------------------------------------------ */
/* local.status handler (biggest per-frame handler in the vendor)      */
/* ------------------------------------------------------------------ */

static void local_handler_status(const char *status)
{
    char cmd[256];
    char oper[64];
    char *light;
    FILE *f;
    int wdr;

    log_printf(LOG_INFO, "%s: otd status: %s", __func__, status);

    snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram set network_status=%s",
             status);
    run_cmd(cmd);

    /* wired carrier is a reliable ground truth for the LED phase */
    snprintf(cmd, sizeof(cmd), "cat %s", MLAN_OPERSTATE);
    f = popen(cmd, "r");
    if (f) {
        if (fgets(oper, sizeof(oper), f))
            log_printf(LOG_DEBUG, "wlan_operstate: %s", oper);
        pclose(f);
    }

    light = nvram_get("light");
    log_printf(LOG_INFO, "nvram setting: light: %s", light);

    if (strstr(status, "device_init") || !strcmp(status, "boot")) {
        ledctl("1 0  1 0 0 2");
    } else if (strstr(status, "ap_mode") || strstr(status, "wifi_ap_mode")) {
        ledctl("1 50 2 100 200 2");
    } else if (strstr(status, "wifi_connecting")) {
        ledctl("0 50 2 100 200 2");
    } else if (strstr(status, "wifi_connected")) {
        if (strcmp(light, "on") == 0)
            ledctl("0 50 2 200 800 2");
        else
            ledctl("0 50 2 200 800 2");     /* light off keeps dim blink */
    } else if (strstr(status, "cloud_trying") || strstr(status, "cloud_retry")) {
        ledctl("0 50 2 5000 1000 2");
    } else if (strstr(status, "cloud_connected")) {
        ledctl("1 0  1 0 0 1");
        if (strstr(status, "fiboot") == NULL)
            voice_play("/mnt/data/sound/binding_success.aac");
    } else if (strstr(status, "internet_failed")) {
        ledctl("0 50  0 0 0 1");
    } else if (strstr(status, "internet_connected") ||
               strstr(status, "sta_mode")) {
        ledctl("0 50 2 200 800 2");
    } else {
        ledctl("1 0  1 0 0 1");
    }

    wdr = (strcmp(nvram_get("wdr"), "on") == 0);
    if (wdr && kit.current_mode != WDR_MODE)
        ircut_switch(WDR_MODE);
}

/* ------------------------------------------------------------------ */
/* msg dispatcher                                                      */
/* ------------------------------------------------------------------ */

static int modify_restore_process(const char *msg)
{
    char idbuf[128];
    char rsp[320];
    struct msgs m;

    log_printf(LOG_INFO, "%s, msg: %s, strlen: %d, len: %d",
               __func__, msg, (int)strlen(msg), (int)strlen(msg));
    log_printf(LOG_WARNING, "Got miIO.restore...");

    if (json_verify_get_string(msg, "id", idbuf, sizeof(idbuf)) != 0)
        return -1;

    /* reply first, then reboot (kit_reboot() sleeps 5s and exits) */
    snprintf(rsp, sizeof(rsp),
             "{\"id\":%s,\"result\":[\"OK\"]}", idbuf);
    m.line = rsp;
    general_send_one(&m);
    kit_reboot();
    return 0;
}

static void reply_ok(const char *id, const char *good)
{
    char rsp[320];
    struct msgs m;
    if (!good) {
        snprintf(rsp, sizeof(rsp),
                 "{\"id\":%s,\"error\":{\"code\":-33020,"
                 "\"message\":\"miIO restore fail\"}}", id);
    } else {
        snprintf(rsp, sizeof(rsp), "{\"id\":%s,\"result\":[\"OK\"]}", id);
    }
    m.line = rsp;
    general_send_one(&m);
}

static void local_handler_time(const char *msg)
{
    int ts;
    struct timeval tv;

    log_printf(LOG_INFO, "Got local.time...");
    if (json_verify_get_int(msg, "params", &ts) != 0 &&
        json_verify_get_int(msg, "result", &ts) != 0)
        return;

    if (ts <= 0) {
        log_printf(LOG_INFO, "Ignore system time: %s", "0");
        return;
    }
    tv.tv_sec = ts;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) == 0) {
        time_t nowsec = ts;
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "%s", ctime(&nowsec));
        tbuf[strcspn(tbuf, "\n")] = '\0';
        log_printf(LOG_INFO, "Set system time success: %s", tbuf);
    } else {
        log_printf(LOG_INFO, "Set system time fail: %s", strerror(errno));
    }
    run_cmd("date;hwclock -w");
}

static void local_handler_shell(const char *msg)
{
    char cmd[400];
    char id[64] = "";

    log_printf(LOG_INFO, "Got shell cmd...");
    if (json_verify_get_string(msg, "params", cmd, sizeof(cmd)) != 0 &&
        json_verify_get_string(msg, "cmd", cmd, sizeof(cmd)) != 0)
        return;
    log_printf(LOG_INFO, "%s: shell cmd: %s", __func__, cmd);
    json_verify_get_string(msg, "id", id, sizeof(id));
    run_cmd(cmd);
    reply_ok(id, "OK");
}

static void otd_set_wdr_process(const char *msg)
{
    char id[64] = "";
    int want = 0;
    int cur;

    log_printf(LOG_INFO, "Got set_wdr...");
    log_printf(LOG_INFO, "msg: %s", msg);
    if (json_verify_get_int(msg, "params", &want) != 0)
        json_verify_method_value(msg, "set_wdr", "wdr", "1") == 0 ?
            (want = 1) : (want = 0);
    json_verify_get_string(msg, "id", id, sizeof(id));
    cur = (kit.current_mode == WDR_MODE);

    if (cur && want) {
        log_printf(LOG_INFO, "otd_set_wdr_process: WDR current is "
                   "already enabled");
    } else if (!cur && !want) {
        log_printf(LOG_INFO, "otd_set_wdr_process: WDR current is "
                   "already disabled");
    } else {
        EnableWDR(want);
        SaveWDRStatustoDisk(want);
        unset_motion_alarm_param();
    }
    reply_ok(id, "OK");
}

static void otd_power_process(const char *msg)
{
    char power[64] = "";
    char id[64] = "";

    if (json_verify_get_string(msg, "power", power, sizeof(power)) != 0)
        json_verify_get_string(msg, "params", power, sizeof(power));
    json_verify_get_string(msg, "id", id, sizeof(id));

    if (strcmp(power, "off") == 0) {
        log_printf(LOG_INFO, "otd_power_process: device_sleep_process");
        unset_motion_alarm_param();
        device_sleep_process();
    } else {
        log_printf(LOG_INFO, "otd_power_process: device_normal_process");
        device_normal_process();
    }
    reply_ok(id, "OK");
}

static void msg_dispatcher(struct msgs *msg)
{
    char *m;
    int rc;

    m = msg->line;
    log_printf(LOG_DEBUG, "%s, msg: %s, strlen: %d, len: %d",
               __func__, m, (int)strlen(m), (int)strlen(m));

    if (json_verify(m) != 0) {
        log_printf(LOG_WARNING, "%s: Not in json format: %s", __func__, m);
        return;
    }

    if ((rc = json_verify_method(m, "miIO.restore")) == 0) {
        modify_restore_process(m);
    } else if ((rc = json_verify_method(m, "local.time")) == 0) {
        local_handler_time(m);
    } else if ((rc = json_verify_method(m, "local.status")) == 0) {
        char st[256];
        if (json_verify_get_string(m, "params", st, sizeof(st)) == 0)
            local_handler_status(st);
        else
            log_printf(LOG_WARNING, "local.status fail");
    } else if ((rc = json_verify_method(m, "set_wdr")) == 0) {
        otd_set_wdr_process(m);
    } else if ((rc = json_verify_method(m, "miIO.shell")) == 0) {
        local_handler_shell(m);
    } else if ((rc = json_verify_method(m, "set_power")) == 0) {
        otd_power_process(m);
    } else if ((rc = json_verify_method(m, "local.shell")) == 0) {
        local_handler_shell(m);
    } else {
        log_printf(LOG_WARNING, "Unknown method");
    }
}

/* one connection -> process lines until EOF; returns when the sock is
 * closed (otd_recv_handler_block in the vendor is the blocking read,
 * recv_line_fd already waits for one line with a timeout) */
static int otd_recv_handler(int fd)
{
    char rbuf[2048];
    struct msgs msg;
    int rd;

    rd = recv_line_fd(fd, rbuf, sizeof(rbuf), OTD_LINE_TIMEOUT_MS);
    if (rd <= 0) {
        otd_close_retry(fd);
        return -1;
    }
    log_printf(LOG_DEBUG, "%s(), sockfd: %d, msg: %.*s, length: %d bytes",
               __func__, fd, rd, rbuf, rd);
    msg.line = rbuf;
    msg.id = 0;
    json_verify_get_int(rbuf, "id", &msg.id);
    msg_dispatcher(&msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* device power / sys status                                           */
/* ------------------------------------------------------------------ */

static void device_get_sys_status(void)
{
    char ip[64];
    char uptime[64];

    get_ip_str(ip, sizeof(ip));
    if (get_uptime(uptime, sizeof(uptime)) == 0)
        log_printf(LOG_INFO, "sys: ip=%s uptime=%ss", ip, uptime);
}

static void device_normal_process(void)
{
    char *power;
    static int adc_fd = -1;

    power = nvram_get("power");
    if (strcmp(power, "off") == 0) {
        device_sleep_process();
        return;
    }
    if (adc_fd < 0)
        adc_fd = open(ISP_DEV, O_RDONLY | O_NONBLOCK);
    if (adc_fd >= 0)
        adc_daynight_check(adc_fd);
}

static void device_sleep_process(void)
{
    /* vendor body is a single return (sz=4); sleep is applied by the
     * OS power hook, guard code only. */
}

/* ------------------------------------------------------------------ */
/* signals                                                             */
/* ------------------------------------------------------------------ */

static void sig_handler(int sig)
{
    (void)sig;
    log_printf(LOG_INFO, "miot_devicekit says goodbye!");
    run_cmd(LOGTF_SCRIPT);
    exit(0);
}

/* ------------------------------------------------------------------ */
/* hid / boot checks                                                   */
/* ------------------------------------------------------------------ */

static int hid_check(void)
{
    char rootfs_hid[128] = "";
    char nvram_hid[128] = "";
    FILE *f;

    f = popen("cat /etc/hid", "r");
    if (f) {
        if (fgets(rootfs_hid, sizeof(rootfs_hid), f))
            rootfs_hid[strcspn(rootfs_hid, "\r\n")] = '\0';
        pclose(f);
    }
    log_printf(LOG_DEBUG, "rootfs hid:%s", rootfs_hid);

    f = popen("/usr/sbin/nvram factory get hid", "r");
    if (f) {
        if (fgets(nvram_hid, sizeof(nvram_hid), f))
            nvram_hid[strcspn(nvram_hid, "\r\n")] = '\0';
        pclose(f);
    }
    log_printf(LOG_DEBUG, "nvram get hid:%s", nvram_hid);

    if (strlen(nvram_hid) == 0 && strlen(rootfs_hid)) {
        /* factory nvram has no hid yet: commit the rootfs one so a
         * wiped factory area can be re-provisioned (iImi variant). */
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "/usr/sbin/nvram set hid=%s",
                 rootfs_hid);
        run_cmd(cmd);
        log_printf(LOG_ERROR, "check hid error:%s, exit !", rootfs_hid);
        exit(1);
    }
    log_printf(LOG_INFO, "check hid ok:%s !", nvram_hid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static const char *version_string = "miot-devicekit 1.0.0";

static void print_version(void)
{
    printf("miot_devictkit - Mi IOT device kit\n");
    printf("Copyright (C) 2016 Xiaomi\n");
    printf("Author: Yin Kangkai <yinkangkai@xiaomi.com>\n");
    printf("Version: %s\n", version_string);
    printf("Build time: 14:13:04 Jun 12 2018\n");
}

static void print_usage(const char *prog)
{
    printf("Usage: %s\n"
           "[-D --daemonize]\n"
           "[-m --ftmode=<mode>] set factory mode.\n"
           "[-l --loglevel=<level>] set loglevel (0-3), bigger = more verbose\n"
           "[-L --logfile=file] output log into file instead of stdout\n"
           "[-t --timestamp] timestamp log lines\n"
           "[-h --help]\n", prog);
}

int main(int argc, char *argv[])
{
    static const struct option long_options[] = {
        { "help",       no_argument,       NULL, 'h' },
        { "version",    no_argument,       NULL, 'v' },
        { "loglevel",   required_argument, NULL, 'l' },
        { "logfile",    required_argument, NULL, 'L' },
        { "daemonize",  no_argument,       NULL, 'D' },
        { "ftmode",     required_argument, NULL, 'm' },
        { "timestamp",  no_argument,       NULL, 't' },
        { NULL, 0, NULL, 0 }
    };
    struct itimerspec hs_its;
    struct itimerspec fts;
    int c;

    kit.otd_sock = -1;
    kit.ft_timer_fd = -1;
    kit.key_fd = -1;
    kit.timer_fd = -1;
    kit.port = get_port();

    while ((c = getopt_long(argc, argv, "hvl:L:Dm:t", long_options,
                            NULL)) != -1) {
        switch (c) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'v':
            print_version();
            return 0;
        case 'l':
            g_loglevel = atoi(optarg);
            log_printf(LOG_INFO, "Set log level to: %d", g_loglevel);
            break;
        case 'L':
            logfile_init(optarg);
            break;
        case 'D':
            if (daemon(0, 0) != 0) {
                log_printf(LOG_ERROR, "daemonize fail: %m");
                return 1;
            }
            break;
        case 'm':
            kit.ftmode = atoi(optarg);
            break;
        case 't':
            g_logtimestamp = 1;
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    generate_random_id((unsigned)(time(NULL) ^ getpid()));
    print_version();

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    hid_check();

    /* new HW has a NTC on the board -> ambient-light auto day/night */
    if (access("/dev/mtd6", F_OK) == 0)
        log_printf(LOG_INFO,
                   "This is new HW, a NTC is mounted on the board...");

    device_get_sys_status();

    /* boot behaviour */
    run_cmd("rm -rf " P2PID_DIR);
    ledctl("0 0  1 0 0 2");
    voice_play(SOUND_BOOT);

    /* initial ISP mode from nvram */
    kit.current_mode = LIGHT_MODE;
    if (strcmp(nvram_get("wdr"), "on") == 0)
        ircut_switch(WDR_MODE);

    /* housekeeping timer (cleans housekeep-* keys, re-arms hourly) */
    memset(&hs_its, 0, sizeof(hs_its));
    hs_its.it_value.tv_sec = 3600;
    hs_its.it_interval.tv_sec = 3600;
    if (timer_setup(&hs_its, O_NONBLOCK) != 0) {
        log_printf(LOG_ERROR, "timer_setup() fail, quit.");
        exit(1);
    }
    log_printf(LOG_INFO, "One shot timer fd: %d", kit.timer_fd);

    /* factory mode timer: wipe binding leftovers every minute */
    memset(&fts, 0, sizeof(fts));
    fts.it_value.tv_sec = 60;
    fts.it_interval.tv_sec = 60;
    if (kit.ftmode) {
        kit.ft_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (kit.ft_timer_fd < 0 ||
            timerfd_settime(kit.ft_timer_fd, 0, &fts, NULL) != 0) {
            log_printf(LOG_ERROR, "timer_setup(ftm) fail, quit.");
            exit(1);
        }
        log_printf(LOG_INFO, "ftm Timer fd: %d, interval: %d ms",
                   kit.ft_timer_fd, 60000);
    }

    /* reset button */
    kit.key_fd = open(KEY_EVENT_DEV, O_RDONLY | O_NONBLOCK);
    if (kit.key_fd < 0) {
        log_printf(LOG_ERROR, "key open error(%s): %m", KEY_EVENT_DEV);
        log_printf(LOG_ERROR, "key_init() fail: %m");
    } else {
        log_printf(LOG_INFO, "key fd: %d", kit.key_fd);
    }

    static int time_synced = 0;

    for (;;) {
        struct pollfd tmp[6];
        int nfd = 0;
        int i;
        int rv;

        /* (re)establish the OTD line to miio_client when down */
        if (kit.otd_sock < 0) {
            otd_reconnect();
            if (kit.otd_sock >= 0 && !time_synced) {
                time_sync();
                time_synced = 1;
            }
            if (kit.otd_sock < 0) {
                sleep(OTD_RECONNECT_DELAY_MS / 1000);
                continue;
            }
        }

        tmp[nfd].fd = kit.otd_sock;
        tmp[nfd].events = POLLIN;
        nfd++;
        if (kit.timer_fd >= 0) {
            tmp[nfd].fd = kit.timer_fd;
            tmp[nfd].events = POLLIN;
            nfd++;
        }
        if (kit.ft_timer_fd >= 0) {
            tmp[nfd].fd = kit.ft_timer_fd;
            tmp[nfd].events = POLLIN;
            nfd++;
        }
        if (kit.key_fd >= 0) {
            tmp[nfd].fd = kit.key_fd;
            tmp[nfd].events = POLLIN;
            nfd++;
        }

        rv = poll(tmp, nfd, 5000);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            log_printf(LOG_ERROR, "poll");
            sleep(1);
            continue;
        }
        if (rv == 0) {
            device_normal_process();
            continue;
        }

        for (i = 0; i < nfd; i++) {
            if (tmp[i].revents & (POLLNVAL | POLLHUP | POLLERR)) {
                log_printf(LOG_WARNING,
                           "POLLNVAL | POLLHUP | POLLERR fd: pollfds[%d]: "
                           "%d, revents: 0x%08x",
                           i, tmp[i].fd, tmp[i].revents);
                if (tmp[i].fd == kit.otd_sock)
                    otd_close_retry(kit.otd_sock);
                continue;
            }
            if (!(tmp[i].revents & POLLIN))
                continue;
            if (tmp[i].fd == kit.otd_sock) {
                otd_recv_handler(kit.otd_sock);
            } else if (kit.timer_fd >= 0 && tmp[i].fd == kit.timer_fd) {
                timer_handler();
            } else if (kit.ft_timer_fd >= 0 &&
                       tmp[i].fd == kit.ft_timer_fd) {
                unsigned long long exp;
                if (read(kit.ft_timer_fd, &exp, sizeof(exp)) > 0) {
                    run_cmd("rm -rf " P2PID_DIR);
                }
            } else if (kit.key_fd >= 0 && tmp[i].fd == kit.key_fd) {
                struct input_event ev;
                if (read(kit.key_fd, &ev, sizeof(ev)) == (int)sizeof(ev))
                    key_handler_restore(&ev);
            }
        }

        device_normal_process();
        if (time(NULL) % 300 == 0)
            otd_status_check();
    }

    return 0;
}