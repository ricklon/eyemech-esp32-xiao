#pragma once
/*
 * eye_motion — everything above the servo layer: where the eyes are looking,
 * how they get there, and what they do when nobody is driving them.
 *
 * A single FreeRTOS task ticks at EYE_MOTION_TICK_HZ, eases the current pose
 * toward the target pose, and hands normalized values to eye_servo.
 */
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EYE_MOTION_TICK_HZ 50

typedef struct {
    float gaze_x;    /* -1 left  .. +1 right */
    float gaze_y;    /* -1 down  .. +1 up    */
    float lid_upper; /*  0 closed .. 1 open  */
    float lid_lower; /*  0 closed .. 1 open  */
} eye_pose_t;

typedef enum {
    EYE_MODE_IDLE = 0,   /* autonomous saccades + blinking */
    EYE_MODE_MANUAL,     /* web UI or API is driving        */
    EYE_MODE_CALIBRATE,  /* raw pulse control, easing off   */
} eye_mode_t;

esp_err_t eye_motion_start(void);

esp_err_t  eye_motion_set_mode(eye_mode_t mode);
eye_mode_t eye_motion_get_mode(void);

/* Manual mode: request a pose. The easing runs at `speed` (0..1, where 1 is
 * an immediate snap) — TODO(tuning): find values that read as lifelike. */
esp_err_t eye_motion_look_at(float gaze_x, float gaze_y, float speed);
esp_err_t eye_motion_set_lids(float upper, float lower, float speed);

/* One blink, then return to the prior lid position. */
esp_err_t eye_motion_blink(void);

eye_pose_t eye_motion_current_pose(void);
eye_pose_t eye_motion_target_pose(void);

#ifdef __cplusplus
}
#endif
