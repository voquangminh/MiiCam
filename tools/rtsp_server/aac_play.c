/*
 * aac_player.c - GM8136 AAC/ADTS file player
 *
 * Reconstructed from the Xiaomi/Chuangmi stripped aac_player binary.
 * Uses the original GM SDK runtime (libgm.so/libgm.a); it does not decode AAC
 * in userspace. Each complete ADTS frame is passed to GM_FILE_OBJECT bound to
 * GM_AUDIO_RENDER_OBJECT.
 *
 * Build example:
 *   arm-unknown-linux-uclibcgnueabi-gcc -std=gnu99 -Os -Wall -Wextra \
 *       aac_player.c -L/path/to/gm_lib/lib -lgm -o aac_player
 *
 * Input requirement: AAC-LC ADTS. ADDA308 on this camera is known to accept
 * 16000 Hz mono; 32000 Hz was rejected by the vendor audio graph.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GM_ABI_VERSION          52
#define GM_FILE_OBJECT          0xfefe0004U
#define GM_AUDIO_RENDER_OBJECT  0xfefe0007U
#define GM_FILE_ATTR_SIZE       68U
#define GM_RENDER_ATTR_SIZE     64U
#define GM_STREAM_DESC_SIZE     128U
#define AAC_FRAME_MAX           12800U
#define GM_SEND_TIMEOUT_MS      500

/* Offsets recovered from the vendor binary. */
#define FILE_SAMPLE_RATE_OFF    48U
#define FILE_SAMPLE_SIZE_OFF    50U
#define FILE_CHANNEL_TYPE_OFF   52U
#define RENDER_VALUE0_OFF       40U
#define RENDER_VALUE1_OFF       44U
#define RENDER_VALUE2_OFF       48U
#define STREAM_BIND_OFF          0U
#define STREAM_DATA_OFF          4U
#define STREAM_LENGTH_OFF        8U

/* GM SDK ABI. These declarations match the ARM32 vendor binary imports. */
extern int   gm_init_private(int abi_version);
extern int   gm_init_attr(void *attr, const char *name,
                          int attr_size, int abi_version);
extern void *gm_new_groupfd(void);
extern int   gm_delete_groupfd(void *groupfd);
extern void *gm_new_obj(uint32_t object_type);
extern int   gm_delete_obj(void *object);
extern int   gm_set_attr(void *object, void *attr);
extern void *gm_bind(void *groupfd, void *source, void *destination);
extern int   gm_unbind(void *bindfd);
extern int   gm_apply(void *groupfd);
extern int   gm_send_multi_bitstreams(void *streams,
                                      int stream_count,
                                      int timeout_ms);
extern void  gm_release(void);

static void put_u16(void *base, size_t offset, uint16_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void put_u32(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void put_ptr32(void *base, size_t offset, const void *value)
{
    uint32_t arm_pointer = (uint32_t)(uintptr_t)value;
    put_u32(base, offset, arm_pointer);
}

static int adts_sample_rate_from_header(const uint8_t header[7])
{
    static const int sample_rates[16] = {
        96000, 88200, 64000, 48000,
        44100, 32000, 24000, 22050,
        16000, 12000, 11025,  8000,
         7350,    -1,    -1,    -1
    };
    unsigned int index;

    if (header[0] != 0xff || (header[1] & 0xf6) != 0xf0)
        return -1;

    index = (header[2] >> 2) & 0x0f;
    return sample_rates[index];
}

static int adts_frame_length(const uint8_t header[7])
{
    int length;

    if (adts_sample_rate_from_header(header) < 0)
        return -1;

    length = ((header[3] & 0x03) << 11) |
             ((int)header[4] << 3) |
             ((header[5] >> 5) & 0x07);

    return length >= 7 ? length : -1;
}

static int inspect_aac(const char *path, int *sample_rate, int *channels)
{
    uint8_t header[7];
    FILE *fp;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "[aac_player] Open %s failed: %s\n",
                path, strerror(errno));
        return -1;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "[aac_player] Cannot read ADTS header from %s\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    *sample_rate = adts_sample_rate_from_header(header);
    if (*sample_rate < 0) {
        fprintf(stderr, "[aac_player] Invalid/unsupported ADTS header\n");
        return -1;
    }

    *channels = ((header[2] & 0x01) << 2) | ((header[3] >> 6) & 0x03);
    if (*channels == 0)
        *channels = 1;

    return 0;
}

static int send_aac_frames(const char *path, void *bindfd)
{
    uint8_t descriptor[GM_STREAM_DESC_SIZE];
    uint8_t *frame = NULL;
    FILE *fp = NULL;
    int result = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "[aac_player] Open %s failed: %s\n",
                path, strerror(errno));
        return 1;
    }

    frame = (uint8_t *)malloc(AAC_FRAME_MAX);
    if (frame == NULL) {
        fprintf(stderr, "[aac_player] Error allocation\n");
        result = 2;
        goto out;
    }

    fprintf(stdout, "[INFO] [aac_player]playing audio file: [%s]\n", path);
    fflush(stdout);

    for (;;) {
        size_t got;
        int frame_length;
        int rc;

        got = fread(frame, 1, 7, fp);
        if (got == 0) {
            if (ferror(fp))
                result = 4;
            break;
        }
        if (got != 7) {
            fprintf(stderr, "[aac_player] Truncated ADTS header\n");
            result = 4;
            break;
        }

        frame_length = adts_frame_length(frame);
        if (frame_length < 7 || frame_length > (int)AAC_FRAME_MAX) {
            fprintf(stderr, "[aac_player] Invalid ADTS frame length: %d\n",
                    frame_length);
            result = 4;
            break;
        }

        got = fread(frame + 7, 1, (size_t)frame_length - 7, fp);
        if (got != (size_t)frame_length - 7) {
            fprintf(stderr,
                    "[aac_player] read %d from bitstream_data failed\n",
                    frame_length);
            result = 4;
            break;
        }

        memset(descriptor, 0, sizeof(descriptor));
        put_ptr32(descriptor, STREAM_BIND_OFF, bindfd);
        put_ptr32(descriptor, STREAM_DATA_OFF, frame);
        put_u32(descriptor, STREAM_LENGTH_OFF, (uint32_t)frame_length);

        rc = gm_send_multi_bitstreams(descriptor, 1, GM_SEND_TIMEOUT_MS);
        if (rc < 0) {
            fprintf(stderr, "[aac_player]<send bitstream fail(%d)!>\n", rc);
            result = 5;
            break;
        }
    }

    /* The original binary waits one second for renderer drain. */
    if (result == 0)
        sleep(1);

out:
    free(frame);
    if (fp != NULL)
        fclose(fp);
    return result;
}

int main(int argc, char **argv)
{
    uint8_t file_attr[GM_FILE_ATTR_SIZE];
    uint8_t render_attr[GM_RENDER_ATTR_SIZE];
    void *groupfd = NULL;
    void *file_object = NULL;
    void *render_object = NULL;
    void *bindfd = NULL;
    int gm_started = 0;
    int sample_rate;
    int channels;
    int result = 1;

    if (argc != 2) {
        fprintf(stderr,
                "[aac_player] Usage:\n"
                "  %s [aac file]\n", argv[0]);
        return 1;
    }

    if (sizeof(void *) != 4) {
        fprintf(stderr,
                "[aac_player] This program must be built for 32-bit ARM\n");
        return 1;
    }

    if (inspect_aac(argv[1], &sample_rate, &channels) < 0)
        return 1;

    if (sample_rate != 16000) {
        fprintf(stderr,
                "[aac_player] Warning: ADDA308 commonly accepts 16000 Hz; "
                "input is %d Hz\n", sample_rate);
    }
    if (channels != 1) {
        fprintf(stderr,
                "[aac_player] Warning: camera speaker path is mono; "
                "input has %d channels\n", channels);
    }

    if (gm_init_private(GM_ABI_VERSION) < 0) {
        fprintf(stderr, "[aac_player] gm_init_private failed\n");
        goto cleanup;
    }
    gm_started = 1;

    groupfd = gm_new_groupfd();
    file_object = gm_new_obj(GM_FILE_OBJECT);
    render_object = gm_new_obj(GM_AUDIO_RENDER_OBJECT);
    if (groupfd == NULL || file_object == NULL || render_object == NULL) {
        fprintf(stderr, "[aac_player] GM object allocation failed\n");
        goto cleanup;
    }

    memset(file_attr, 0, sizeof(file_attr));
    memset(render_attr, 0, sizeof(render_attr));

    if (gm_init_attr(file_attr, "gm_file_attr_t",
                     GM_FILE_ATTR_SIZE, GM_ABI_VERSION) < 0 ||
        gm_init_attr(render_attr, "gm_audio_render_attr_t",
                     GM_RENDER_ATTR_SIZE, GM_ABI_VERSION) < 0) {
        fprintf(stderr, "[aac_player] gm_init_attr failed\n");
        goto cleanup;
    }

    put_u16(file_attr, FILE_SAMPLE_RATE_OFF, (uint16_t)sample_rate);
    put_u16(file_attr, FILE_SAMPLE_SIZE_OFF, 16);
    put_u32(file_attr, FILE_CHANNEL_TYPE_OFF, 1); /* mono */

    /* Exact values and offsets used by the vendor player. */
    put_u32(render_attr, RENDER_VALUE0_OFF, 0);
    put_u32(render_attr, RENDER_VALUE1_OFF, 2);
    put_u32(render_attr, RENDER_VALUE2_OFF, 1024);

    if (gm_set_attr(file_object, file_attr) < 0) {
        fprintf(stderr,
                "[aac_player] gm_set_attr(file) failed; sample_rate=%d\n",
                sample_rate);
        goto cleanup;
    }
    if (gm_set_attr(render_object, render_attr) < 0) {
        fprintf(stderr, "[aac_player] gm_set_attr(audio render) failed\n");
        goto cleanup;
    }

    bindfd = gm_bind(groupfd, file_object, render_object);
    if (bindfd == NULL) {
        fprintf(stderr, "[aac_player] gm_bind failed\n");
        goto cleanup;
    }

    if (gm_apply(groupfd) < 0) {
        fprintf(stderr,
                "[aac_player] gm_apply fail, AP procedure something wrong!\n");
        goto cleanup;
    }

    result = send_aac_frames(argv[1], bindfd);
    if (result == 0)
        fprintf(stdout, "[INFO] [aac_player] play done!\n");
    else
        fprintf(stderr, "[aac_player] play failed with code: %d\n", result);

cleanup:
    if (bindfd != NULL) {
        gm_unbind(bindfd);
        if (groupfd != NULL)
            gm_apply(groupfd);
    }
    if (file_object != NULL)
        gm_delete_obj(file_object);
    if (render_object != NULL)
        gm_delete_obj(render_object);
    if (groupfd != NULL)
        gm_delete_groupfd(groupfd);
    if (gm_started)
        gm_release();

    return result;
}
