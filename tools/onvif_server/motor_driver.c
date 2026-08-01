#include "motor_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MOTOR_MAGIC 'M'

/* Recovered from motor.ko::motor_ioctl. The vendor used _IOW for getters too. */
#define MOTOR_H_DIR_SET       _IOW(MOTOR_MAGIC,  3, int)
#define MOTOR_H_DIST_SET      _IOW(MOTOR_MAGIC,  4, int)
#define MOTOR_H_COORD_GET     _IOW(MOTOR_MAGIC,  5, int)
#define MOTOR_H_COORD_SET     _IOW(MOTOR_MAGIC,  6, int)
#define MOTOR_H_SPEED_SET     _IOW(MOTOR_MAGIC,  7, int)
#define MOTOR_V_DIR_SET       _IOW(MOTOR_MAGIC, 23, int)
#define MOTOR_V_DIST_SET      _IOW(MOTOR_MAGIC, 24, int)
#define MOTOR_V_COORD_GET     _IOW(MOTOR_MAGIC, 25, int)
#define MOTOR_V_COORD_SET     _IOW(MOTOR_MAGIC, 26, int)
#define MOTOR_V_SPEED_SET     _IOW(MOTOR_MAGIC, 27, int)

#define MOTOR_X_MIN 0
#define MOTOR_X_MAX 31
#define MOTOR_Y_MIN 0
#define MOTOR_Y_MAX 15

static int motor_fd = -1;
static pthread_mutex_t motor_mutex = PTHREAD_MUTEX_INITIALIZER;

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int do_ioctl(unsigned long request, int *value)
{
    int rc;
    do {
        rc = ioctl(motor_fd, request, value);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

int gm8136_motor_open(const char *device)
{
    int fd;
    if (!device) device = "/dev/motor";

    pthread_mutex_lock(&motor_mutex);
    if (motor_fd >= 0) {
        pthread_mutex_unlock(&motor_mutex);
        return 0;
    }

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        pthread_mutex_unlock(&motor_mutex);
        return -1;
    }
    motor_fd = fd;
    pthread_mutex_unlock(&motor_mutex);
    return 0;
}

void gm8136_motor_close(void)
{
    pthread_mutex_lock(&motor_mutex);
    if (motor_fd >= 0) close(motor_fd);
    motor_fd = -1;
    pthread_mutex_unlock(&motor_mutex);
}

int gm8136_motor_get_status(gm8136_motor_status_t *status)
{
    int x = 0, y = 0;
    if (!status) { errno = EINVAL; return -1; }

    pthread_mutex_lock(&motor_mutex);
    if (motor_fd < 0) { pthread_mutex_unlock(&motor_mutex); errno = ENODEV; return -1; }
    if (do_ioctl(MOTOR_H_COORD_GET, &x) < 0 ||
        do_ioctl(MOTOR_V_COORD_GET, &y) < 0) {
        pthread_mutex_unlock(&motor_mutex);
        return -1;
    }
    pthread_mutex_unlock(&motor_mutex);

    status->horizontal = clamp_int(x, MOTOR_X_MIN, MOTOR_X_MAX);
    status->vertical = clamp_int(y, MOTOR_Y_MIN, MOTOR_Y_MAX);
    return 0;
}

static int move_axis(unsigned long dir_request,
                     unsigned long dist_request,
                     int delta)
{
    int direction, distance;
    if (delta == 0) return 0;
    direction = delta > 0 ? 1 : 0;
    distance = delta > 0 ? delta : -delta;
    if (do_ioctl(dir_request, &direction) < 0) return -1;
    if (do_ioctl(dist_request, &distance) < 0) return -1;
    return 0;
}

int gm8136_motor_move_relative(int dx, int dy)
{
    gm8136_motor_status_t s;
    int tx, ty, actual_dx, actual_dy, rc = 0;

    pthread_mutex_lock(&motor_mutex);
    if (motor_fd < 0) { pthread_mutex_unlock(&motor_mutex); errno = ENODEV; return -1; }

    if (do_ioctl(MOTOR_H_COORD_GET, &s.horizontal) < 0 ||
        do_ioctl(MOTOR_V_COORD_GET, &s.vertical) < 0) {
        pthread_mutex_unlock(&motor_mutex);
        return -1;
    }

    tx = clamp_int(s.horizontal + dx, MOTOR_X_MIN, MOTOR_X_MAX);
    ty = clamp_int(s.vertical + dy, MOTOR_Y_MIN, MOTOR_Y_MAX);
    actual_dx = tx - s.horizontal;
    actual_dy = ty - s.vertical;

    if (move_axis(MOTOR_H_DIR_SET, MOTOR_H_DIST_SET, actual_dx) < 0)
        rc = -1;
    if (rc == 0 && move_axis(MOTOR_V_DIR_SET, MOTOR_V_DIST_SET, actual_dy) < 0)
        rc = -1;

    pthread_mutex_unlock(&motor_mutex);
    return rc;
}

int gm8136_motor_move_absolute(int x, int y)
{
    gm8136_motor_status_t s;
    if (gm8136_motor_get_status(&s) < 0) return -1;
    x = clamp_int(x, MOTOR_X_MIN, MOTOR_X_MAX);
    y = clamp_int(y, MOTOR_Y_MIN, MOTOR_Y_MAX);
    return gm8136_motor_move_relative(x - s.horizontal, y - s.vertical);
}

int gm8136_motor_set_logical_position(int x, int y)
{
    x = clamp_int(x, MOTOR_X_MIN, MOTOR_X_MAX);
    y = clamp_int(y, MOTOR_Y_MIN, MOTOR_Y_MAX);

    pthread_mutex_lock(&motor_mutex);
    if (motor_fd < 0) { pthread_mutex_unlock(&motor_mutex); errno = ENODEV; return -1; }
    if (do_ioctl(MOTOR_H_COORD_SET, &x) < 0 ||
        do_ioctl(MOTOR_V_COORD_SET, &y) < 0) {
        pthread_mutex_unlock(&motor_mutex);
        return -1;
    }
    pthread_mutex_unlock(&motor_mutex);
    return 0;
}
