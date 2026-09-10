/**
 * tracking.c - Motion tracking daemon
 *
 * Reads motion centroid from /dev/shm/rtspd_tracking (written by rtspd)
 * and controls PTZ motor to follow the detected motion.
 *
 * Architecture: separate process from rtspd to minimize CPU coupling.
 * rtspd writes centroid -> tracking daemon reads -> controls motor.
 *
 * Usage: tracking [-d deadzone] [-s speed] [-z] [-v]
 *   -d N   deadzone: macroblocks from center to ignore (default 2)
 *   -s N   max steps per move 1-10 (default 3)
 *   -z     auto return-to-home after timeout
 *   -v     verbose logging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define MOTOR_DEVICE        "/dev/motor"
#define PWM_DEVICE          "/dev/ftpwmtmr010"
#define TRACKING_FILE       "/dev/shm/rtspd_tracking"
#define TRACKING_STATE      "/dev/shm/rtspd_tracking_state"
#define PIDFILE             "/var/run/tracking.pid"

#define TRACKING_SCRIPT     "/tmp/sd/firmware/scripts/tracking.sh"

#define MOTOR_MAGIC     'M'
#define H_DIR_SET       _IOW(MOTOR_MAGIC,  3, int)
#define H_DIST_SET      _IOW(MOTOR_MAGIC,  4, int)
#define H_COORD_GET     _IOW(MOTOR_MAGIC,  5, int)
#define V_DIR_SET       _IOW(MOTOR_MAGIC, 23, int)
#define V_DIST_SET      _IOW(MOTOR_MAGIC, 24, int)
#define V_COORD_GET     _IOW(MOTOR_MAGIC, 25, int)

#define PWM_IOCTL_01    0x40047001UL
#define PWM_IOCTL_02    0x40047002UL
#define PWM_IOCTL_05    0x40307005UL
#define PWM_IOCTL_06    0x40307006UL
#define PWM_IOCTL_07    0x40307007UL
#define PWM_IOCTL_09    0x40307009UL
#define PWM_IOCTL_0E    0x4004700eUL

#define X_MAX   31
#define Y_MAX   15
#define X_CENTER (X_MAX / 2)
#define Y_CENTER (Y_MAX / 2)

#define POLL_INTERVAL_MS    500
#define MOVE_COOLDOWN_MS    400
#define HOME_TIMEOUT_SEC    10

typedef struct { uint32_t value[12]; } pwm_config_t;

static volatile int running = 1;
static int motor_fd = -1;
static int pwm_fd = -1;
static int cur_x = 15, cur_y = 7;
static int deadzone = 2;
static int max_speed = 3;
static int return_home = 0;
static int verbose = 0;
static time_t last_motion_time = 0;
static int tracking_active = 0;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

static int motor_ioctl(unsigned long cmd, int *val)
{
    int rc;
    do { rc = ioctl(motor_fd, cmd, val); } while (rc < 0 && errno == EINTR);
    return rc;
}

static int pwm_init(void)
{
    pwm_config_t config[2];
    int channel, rc;

    memset(config, 0, sizeof(config));
    config[0].value[0]  = 0;
    config[0].value[1]  = 1;
    config[0].value[2]  = 1;
    config[0].value[3]  = 0;
    config[0].value[4]  = 255;
    config[0].value[5]  = 127;
    config[0].value[10] = 1;
    config[0].value[11] = 127;

    memcpy(&config[1], &config[0], sizeof(config[0]));
    config[1].value[0] = 1;

    pwm_fd = open(PWM_DEVICE, O_RDWR);
    if (pwm_fd < 0) return -1;

    for (channel = 0; channel < 2; channel++) {
        rc  = ioctl(pwm_fd, PWM_IOCTL_01, &config[channel]);
        rc |= ioctl(pwm_fd, PWM_IOCTL_05, &config[channel]);
        rc |= ioctl(pwm_fd, PWM_IOCTL_09, &config[channel]);
        rc |= ioctl(pwm_fd, PWM_IOCTL_0E, &config[channel]);
        rc |= ioctl(pwm_fd, PWM_IOCTL_07, &config[channel]);
        if (rc < 0) goto fail;
    }

    config[1].value[3] = 15000000U;
    rc  = ioctl(pwm_fd, PWM_IOCTL_06, &config[1]);
    rc |= ioctl(pwm_fd, PWM_IOCTL_0E, &config[1]);
    rc |= ioctl(pwm_fd, PWM_IOCTL_02, &config[1]);
    if (rc < 0) goto fail;

    return 0;
fail:
    close(pwm_fd);
    pwm_fd = -1;
    return -1;
}

static int motor_open(void)
{
    if (pwm_fd < 0 && pwm_init() < 0)
        fprintf(stderr, "WARN: PWM init failed\n");

    motor_fd = open(MOTOR_DEVICE, O_RDWR);
    if (motor_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", MOTOR_DEVICE, strerror(errno));
        return -1;
    }
    return 0;
}

static void motor_close(void)
{
    if (motor_fd >= 0) { close(motor_fd); motor_fd = -1; }
    if (pwm_fd >= 0)   { close(pwm_fd);   pwm_fd = -1;   }
}

static int motor_move(int dx, int dy)
{
    int dir, dist, rc = 0;

    if (cur_x + dx < 0) dx = -cur_x;
    if (cur_x + dx > X_MAX) dx = X_MAX - cur_x;
    if (cur_y + dy < 0) dy = -cur_y;
    if (cur_y + dy > Y_MAX) dy = Y_MAX - cur_y;

    if (dx == 0 && dy == 0) return 0;

    if (dx != 0) {
        dir = dx > 0 ? 0 : 1;
        dist = dx > 0 ? dx : -dx;
        rc |= motor_ioctl(H_DIR_SET, &dir);
        rc |= motor_ioctl(H_DIST_SET, &dist);
    }
    if (dy != 0) {
        dir = dy > 0 ? 1 : 0;
        dist = dy > 0 ? dy : -dy;
        rc |= motor_ioctl(V_DIR_SET, &dir);
        rc |= motor_ioctl(V_DIST_SET, &dist);
    }
    if (rc < 0) return -1;

    usleep(300000);
    cur_x += dx;
    cur_y += dy;
    return 0;
}

static void write_state(const char *state)
{
    FILE *f = fopen(TRACKING_STATE, "w");
    if (f) {
        fprintf(f, "%s %d %d\n", state, cur_x, cur_y);
        fclose(f);
    }
}

static void run_script(const char *arg)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s %s &", TRACKING_SCRIPT, arg);
    system(cmd);
}

static int read_tracking_file(int *col, int *row, int *mb_w, int *mb_h, int *count)
{
    FILE *f = fopen(TRACKING_FILE, "r");
    if (!f) return -1;
    int n = fscanf(f, "%d %d %d %d %d", col, row, mb_w, mb_h, count);
    fclose(f);
    return (n == 5) ? 0 : -1;
}

static void write_pid(void)
{
    FILE *f = fopen(PIDFILE, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
}

int main(int argc, char *argv[])
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            deadzone = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            max_speed = atoi(argv[++i]);
        else if (strcmp(argv[i], "-z") == 0)
            return_home = 1;
        else if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else {
            fprintf(stderr, "Usage: tracking [-d deadzone] [-s speed] [-z] [-v]\n");
            return 1;
        }
    }

    if (deadzone < 0) deadzone = 2;
    if (max_speed < 1 || max_speed > 10) max_speed = 3;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    if (motor_open() < 0)
        return 1;

    write_pid();
    write_state("active");
    run_script("on");
    tracking_active = 1;

    if (verbose)
        fprintf(stderr, "tracking: started (deadzone=%d speed=%d)\n", deadzone, max_speed);

    while (running) {
        int col, row, mb_w, mb_h, count;

        if (read_tracking_file(&col, &row, &mb_w, &mb_h, &count) < 0) {
            /* No tracking file = no motion */
            if (tracking_active) {
                tracking_active = 0;
                if (verbose)
                    fprintf(stderr, "tracking: no motion\n");
            }
            if (return_home && last_motion_time > 0
                && (time(NULL) - last_motion_time) >= HOME_TIMEOUT_SEC
                && (cur_x != X_CENTER || cur_y != Y_CENTER)) {
                int dx = X_CENTER - cur_x;
                int dy = Y_CENTER - cur_y;
                last_motion_time = 0;
                if (verbose)
                    fprintf(stderr, "tracking: return home dx=%d dy=%d\n", dx, dy);
                motor_move(dx, dy);
            }
            usleep(POLL_INTERVAL_MS * 1000);
            continue;
        }

        last_motion_time = time(NULL);
        if (!tracking_active) {
            tracking_active = 1;
            write_state("tracking");
            if (verbose)
                fprintf(stderr, "tracking: motion detected at mb(%d,%d) grid=%dx%d count=%d\n",
                        col, row, mb_w, mb_h, count);
        }

        /* Map macroblock grid to motor coordinates */
        int motor_target_x = (mb_w > 1) ? (col * X_MAX) / (mb_w - 1) : X_CENTER;
        int motor_target_y = (mb_h > 1) ? (row * Y_MAX) / (mb_h - 1) : Y_CENTER;

        int dx = motor_target_x - cur_x;
        int dy = motor_target_y - cur_y;

        /* Apply deadzone */
        if (abs(dx) <= deadzone) dx = 0;
        if (abs(dy) <= deadzone) dy = 0;

        /* Clamp to max speed */
        if (dx > max_speed) dx = max_speed;
        if (dx < -max_speed) dx = -max_speed;
        if (dy > max_speed) dy = max_speed;
        if (dy < -max_speed) dy = -max_speed;

        if (dx != 0 || dy != 0) {
            if (verbose)
                fprintf(stderr, "tracking: move dx=%d dy=%d (target=%d,%d cur=%d,%d)\n",
                        dx, dy, motor_target_x, motor_target_y, cur_x, cur_y);
            motor_move(dx, dy);
            write_state("tracking");
        }

        usleep(MOVE_COOLDOWN_MS * 1000);
    }

    write_state("stopped");
    run_script("off");
    motor_close();
    unlink(PIDFILE);

    return 0;
}
