#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <pthread.h>

#define MOTOR_DEVICE    "/dev/motor"
#define PWM_DEVICE      "/dev/ftpwmtmr010"
#define STATE_FILE      "/tmp/motor_ctrl.state"
#define ZOOM_STATE_FILE "/dev/shm/rtspd_zoom"

#define X_MAX   31
#define Y_MAX   15
#define PRESET_MAX 16

#define MOTOR_MAGIC     'M'
#define H_DIR_SET       _IOW(MOTOR_MAGIC,  3, int)
#define H_DIST_SET      _IOW(MOTOR_MAGIC,  4, int)
#define H_COORD_GET     _IOW(MOTOR_MAGIC,  5, int)
#define H_COORD_SET     _IOW(MOTOR_MAGIC,  6, int)
#define V_DIR_SET       _IOW(MOTOR_MAGIC, 23, int)
#define V_DIST_SET      _IOW(MOTOR_MAGIC, 24, int)
#define V_COORD_GET     _IOW(MOTOR_MAGIC, 25, int)
#define V_COORD_SET     _IOW(MOTOR_MAGIC, 26, int)

#define PWM_IOCTL_01    0x40047001UL
#define PWM_IOCTL_02    0x40047002UL
#define PWM_IOCTL_05    0x40307005UL
#define PWM_IOCTL_06    0x40307006UL
#define PWM_IOCTL_07    0x40307007UL
#define PWM_IOCTL_09    0x40307009UL
#define PWM_IOCTL_0E    0x4004700eUL

typedef struct { uint32_t value[12]; } pwm_config_t;

typedef struct {
    int used, x, y;
    char name[64];
} preset_t;

static int motor_fd = -1;
static int pwm_fd = -1;
static int cur_x = 15, cur_y = 7;
static preset_t presets[PRESET_MAX];

static void print_usage(void)
{
    printf(
        "Usage:\n"
        "  motor_control <command> [args]\n"
        "\n"
        "Commands:\n"
        "  left [steps]       Pan left (default 1 step)\n"
        "  right [steps]      Pan right (default 1 step)\n"
        "  up [steps]         Tilt up (default 1 step)\n"
        "  down [steps]       Tilt down (default 1 step)\n"
        "  stop               Stop any ongoing movement\n"
        "  home               Move to home position (center)\n"
        "  goto <x> <y>       Move to absolute position (x: 0-%d, y: 0-%d)\n"
        "  pos                Show current position\n"
        "  status             Show motor status (position + presets)\n"
        "  status -j          Show as JSON\n"
        "  preset save <n> <name>  Save current position as preset <n> (0-%d)\n"
        "  preset goto <n>    Move to preset <n>\n"
        "  preset list        List all saved presets\n"
        "  preset clear <n>   Delete preset <n>\n"
        "  zoom               Show current zoom level (from rtspd shared state)\n"
        "\n"
        "Coordinate ranges:\n"
        "  Horizontal (x): 0 to %d (center: %d)\n"
        "  Vertical (y):   0 to %d (center: %d)\n"
        "\n"
        "Examples:\n"
        "  motor_control right 5       # pan right 5 steps\n"
        "  motor_control goto 15 7     # move to center\n"
        "  motor_control preset save 0 home   # save center as preset 0\n"
        "  motor_control preset goto 0         # go to preset 0\n",
        X_MAX, Y_MAX, PRESET_MAX,
        X_MAX, X_MAX / 2, Y_MAX, Y_MAX / 2
    );
    exit(EXIT_FAILURE);
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
        fprintf(stderr, "WARN: PWM init failed (motor may not move)\n");

    motor_fd = open(MOTOR_DEVICE, O_RDWR);
    if (motor_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", MOTOR_DEVICE, strerror(errno));
        return -1;
    }
    return 0;
}

static int motor_ioctl(unsigned long cmd, int *val)
{
    int rc;
    do { rc = ioctl(motor_fd, cmd, val); } while (rc < 0 && errno == EINTR);
    return rc;
}

static int motor_refresh(void)
{
    int hpos = -1, vpos = -1;
    if (motor_ioctl(H_COORD_GET, &hpos) < 0 || motor_ioctl(V_COORD_GET, &vpos) < 0)
        return -1;
    if (hpos < 0 || hpos > X_MAX || vpos < 0 || vpos > Y_MAX)
        return -1;
    cur_x = hpos;
    cur_y = vpos;
    return 0;
}

static int motor_move(int dx, int dy)
{
    int dir, dist, rc = 0;

    if (cur_x + dx < 0) dx = -cur_x;
    if (cur_x + dx > X_MAX) dx = X_MAX - cur_x;
    if (cur_y + dy < 0) dy = -cur_y;
    if (cur_y + dy > Y_MAX) dy = Y_MAX - cur_y;

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
    return motor_refresh();
}

static void save_state(void)
{
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "%d %d\n", cur_x, cur_y);
    for (int i = 0; i < PRESET_MAX; i++)
        if (presets[i].used)
            fprintf(f, "P %d %d %s\n", presets[i].x, presets[i].y, presets[i].name);
    fclose(f);
}

static void load_state(void)
{
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f))
        sscanf(line, "%d %d", &cur_x, &cur_y);
    while (fgets(line, sizeof(line), f)) {
        int x, y;
        char name[64];
        if (sscanf(line, "P %d %d %63s", &x, &y, name) == 3) {
            for (int i = 0; i < PRESET_MAX; i++) {
                if (!presets[i].used) {
                    presets[i].used = 1;
                    presets[i].x = x;
                    presets[i].y = y;
                    strncpy(presets[i].name, name, 63);
                    break;
                }
            }
        }
    }
    fclose(f);
}

static void write_zoom(void)
{
    FILE *f = fopen(ZOOM_STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "0.0000 %.4f %.4f\n",
            (float)cur_x / X_MAX, (float)cur_y / Y_MAX);
    fclose(f);
}

static void cmd_pos(void)
{
    printf("Position: X=%d Y=%d\n", cur_x, cur_y);
}

static void cmd_status_json(void)
{
    printf("{\n");
    printf("  \"x\": %d,\n", cur_x);
    printf("  \"y\": %d,\n", cur_y);
    printf("  \"x_max\": %d,\n", X_MAX);
    printf("  \"y_max\": %d,\n", Y_MAX);
    printf("  \"presets\": [\n");
    int first = 1;
    for (int i = 0; i < PRESET_MAX; i++) {
        if (presets[i].used) {
            if (!first) printf(",\n");
            printf("    {\"slot\": %d, \"x\": %d, \"y\": %d, \"name\": \"%s\"}",
                   i, presets[i].x, presets[i].y, presets[i].name);
            first = 0;
        }
    }
    printf("\n  ]\n");
    printf("}\n");
}

static void cmd_status_human(void)
{
    printf("=== Motor Status ===\n");
    printf("  Position:  X=%d Y=%d  (range: 0-%d x 0-%d)\n", cur_x, cur_y, X_MAX, Y_MAX);
    printf("  Presets:\n");
    int found = 0;
    for (int i = 0; i < PRESET_MAX; i++) {
        if (presets[i].used) {
            printf("    [%d] %s  -> X=%d Y=%d\n", i, presets[i].name, presets[i].x, presets[i].y);
            found = 1;
        }
    }
    if (!found) printf("    (none)\n");
}

static void cmd_zoom(void)
{
    FILE *f = fopen(ZOOM_STATE_FILE, "r");
    if (!f) { printf("Zoom not available (rtspd not running?)\n"); return; }
    float zoom = 0, px = 0, py = 0;
    if (fscanf(f, "%f %f %f", &zoom, &px, &py) >= 1)
        printf("Zoom: %.2f  Pan: %.2f  Tilt: %.2f\n", zoom, px, py);
    fclose(f);
}

static void cmd_preset_save(int argc, char *argv[])
{
    if (argc < 4) { fprintf(stderr, "Usage: motor_control preset save <n> <name>\n"); return; }
    int n = atoi(argv[2]);
    if (n < 0 || n >= PRESET_MAX) { fprintf(stderr, "Preset slot must be 0-%d\n", PRESET_MAX - 1); return; }
    presets[n].used = 1;
    presets[n].x = cur_x;
    presets[n].y = cur_y;
    strncpy(presets[n].name, argv[3], 63);
    save_state();
    printf("OK,PRESET %d SAVED: %s -> X=%d Y=%d\n", n, argv[3], cur_x, cur_y);
}

static void cmd_preset_goto(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: motor_control preset goto <n>\n"); return; }
    int n = atoi(argv[2]);
    if (n < 0 || n >= PRESET_MAX || !presets[n].used) {
        fprintf(stderr, "Preset %d not found\n", n);
        return;
    }
    int dx = presets[n].x - cur_x;
    int dy = presets[n].y - cur_y;
    if (motor_move(dx, dy) < 0) fprintf(stderr, "Move failed\n");
    else printf("OK,GOTO PRESET %d (%s) -> X=%d Y=%d\n", n, presets[n].name, cur_x, cur_y);
}

static void cmd_preset_list(void)
{
    int found = 0;
    for (int i = 0; i < PRESET_MAX; i++) {
        if (presets[i].used) {
            printf("[%d] %s  -> X=%d Y=%d\n", i, presets[i].name, presets[i].x, presets[i].y);
            found = 1;
        }
    }
    if (!found) printf("No presets saved\n");
}

static void cmd_preset_clear(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: motor_control preset clear <n>\n"); return; }
    int n = atoi(argv[2]);
    if (n < 0 || n >= PRESET_MAX) { fprintf(stderr, "Preset slot must be 0-%d\n", PRESET_MAX - 1); return; }
    presets[n].used = 0;
    presets[n].name[0] = '\0';
    save_state();
    printf("OK,PRESET %d CLEARED\n", n);
}

int main(int argc, char *argv[])
{
    if (argc < 2) print_usage();

    load_state();

    if (motor_open() < 0) {
        fprintf(stderr, "Motor device not available\n");
        return EXIT_FAILURE;
    }

    if (motor_refresh() < 0)
        fprintf(stderr, "WARN: Could not read initial position\n");

    if (strcmp(argv[1], "left") == 0) {
        int steps = argc > 2 ? atoi(argv[2]) : 1;
        if (motor_move(-steps, 0) < 0) fprintf(stderr, "Move failed\n");
        else cmd_pos();
    }
    else if (strcmp(argv[1], "right") == 0) {
        int steps = argc > 2 ? atoi(argv[2]) : 1;
        if (motor_move(steps, 0) < 0) fprintf(stderr, "Move failed\n");
        else cmd_pos();
    }
    else if (strcmp(argv[1], "up") == 0) {
        int steps = argc > 2 ? atoi(argv[2]) : 1;
        if (motor_move(0, steps) < 0) fprintf(stderr, "Move failed\n");
        else cmd_pos();
    }
    else if (strcmp(argv[1], "down") == 0) {
        int steps = argc > 2 ? atoi(argv[2]) : 1;
        if (motor_move(0, -steps) < 0) fprintf(stderr, "Move failed\n");
        else cmd_pos();
    }
    else if (strcmp(argv[1], "stop") == 0) {
        printf("OK,STOPPED\n");
    }
    else if (strcmp(argv[1], "home") == 0) {
        int dx = (X_MAX / 2) - cur_x;
        int dy = (Y_MAX / 2) - cur_y;
        if (motor_move(dx, dy) < 0) fprintf(stderr, "Move to home failed\n");
        else printf("OK,HOME -> X=%d Y=%d\n", cur_x, cur_y);
    }
    else if (strcmp(argv[1], "goto") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: motor_control goto <x> <y>\n"); return 1; }
        int tx = atoi(argv[2]);
        int ty = atoi(argv[3]);
        if (tx < 0 || tx > X_MAX || ty < 0 || ty > Y_MAX) {
            fprintf(stderr, "Position out of range (x: 0-%d, y: 0-%d)\n", X_MAX, Y_MAX);
            return 1;
        }
        if (motor_move(tx - cur_x, ty - cur_y) < 0) fprintf(stderr, "Move failed\n");
        else printf("OK,GOTO -> X=%d Y=%d\n", cur_x, cur_y);
    }
    else if (strcmp(argv[1], "pos") == 0) {
        motor_refresh();
        cmd_pos();
    }
    else if (strcmp(argv[1], "status") == 0) {
        motor_refresh();
        if (argc > 2 && strcmp(argv[2], "-j") == 0)
            cmd_status_json();
        else
            cmd_status_human();
    }
    else if (strcmp(argv[1], "preset") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: motor_control preset <save|goto|list|clear> ...\n"); return 1; }
        if (strcmp(argv[2], "save") == 0) cmd_preset_save(argc, argv);
        else if (strcmp(argv[2], "goto") == 0) cmd_preset_goto(argc, argv);
        else if (strcmp(argv[2], "list") == 0) cmd_preset_list();
        else if (strcmp(argv[2], "clear") == 0) cmd_preset_clear(argc, argv);
        else { fprintf(stderr, "Unknown preset command: %s\n", argv[2]); return 1; }
    }
    else if (strcmp(argv[1], "zoom") == 0) {
        cmd_zoom();
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        print_usage();
    }

    save_state();
    write_zoom();

    if (motor_fd >= 0) close(motor_fd);
    if (pwm_fd >= 0) close(pwm_fd);
    return 0;
}
