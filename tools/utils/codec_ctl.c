#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define GMLIB_SETTING  "/proc/videograph/gmlib_setting"
#define RTSPD_CTRL     "/tmp/rtspd.ctrl"
#define ZOOM_STATE     "/dev/shm/rtspd_zoom"

static void print_usage(void)
{
    printf(
        "Usage:\n"
        "  codec_ctl <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status          Show current encoder settings (human-readable)\n"
        "  status -j       Show current encoder settings (JSON)\n"
        "  status -k       Show current encoder settings (shell key=value)\n"
        "  keyframe        Request an immediate keyframe from the running encoder\n"
        "  bitrate <kbps>  Signal rtspd to change video bitrate (requires rtspd)\n"
        "  fps <num>       Signal rtspd to change framerate (requires rtspd)\n"
        "  gop <num>       Signal rtspd to change GOP length (requires rtspd)\n"
        "  zoom            Show current zoom/pan/tilt from shared state\n"
        "\n"
        "Examples:\n"
        "  codec_ctl status        # show all encoder settings\n"
        "  codec_ctl status -j     # show as JSON\n"
        "  codec_ctl keyframe      # force keyframe now\n"
        "  codec_ctl bitrate 2048  # change to 2048 kbps\n"
        "  codec_ctl fps 25        # change to 25 fps\n"
    );
    exit(EXIT_FAILURE);
}

static char *read_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    int cap = 4096;
    int total = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        int n = read(fd, buf + total, cap - total - 1);
        if (n <= 0) break;
        total += n;
        if (total >= cap - 1) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) break;
        }
    }
    close(fd);
    buf[total] = '\0';
    return buf;
}

static int write_ctrl(const char *cmd)
{
    int fd = open(RTSPD_CTRL, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: is rtspd running?\n", RTSPD_CTRL);
        return -1;
    }
    write(fd, cmd, strlen(cmd));
    close(fd);
    fprintf(stdout, "OK,%s\n", cmd);
    return 0;
}

static void parse_value(const char *line, const char *key, int *out)
{
    const char *p = strstr(line, key);
    if (!p) return;
    p += strlen(key);
    while (*p == '(' || *p == ' ') p++;
    *out = atoi(p);
}

static void cmd_status_json(void)
{
    char *data = read_file(GMLIB_SETTING);
    if (!data) { fprintf(stderr, "Cannot read %s\n", GMLIB_SETTING); return; }

    int w = 0, h = 0, fps = 0, gop = 0, bitrate = 0, bitrate_max = 0;
    int init_q = 0, min_q = 0, max_q = 0, ms = 0, rm = 0;
    int ev = 0, ir = 0;

    char *line = strtok(data, "\n");
    while (line) {
        parse_value(line, "dim_width(", &w);
        parse_value(line, "dim_height(", &h);
        parse_value(line, "framerate(", &fps);
        parse_value(line, "gop(", &gop);
        parse_value(line, "init_quant(", &init_q);
        parse_value(line, "min_quant(", &min_q);
        parse_value(line, "max_quant(", &max_q);
        parse_value(line, "bitrate(", &bitrate);
        parse_value(line, "bitrate_max(", &bitrate_max);
        parse_value(line, "multi_slice(", &ms);
        parse_value(line, "rate_mode(", &rm);
        line = strtok(NULL, "\n");
    }

    printf("{\n");
    printf("  \"video\": {\n");
    printf("    \"width\": %d,\n", w);
    printf("    \"height\": %d,\n", h);
    printf("    \"framerate\": %d,\n", fps);
    printf("    \"gop\": %d,\n", gop);
    printf("    \"bitrate\": %d,\n", bitrate);
    printf("    \"bitrate_max\": %d,\n", bitrate_max);
    printf("    \"init_quant\": %d,\n", init_q);
    printf("    \"min_quant\": %d,\n", min_q);
    printf("    \"max_quant\": %d,\n", max_q);
    printf("    \"rate_mode\": %d,\n", rm);
    printf("    \"multi_slice\": %d\n", ms);
    printf("  }\n");
    printf("}\n");

    free(data);
}

static void cmd_status_shell(void)
{
    char *data = read_file(GMLIB_SETTING);
    if (!data) { fprintf(stderr, "Cannot read %s\n", GMLIB_SETTING); return; }

    int w = 0, h = 0, fps = 0, gop = 0, bitrate = 0, bitrate_max = 0;
    int init_q = 0, min_q = 0, max_q = 0, ms = 0, rm = 0;

    char *line = strtok(data, "\n");
    while (line) {
        parse_value(line, "dim_width(", &w);
        parse_value(line, "dim_height(", &h);
        parse_value(line, "framerate(", &fps);
        parse_value(line, "gop(", &gop);
        parse_value(line, "init_quant(", &init_q);
        parse_value(line, "min_quant(", &min_q);
        parse_value(line, "max_quant(", &max_q);
        parse_value(line, "bitrate(", &bitrate);
        parse_value(line, "bitrate_max(", &bitrate_max);
        parse_value(line, "multi_slice(", &ms);
        parse_value(line, "rate_mode(", &rm);
        line = strtok(NULL, "\n");
    }

    printf("CODEC_WIDTH=%d\n", w);
    printf("CODEC_HEIGHT=%d\n", h);
    printf("CODEC_FRAMERATE=%d\n", fps);
    printf("CODEC_GOP=%d\n", gop);
    printf("CODEC_BITRATE=%d\n", bitrate);
    printf("CODEC_BITRATE_MAX=%d\n", bitrate_max);
    printf("CODEC_INIT_QUANT=%d\n", init_q);
    printf("CODEC_MIN_QUANT=%d\n", min_q);
    printf("CODEC_MAX_QUANT=%d\n", max_q);
    printf("CODEC_RATE_MODE=%d\n", rm);
    printf("CODEC_MULTI_SLICE=%d\n", ms);

    free(data);
}

static void cmd_status_human(void)
{
    char *data = read_file(GMLIB_SETTING);
    if (!data) { fprintf(stderr, "Cannot read %s\n", GMLIB_SETTING); return; }

    int w = 0, h = 0, fps = 0, gop = 0, bitrate = 0, bitrate_max = 0;
    int init_q = 0, min_q = 0, max_q = 0, ms = 0, rm = 0;

    char *line = strtok(data, "\n");
    while (line) {
        parse_value(line, "dim_width(", &w);
        parse_value(line, "dim_height(", &h);
        parse_value(line, "framerate(", &fps);
        parse_value(line, "gop(", &gop);
        parse_value(line, "init_quant(", &init_q);
        parse_value(line, "min_quant(", &min_q);
        parse_value(line, "max_quant(", &max_q);
        parse_value(line, "bitrate(", &bitrate);
        parse_value(line, "bitrate_max(", &bitrate_max);
        parse_value(line, "multi_slice(", &ms);
        parse_value(line, "rate_mode(", &rm);
        line = strtok(NULL, "\n");
    }

    printf("=== Video Encoder Status ===\n");
    printf("  Resolution:   %dx%d\n", w, h);
    printf("  Framerate:    %d fps\n", fps);
    printf("  GOP:          %d\n", gop);
    printf("  Bitrate:      %d kbps (max: %d)\n", bitrate, bitrate_max);
    printf("  Quantizer:    init=%d  min=%d  max=%d\n", init_q, min_q, max_q);
    printf("  Rate mode:    %s\n", rm ? "VBR" : "CBR");
    printf("  Multi-slice:  %d\n", ms);

    free(data);
}

static void cmd_zoom(void)
{
    char *data = read_file(ZOOM_STATE);
    if (!data) {
        printf("Zoom state not available (rtspd not running?)\n");
        return;
    }
    float zoom = 0, px = 0, py = 0;
    sscanf(data, "%f %f %f", &zoom, &px, &py);
    printf("Zoom: %.2f  Pan: %.2f  Tilt: %.2f\n", zoom, px, py);
    free(data);
}

int main(int argc, char *argv[])
{
    if (argc < 2) print_usage();

    if (strcmp(argv[1], "status") == 0) {
        if (argc > 2 && strcmp(argv[2], "-j") == 0)
            cmd_status_json();
        else if (argc > 2 && strcmp(argv[2], "-k") == 0)
            cmd_status_shell();
        else
            cmd_status_human();
    }
    else if (strcmp(argv[1], "keyframe") == 0) {
        return write_ctrl("keyframe");
    }
    else if (strcmp(argv[1], "bitrate") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: codec_ctl bitrate <kbps>\n"); return 1; }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "bitrate %s", argv[2]);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[1], "fps") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: codec_ctl fps <num>\n"); return 1; }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "fps %s", argv[2]);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[1], "gop") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: codec_ctl gop <num>\n"); return 1; }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "gop %s", argv[2]);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[1], "zoom") == 0) {
        cmd_zoom();
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        print_usage();
    }

    return 0;
}
