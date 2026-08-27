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
        "  codec_ctrl <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status              Show video + audio encoder settings\n"
        "  status -j           Show as JSON\n"
        "  status -k           Show as shell key=value\n"
        "  keyframe            Request an immediate keyframe\n"
        "  bitrate <kbps>      Change video bitrate (requires rtspd)\n"
        "  mode <1-4>          Change bitrate mode: 1=CBR 2=VBR 3=ECBR 4=EVBR (requires rtspd)\n"
        "  fps <num>           Change framerate (requires rtspd)\n"
        "  gop <num>           Change GOP length (requires rtspd)\n"
        "  zoom                Show zoom/pan/tilt from shared state\n"
        "\n"
        "Examples:\n"
        "  codec_ctrl status            # show all settings\n"
        "  codec_ctrl status -j         # show as JSON\n"
        "  codec_ctrl bitrate 4096      # change to 4096 kbps\n"
        "  codec_ctrl mode 4            # change to EVBR\n"
        "  codec_ctrl fps 25            # change to 25 fps\n"
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

#define PARSE_ALL_VIDEO(line, v) do { \
    parse_value(line, "dim_width(",    &(v)->w);          \
    parse_value(line, "dim_height(",   &(v)->h);          \
    parse_value(line, "framerate(",    &(v)->fps);        \
    parse_value(line, "gop(",          &(v)->gop);        \
    parse_value(line, "init_quant(",   &(v)->init_q);     \
    parse_value(line, "min_quant(",    &(v)->min_q);      \
    parse_value(line, "max_quant(",    &(v)->max_q);      \
    parse_value(line, "bitrate_max(",  &(v)->bitrate_max); \
    parse_value(line, "multi_slice(",  &(v)->ms);         \
    parse_value(line, "rate_mode(",    &(v)->rm);         \
} while(0)

#define PARSE_ALL_AUDIO(line, a) do { \
    parse_value(line, "sample_rate(",   &(a)->sample_rate); \
    parse_value(line, "sample_size(",   &(a)->sample_size); \
    parse_value(line, "frame_samples(", &(a)->frame_samples); \
    parse_value(line, "block_count(",   &(a)->block_count); \
} while(0)

struct video_info {
    int w, h, fps, gop, bitrate, bitrate_max;
    int init_q, min_q, max_q, ms, rm;
};

struct audio_info {
    int sample_rate, sample_size, bitrate, frame_samples, block_count;
};

static void parse_proc(char *data, struct video_info *v, struct audio_info *a)
{
    int in_video = 0, in_audio = 0;
    char *line = strtok(data, "\n");
    while (line) {
        if (strstr(line, "H264E("))      { in_video = 1; in_audio = 0; }
        if (strstr(line, "AUDIO_ENC(")) { in_audio = 1; in_video = 0; }

        if (in_video) {
            PARSE_ALL_VIDEO(line, v);
            parse_value(line, "bitrate(", &v->bitrate);
        }
        if (in_audio) {
            PARSE_ALL_AUDIO(line, a);
            parse_value(line, "bitrate(", &a->bitrate);
        }
        if (strstr(line, "AUDIO_GRAB(")) {
            parse_value(line, "sample_rate(", &a->sample_rate);
            parse_value(line, "sample_size(", &a->sample_size);
        }
        line = strtok(NULL, "\n");
    }
}

static void cmd_status_json(void)
{
    char *data = read_file(GMLIB_SETTING);
    if (!data) { fprintf(stderr, "Cannot read %s\n", GMLIB_SETTING); return; }

    struct video_info v = {0};
    struct audio_info a = {0};
    parse_proc(data, &v, &a);

    printf("{\n");
    printf("  \"video\": {\n");
    printf("    \"width\": %d,\n", v.w);
    printf("    \"height\": %d,\n", v.h);
    printf("    \"framerate\": %d,\n", v.fps);
    printf("    \"gop\": %d,\n", v.gop);
    printf("    \"bitrate\": %d,\n", v.bitrate);
    printf("    \"bitrate_max\": %d,\n", v.bitrate_max);
    printf("    \"init_quant\": %d,\n", v.init_q);
    printf("    \"min_quant\": %d,\n", v.min_q);
    printf("    \"max_quant\": %d,\n", v.max_q);
    printf("    \"rate_mode\": %d,\n", v.rm);
    printf("    \"multi_slice\": %d\n", v.ms);
    printf("  },\n");
    printf("  \"audio\": {\n");
    printf("    \"sample_rate\": %d,\n", a.sample_rate);
    printf("    \"sample_size\": %d,\n", a.sample_size);
    printf("    \"bitrate\": %d,\n", a.bitrate);
    printf("    \"frame_samples\": %d,\n", a.frame_samples);
    printf("    \"block_count\": %d\n", a.block_count);
    printf("  }\n");
    printf("}\n");

    free(data);
}

static void cmd_status_shell(void)
{
    char *data = read_file(GMLIB_SETTING);
    if (!data) { fprintf(stderr, "Cannot read %s\n", GMLIB_SETTING); return; }

    struct video_info v = {0};
    struct audio_info a = {0};
    parse_proc(data, &v, &a);

    printf("CODEC_WIDTH=%d\n", v.w);
    printf("CODEC_HEIGHT=%d\n", v.h);
    printf("CODEC_FRAMERATE=%d\n", v.fps);
    printf("CODEC_GOP=%d\n", v.gop);
    printf("CODEC_BITRATE=%d\n", v.bitrate);
    printf("CODEC_BITRATE_MAX=%d\n", v.bitrate_max);
    printf("CODEC_INIT_QUANT=%d\n", v.init_q);
    printf("CODEC_MIN_QUANT=%d\n", v.min_q);
    printf("CODEC_MAX_QUANT=%d\n", v.max_q);
    printf("CODEC_RATE_MODE=%d\n", v.rm);
    printf("CODEC_MULTI_SLICE=%d\n", v.ms);
    printf("AUDIO_SAMPLE_RATE=%d\n", a.sample_rate);
    printf("AUDIO_SAMPLE_SIZE=%d\n", a.sample_size);
    printf("AUDIO_BITRATE=%d\n", a.bitrate);
    printf("AUDIO_FRAME_SAMPLES=%d\n", a.frame_samples);
    printf("AUDIO_BLOCK_COUNT=%d\n", a.block_count);

    free(data);
}

static const char *rate_mode_name(int rm)
{
    switch (rm) {
        case 1: return "CBR";
        case 2: return "VBR";
        case 3: return "ECBR";
        case 4: return "EVBR";
        default: return "?";
    }
}

static void cmd_status_human(void)
{
    char *data = read_file(GMLIB_SETTING);
    if (!data) { fprintf(stderr, "Cannot read %s\n", GMLIB_SETTING); return; }

    struct video_info v = {0};
    struct audio_info a = {0};
    parse_proc(data, &v, &a);

    printf("=== Video Encoder ===\n");
    printf("  Resolution:     %dx%d\n", v.w, v.h);
    printf("  Framerate:      %d fps\n", v.fps);
    printf("  GOP:            %d\n", v.gop);
    printf("  Bitrate:        %d kbps (max: %d)\n", v.bitrate, v.bitrate_max);
    printf("  Quantizer:      init=%d  min=%d  max=%d\n", v.init_q, v.min_q, v.max_q);
    printf("  Rate mode:      %s (%d)\n", rate_mode_name(v.rm), v.rm);
    printf("  Multi-slice:    %d\n", v.ms);
    printf("\n");
    printf("=== Audio Encoder ===\n");
    printf("  Sample rate:    %d Hz\n", a.sample_rate);
    printf("  Sample size:    %d bit\n", a.sample_size);
    printf("  Bitrate:        %d bps\n", a.bitrate);
    printf("  Frame samples:  %d\n", a.frame_samples);
    printf("  Block count:    %d\n", a.block_count);

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
    int json = 0, shell = 0, argi = 1;

    if (argc < 2) print_usage();

    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-j") == 0) json = 1;
        else if (strcmp(argv[argi], "-k") == 0) shell = 1;
        else break;
        argi++;
    }

    if (strcmp(argv[argi], "status") == 0) {
        if (json) cmd_status_json();
        else if (shell) cmd_status_shell();
        else cmd_status_human();
    }
    else if (strcmp(argv[argi], "keyframe") == 0) {
        return write_ctrl("keyframe");
    }
    else if (strcmp(argv[argi], "bitrate") == 0) {
        if (argi + 1 >= argc) { fprintf(stderr, "Usage: codec_ctrl bitrate <kbps>\n"); return 1; }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "bitrate %s", argv[argi + 1]);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[argi], "mode") == 0) {
        if (argi + 1 >= argc) { fprintf(stderr, "Usage: codec_ctrl mode <1-4> (1=CBR 2=VBR 3=ECBR 4=EVBR)\n"); return 1; }
        int m = atoi(argv[argi + 1]);
        if (m < 1 || m > 4) {
            fprintf(stderr, "mode must be 1-4 (1=CBR 2=VBR 3=ECBR 4=EVBR)\n");
            return 1;
        }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "mode %d", m);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[argi], "fps") == 0) {
        if (argi + 1 >= argc) { fprintf(stderr, "Usage: codec_ctrl fps <num>\n"); return 1; }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "fps %s", argv[argi + 1]);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[argi], "gop") == 0) {
        if (argi + 1 >= argc) { fprintf(stderr, "Usage: codec_ctrl gop <num>\n"); return 1; }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "gop %s", argv[argi + 1]);
        return write_ctrl(cmd);
    }
    else if (strcmp(argv[argi], "zoom") == 0) {
        cmd_zoom();
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[argi]);
        print_usage();
    }

    return 0;
}
