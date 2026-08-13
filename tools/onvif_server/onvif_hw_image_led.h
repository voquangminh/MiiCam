#ifndef ONVIF_HW_IMAGE_LED_H
#define ONVIF_HW_IMAGE_LED_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct onvif_image_settings {
    float brightness;
    float contrast;
    float saturation;
    float sharpness;
    int ir_cut;       /* 0=night/IR path, 1=day/visible path */
} onvif_image_settings_t;

int hw_isp_get(const char *variable, int *value);
int hw_isp_set(const char *variable, int value);
int hw_image_get(onvif_image_settings_t *settings);
int hw_image_set(const onvif_image_settings_t *settings,
                 unsigned int update_mask);

#define HW_IMAGE_BRIGHTNESS (1U << 0)
#define HW_IMAGE_CONTRAST   (1U << 1)
#define HW_IMAGE_SATURATION (1U << 2)
#define HW_IMAGE_SHARPNESS  (1U << 3)
#define HW_IMAGE_IRCUT      (1U << 4)

int hw_blue_led_set(int mode);   /* 0=off, 1=on; blink unsupported */
int hw_yellow_led_set(int mode); /* 0=off, 1=on; blink unsupported */
int hw_ir_cut_set(int enabled);
int hw_ir_cut_get(int *enabled);

#ifdef __cplusplus
}
#endif
#endif
