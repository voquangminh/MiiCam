#ifndef GM8136_MOTOR_DRIVER_H
#define GM8136_MOTOR_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gm8136_motor_status {
    int horizontal; /* 0..31 */
    int vertical;   /* 0..15 */
} gm8136_motor_status_t;

int gm8136_motor_open(const char *device);
void gm8136_motor_close(void);
int gm8136_motor_get_status(gm8136_motor_status_t *status);
int gm8136_motor_move_relative(int dx, int dy);
int gm8136_motor_move_absolute(int x, int y);
int gm8136_motor_set_logical_position(int x, int y);

#ifdef __cplusplus
}
#endif
#endif
