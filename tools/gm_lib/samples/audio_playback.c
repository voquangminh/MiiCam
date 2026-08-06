/**
 * @file audio_playback.c
 *
 * Support platform: GM8210, GM8287, GM8139
 * This sample demos audio playback
 * Copyright (C) 2013 GM Corp. (http://www.grain-media.com)
 *
 * $Revision: 1.10 $
 * $Date: 2014/06/26 02:55:48 $
 *
 */
/**
 * @example audio_playback.c
 */
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include "gmlib.h"

#define BITSTREAM_LEN     12800

gm_system_t gm_system;
void *groupfd;    // return of gm_new_groupfd()
void *bindfd;      // return of gm_bind()
void *file_object;
void *audio_render_object;
pthread_t thread_id;
int pb_exit = 0;
static char playback_filename[256];
static char playback_len_filename[256];

static void *playback_thread(void *arg)
{
    int ret, length;
    FILE *bs_fd, *len_fd;
    char *bitstream_data;
    gm_dec_multi_bitstream_t multi_bs[1];

    bs_fd = fopen(playback_filename, "rb");
    if (bs_fd == NULL) {
        printf("[ERROR] Open %s failed!!\n", playback_filename);
        exit(1);
    }

    printf("Play file: [%s]\n", playback_filename);
    if (strlen(playback_filename) > 4 && strcmp(playback_filename + strlen(playback_filename) - 4, ".aac") == 0) {
        strncpy(playback_len_filename, playback_filename, strlen(playback_filename) - 4);
        playback_len_filename[strlen(playback_filename) - 4] = '\0';
    } else {
        strncpy(playback_len_filename, playback_filename, sizeof(playback_len_filename) - 1);
        playback_len_filename[sizeof(playback_len_filename) - 1] = '\0';
    }
    strncat(playback_len_filename, ".len", sizeof(playback_len_filename) - strlen(playback_len_filename) - 1);
    len_fd = fopen(playback_len_filename, "rb");

    bitstream_data = (char *)malloc(BITSTREAM_LEN);
    if (!bitstream_data) {
        printf("Error allocation\n");
        exit(1);
    }

    if (len_fd == NULL) {
        long file_size;
        printf("No %s found, playing the whole AAC file as a single chunk\n", playback_len_filename);
        if (fseek(bs_fd, 0, SEEK_END) != 0) {
            printf("Failed to seek %s\n", playback_filename);
            goto playback_done;
        }
        file_size = ftell(bs_fd);
        if (file_size <= 0 || file_size > BITSTREAM_LEN) {
            printf("Invalid file size %ld for %s\n", file_size, playback_filename);
            goto playback_done;
        }
        rewind(bs_fd);
        if (fread(bitstream_data, 1, file_size, bs_fd) != (size_t)file_size) {
            printf("Failed to read %ld bytes from %s\n", file_size, playback_filename);
            goto playback_done;
        }

        memset(multi_bs, 0, sizeof(multi_bs));
        multi_bs[0].bindfd = bindfd;
        multi_bs[0].bs_buf = bitstream_data;
        multi_bs[0].bs_buf_len = file_size;

        if ((ret = gm_send_multi_bitstreams(multi_bs, 1, 500)) < 0) {
            printf("<send bitstream fail(%d)!>\n", ret);
        }
        goto playback_done;
    }

    while (1) {
        if (pb_exit == 1)
            break;

        if (fscanf(len_fd, "%d\n", &length) == EOF) {
            fseek(bs_fd, 0, SEEK_SET);
            fseek(len_fd, 0, SEEK_SET);
            if (fscanf(len_fd, "%d\n", &length) == EOF)
                break;
        }
        if (length == 0)
            continue;

        if (length > BITSTREAM_LEN) {
            printf("Invalid length, len(%d)\n", length);
            exit(1);
        }

        if (fread(bitstream_data, 1, length, bs_fd) != (size_t)length) {
            fseek(bs_fd, 0, SEEK_SET);
            if (fread(bitstream_data, 1, length, bs_fd) != (size_t)length) {
                printf("Failed to read %d bytes from %s\n", length, playback_filename);
                break;
            }
        }

        memset(multi_bs, 0, sizeof(multi_bs));  //clear all mutli bs         
        multi_bs[0].bindfd = bindfd;
        multi_bs[0].bs_buf = bitstream_data;
        multi_bs[0].bs_buf_len = length;

        if ((ret = gm_send_multi_bitstreams(multi_bs, 1, 500)) < 0) {
            printf("<send bitstream fail(%d)!>\n", ret);
            exit(1);
        }
    }

playback_done:
    fclose(bs_fd);
    if (len_fd)
        fclose(len_fd);
    free(bitstream_data);
    return 0;
}


void show_message(void)
{
    printf("Usage:\n  #./aac_play <file.aac> [output_vch]\n\n");
    printf("You may use commands to know GM standard EVB audio setting:\n");
    printf("  #cat /proc/videograph/vpd/au_grab\n");
    printf("  #cat /proc/videograph/vpd/au_render\n");
    printf("  GM8210_32D1_1080P:\n");
    printf("    input vch: 0\n");
    printf("    output vch: 0(nvp1918)   2(HDMI)\n");
    printf("  GM8210_8HD_1080P:\n");
    printf("    input vch: 8\n");
    printf("    output vch: 3(HDMI)\n");
    printf("  GM8287_16D1_1080P:\n");
    printf("    input vch: 0\n");
    printf("    output vch: 0(nvp1918)   1(HDMI)\n");
    printf("  GM8287_4HD_1080P:\n");
    printf("    input vch: 4\n");
    printf("    output vch: 1(cx20811)   2(HDMI)\n");
    printf("  GM8283_8CH_960H_1080P:\n");
    printf("    input vch: 0\n");
    printf("    output vch: 0(nvp1918)   1(HDMI)\n");
    printf("  GM8282_16CH_960H_1080P:\n");
    printf("    input vch: 0\n");
    printf("    output vch: 0(nvp1918)\n");
    printf("  GM8138/GM8139:\n");
    printf("    input vch: 0\n");
    printf("    output vch: 0(adda302)\n");
}


int main(int argc, char *argv[])
{
    int key, ch = 0;
    DECLARE_ATTR(file_attr, gm_file_attr_t);
    DECLARE_ATTR(audio_render_attr, gm_audio_render_attr_t);

    if (argc < 2) {
        show_message();
        exit(0);
    }

    strncpy(playback_filename, argv[1], sizeof(playback_filename) - 1);
    playback_filename[sizeof(playback_filename) - 1] = '\0';
    if (argc >= 3)
        ch = atoi(argv[2]);

    printf("Audio playback output vch %d from file %s\n", ch, playback_filename);

    /**
     * This sample demonstrates audio playback output to one device.
     * If you implement dual audio output, create the second bind with different vch and apply it.
     * The vch of file object can be any value, and all the vch of file_objs with the same value
     *  means the same audio source.
     * All of the binds should be allocated within the same group.
     * While sending the audio data, you can send to any one of bind fds,
     *  and don't care about another fd.
     * ex: bindfd0 = file_obj0(vch=0) -> render_obj0(vch=0)  @ group0
     *     bindfd1 = file_obj0(vch=0) -> render_obj1(vch=1)  @ group0
     *     apply(group0);
     *     send_bistream(bindfd0, data);  //don't care about bindfd1
     */

    gm_init();
    gm_get_sysinfo(&gm_system);
    groupfd = gm_new_groupfd();
    
    file_object = gm_new_obj(GM_FILE_OBJECT);
    audio_render_object = gm_new_obj(GM_AUDIO_RENDER_OBJECT);

    // sample_rate/sample_size/channel_type: the audio info of the input file
    file_attr.sample_rate = 8000;
    file_attr.sample_size = 16;
    file_attr.channel_type = GM_MONO;
    gm_set_attr(file_object, &file_attr);

    // block_size: PCM(don't care) AAC(1024) ADPCM(256) G711(320)
    audio_render_attr.vch = ch;
    audio_render_attr.encode_type = GM_AAC;
    audio_render_attr.block_size = 1024;
    gm_set_attr(audio_render_object, &audio_render_attr);

    bindfd = gm_bind(groupfd, file_object, audio_render_object);
    if (gm_apply(groupfd) < 0) {
        perror("Error! gm_apply fail, AP procedure something wrong!");
        exit(0);
    }

    if (pthread_create(&thread_id, NULL, playback_thread, (void *)0)) {
        perror("Create au_thread[sample_send_audio_bitstream] failed");
        exit(1);
    }

    printf("Enter q to quit\n");
    while (1) {
        printf("Enter q to exit\n");
        key = getchar();
        if (key == 'q') {
            pb_exit = 1;
            break;
        }
    }

    pthread_join(thread_id, NULL);

    gm_unbind(bindfd);
    gm_apply(groupfd);
    gm_delete_obj(file_object);
    gm_delete_obj(audio_render_object);
    gm_delete_groupfd(groupfd);
    gm_release();
    return 0;
}

