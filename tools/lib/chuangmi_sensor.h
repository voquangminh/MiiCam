#ifndef chuangmi_sensor_h
#define chuangmi_sensor_h

#include <gmlib.h>

/* Capture-side (SoC) image orientation, independent from ISP328 mirror/flip */
int sensor_cap_flip_set(int h_flip, int v_flip);

/* tamper_sensitive_b = 0 disables tamper detection */
int sensor_tamper_set(int sensitive_b, int threshold, int sensitive_h);
int sensor_tamper_disable(void);

int sensor_motion_param_set(unsigned int id, unsigned int value);

typedef void (*sensor_notify_cb)(int obj_type, int vch, int notify, void *user);

/* notify: GM_NOTIFY_SIGNAL_LOSS / GM_NOTIFY_SIGNAL_PRESENT /
 *         GM_NOTIFY_FRAMERATE_CHANGE / GM_NOTIFY_HW_CONFIG_CHANGE /
 *         GM_NOTIFY_TAMPER_ALARM / GM_NOTIFY_TAMPER_ALARM_RELEASE /
 *         GM_NOTIFY_AUDIO_BUFFER_UNDERRUN / GM_NOTIFY_AUDIO_BUFFER_OVERRUN */
int sensor_notify_register(int notify, sensor_notify_cb cb, void *user);
int sensor_notify_unregister(int notify);

/* Requires a bindfd whose capture object was created with enable_mv_data = 1.
 * Returns md_len (>0), 0 if no valid data yet, negative on error. */
int sensor_md_poll(void *bindfd, char *md_buf, int md_buf_len);

int sensor_init(void);
int sensor_end(void);
int sensor_is_initialized(void);
unsigned int sensor_chipid(void);
const gm_cap_sys_info_t *sensor_get_cap_info(int vch);

#endif
