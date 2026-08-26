/*
 * libchuangmi_sensor - gmlib capture/sensor feature wrapper
 *
 * NOTE: gm_init()/gm_release() are process-global. Do not mix this library
 * with libchuangmi_codec in the same process unless one side only consumes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chuangmi_sensor.h"

#define MD_BUF_MAX  CAP_MOTION_SIZE

static int sensor_initialized;
static int gm_owned;
static sensor_notify_cb notify_cbs[MAX_GM_NOTIFY_COUNT];
static void *notify_users[MAX_GM_NOTIFY_COUNT];

unsigned int sensor_chipid(void)
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

const gm_cap_sys_info_t *sensor_get_cap_info(int vch)
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

int sensor_is_initialized(void)
{
    return sensor_initialized ? 0 : -1;
}

int sensor_init(void)
{
    if (sensor_initialized)
        return 0;

    if (gm_init() < 0) {
        fprintf(stderr, "*** Error: gm_init failed\n");
        return -1;
    }
    gm_owned = 1;
    sensor_initialized = 1;
    return 0;
}

int sensor_end(void)
{
    if (!sensor_initialized)
        return -1;

    if (gm_owned) {
        gm_release();
        gm_owned = 0;
    }
    sensor_initialized = 0;
    return 0;
}

/* gm_cap_flip_t has no gm_priv_t member: plain struct, no DECLARE_ATTR */
int sensor_cap_flip_set(int h_flip, int v_flip)
{
    gm_cap_flip_t flip;

    if (!sensor_initialized) {
        fprintf(stderr, "*** Error: sensor library is uninitialized\n");
        return -1;
    }

    memset(&flip, 0, sizeof(flip));
    flip.h_flip_enabled = h_flip ? 1 : 0;
    flip.v_flip_enabled = v_flip ? 1 : 0;
    return gm_set_cap_flip(0, &flip);
}

int sensor_tamper_set(int sensitive_b, int threshold, int sensitive_h)
{
    gm_cap_tamper_t tamper;

    if (!sensor_initialized) {
        fprintf(stderr, "*** Error: sensor library is uninitialized\n");
        return -1;
    }
    if (threshold < 1 || threshold > 255) {
        fprintf(stderr, "*** Error: tamper threshold must be 1-255\n");
        return -1;
    }

    memset(&tamper, 0, sizeof(tamper));
    tamper.tamper_sensitive_b = sensitive_b < 0 ? 0 : sensitive_b;
    tamper.tamper_sensitive_h = sensitive_h < 0 ? 0 : sensitive_h;
    tamper.tamper_threshold = threshold;
    return gm_set_cap_tamper(0, &tamper);
}

int sensor_tamper_disable(void)
{
    return sensor_tamper_set(0, 128, 0);
}

int sensor_motion_param_set(unsigned int id, unsigned int value)
{
    gm_cap_motion_t motion;

    if (!sensor_initialized) {
        fprintf(stderr, "*** Error: sensor library is uninitialized\n");
        return -1;
    }

    memset(&motion, 0, sizeof(motion));
    motion.id = id;
    motion.value = value;
    return gm_set_cap_motion(0, &motion);
}

static void sensor_notify_trampoline(gm_obj_type_t obj_type, int vch, gm_notify_t notify)
{
    if (notify >= 0 && notify < MAX_GM_NOTIFY_COUNT && notify_cbs[notify])
        notify_cbs[notify]((int)obj_type, vch, (int)notify, notify_users[notify]);
}

int sensor_notify_register(int notify, sensor_notify_cb cb, void *user)
{
    if (!sensor_initialized) {
        fprintf(stderr, "*** Error: sensor library is uninitialized\n");
        return -1;
    }
    if (notify < 0 || notify >= MAX_GM_NOTIFY_COUNT)
        return -1;

    if (gm_register_notify_handler(notify, cb ? sensor_notify_trampoline : NULL) < 0)
        return -1;

    notify_cbs[notify] = cb;
    notify_users[notify] = user;
    return 0;
}

int sensor_notify_unregister(int notify)
{
    return sensor_notify_register(notify, NULL, NULL);
}

int sensor_md_poll(void *bindfd, char *md_buf, int md_buf_len)
{
    gm_multi_cap_md_t multi[1];

    if (!sensor_initialized || !bindfd || !md_buf || md_buf_len <= 0)
        return -1;

    memset(multi, 0, sizeof(multi));
    multi[0].bindfd = bindfd;
    multi[0].cap_md_info.md_buf = md_buf;
    multi[0].cap_md_info.md_buf_len = md_buf_len > MD_BUF_MAX ? MD_BUF_MAX : md_buf_len;

    if (gm_recv_multi_cap_md(multi, 1) < 0)
        return -1;

    if (!multi[0].cap_md_info.is_valid)
        return 0;

    return multi[0].cap_md_info.md_len;
}
