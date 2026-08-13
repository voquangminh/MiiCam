#define _GNU_SOURCE
#include "onvif_hw_image_led.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ISP_COMMAND "/proc/isp328/command"
#define IRCUT_STATE "/var/run/ircut"

/*
 * Override these at build time when dump_cfg confirms different ISP names:
 *   -DISP_VAR_BRIGHTNESS='"brightness"'
 */
#ifndef ISP_VAR_BRIGHTNESS
#define ISP_VAR_BRIGHTNESS "brightness"
#endif
#ifndef ISP_VAR_CONTRAST
#define ISP_VAR_CONTRAST "contrast"
#endif
#ifndef ISP_VAR_SATURATION
#define ISP_VAR_SATURATION "saturation"
#endif
#ifndef ISP_VAR_SHARPNESS
#define ISP_VAR_SHARPNESS "sharpness"
#endif

static pthread_mutex_t isp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gpio_mutex = PTHREAD_MUTEX_INITIALIZER;

static int write_text(const char *path, const char *text)
{
    int fd, rc = 0;
    size_t len = strlen(text), off = 0;
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        off += (size_t)n;
    }
    if (close(fd) < 0 && rc == 0) rc = -1;
    return rc;
}

static int read_text(const char *path, char *buf, size_t size)
{
    int fd;
    ssize_t n;
    if (!buf || size < 2) { errno = EINVAL; return -1; }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    do n = read(fd, buf, size - 1); while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

static int extract_last_integer(const char *s, int *value)
{
    const char *p = s;
    char *end;
    long found = 0;
    int have = 0;
    while (*p) {
        if (*p == '-' || isdigit((unsigned char)*p)) {
            errno = 0;
            long v = strtol(p, &end, 0);
            if (end != p && errno == 0) {
                found = v;
                have = 1;
                p = end;
                continue;
            }
        }
        p++;
    }
    if (!have) { errno = EPROTO; return -1; }
    *value = (int)found;
    return 0;
}

int hw_isp_get(const char *variable, int *value)
{
    char command[160], reply[512];
    int rc = -1;
    if (!variable || !*variable || !value) { errno = EINVAL; return -1; }
    for (const char *p = variable; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.')) {
            errno = EINVAL;
            return -1;
        }
    }
    if (snprintf(command, sizeof(command), "r %s\n", variable) >= (int)sizeof(command)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    pthread_mutex_lock(&isp_mutex);
    if (write_text(ISP_COMMAND, command) == 0 &&
        read_text(ISP_COMMAND, reply, sizeof(reply)) == 0)
        rc = extract_last_integer(reply, value);
    pthread_mutex_unlock(&isp_mutex);
    return rc;
}

int hw_isp_set(const char *variable, int value)
{
    char command[160];
    int rc;
    if (!variable || !*variable) { errno = EINVAL; return -1; }
    for (const char *p = variable; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.')) {
            errno = EINVAL;
            return -1;
        }
    }
    if (snprintf(command, sizeof(command), "w %s %d\n", variable, value) >= (int)sizeof(command)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    pthread_mutex_lock(&isp_mutex);
    rc = write_text(ISP_COMMAND, command);
    pthread_mutex_unlock(&isp_mutex);
    return rc;
}

static int led_write(const char *name, int on)
{
    char path[160], value[4];
    const char *roots[] = {
        "/sys/class/leds",
        "/sys/devices/platform/leds-gpio/leds"
    };
    snprintf(value, sizeof(value), "%d\n", on ? 1 : 0);
    for (unsigned int i = 0; i < sizeof(roots)/sizeof(roots[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/brightness", roots[i], name);
        if (write_text(path, value) == 0) return 0;
    }
    return -1;
}

int hw_blue_led_set(int mode)
{
    if (mode != 0 && mode != 1) { errno = ENOTSUP; return -1; }
    return led_write("BLUE", mode);
}

int hw_yellow_led_set(int mode)
{
    if (mode != 0 && mode != 1) { errno = ENOTSUP; return -1; }
    /* Firmware exposes the yellow indicator through the RED GPIO LED node. */
    return led_write("RED", mode);
}

static int gpio_write(int pin, int value)
{
    char path[128], text[4];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    snprintf(text, sizeof(text), "%d\n", value ? 1 : 0);
    return write_text(path, text);
}

static int gpio_read(int pin, int *value)
{
    char path[128], text[32];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    if (read_text(path, text, sizeof(text)) < 0) return -1;
    *value = atoi(text) ? 1 : 0;
    return 0;
}

int hw_ir_cut_set(int enabled)
{
    int rc;
    char state[4];
    enabled = enabled ? 1 : 0;
    pthread_mutex_lock(&gpio_mutex);
    if (enabled) {
        rc = gpio_write(14, 1);
        if (rc == 0) rc = gpio_write(15, 0);
    } else {
        rc = gpio_write(14, 0);
        if (rc == 0) rc = gpio_write(15, 1);
    }
    if (rc == 0) {
        snprintf(state, sizeof(state), "%d\n", enabled);
        rc = write_text(IRCUT_STATE, state);
    }
    pthread_mutex_unlock(&gpio_mutex);
    return rc;
}

int hw_ir_cut_get(int *enabled)
{
    char state[32];
    int value;
    if (!enabled) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&gpio_mutex);
    if (read_text(IRCUT_STATE, state, sizeof(state)) == 0)
        value = atoi(state) ? 1 : 0;
    else if (gpio_read(14, &value) < 0) {
        pthread_mutex_unlock(&gpio_mutex);
        return -1;
    }
    pthread_mutex_unlock(&gpio_mutex);
    *enabled = value;
    return 0;
}

int hw_image_get(onvif_image_settings_t *s)
{
    int v, rc = 0;
    if (!s) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof(*s));
    if (hw_isp_get(ISP_VAR_BRIGHTNESS, &v) == 0) s->brightness = (float)v; else rc = -1;
    if (hw_isp_get(ISP_VAR_CONTRAST, &v) == 0) s->contrast = (float)v; else rc = -1;
    if (hw_isp_get(ISP_VAR_SATURATION, &v) == 0) s->saturation = (float)v; else rc = -1;
    if (hw_isp_get(ISP_VAR_SHARPNESS, &v) == 0) s->sharpness = (float)v; else rc = -1;
    if (hw_ir_cut_get(&s->ir_cut) < 0) rc = -1;
    return rc;
}

int hw_image_set(const onvif_image_settings_t *s, unsigned int mask)
{
    int rc = 0;
    if (!s) { errno = EINVAL; return -1; }
    if ((mask & HW_IMAGE_BRIGHTNESS) && hw_isp_set(ISP_VAR_BRIGHTNESS, (int)s->brightness) < 0) rc = -1;
    if ((mask & HW_IMAGE_CONTRAST) && hw_isp_set(ISP_VAR_CONTRAST, (int)s->contrast) < 0) rc = -1;
    if ((mask & HW_IMAGE_SATURATION) && hw_isp_set(ISP_VAR_SATURATION, (int)s->saturation) < 0) rc = -1;
    if ((mask & HW_IMAGE_SHARPNESS) && hw_isp_set(ISP_VAR_SHARPNESS, (int)s->sharpness) < 0) rc = -1;
    if ((mask & HW_IMAGE_IRCUT) && hw_ir_cut_set(s->ir_cut) < 0) rc = -1;
    return rc;
}
