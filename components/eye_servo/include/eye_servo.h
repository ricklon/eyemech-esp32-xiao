#pragma once
/*
 * eye_servo — LEDC-based hobby-servo driver for the eye mechanism.
 *
 * One LEDC timer at 50 Hz drives up to EYE_AXIS_COUNT channels. Every axis
 * carries its own calibration (pulse-width endpoints + center + inversion)
 * so mechanical differences between linkages stay out of the motion layer.
 *
 * TODO(hardware): confirm the axis list below against the actual mechanism.
 * The six axes here are the common Nilheim-style layout; a 4-servo build
 * would drop the lower lids.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EYE_AXIS_PAN = 0,        /* both eyes left/right  */
    EYE_AXIS_TILT,           /* both eyes up/down     */
    EYE_AXIS_LID_UPPER_L,
    EYE_AXIS_LID_UPPER_R,
    EYE_AXIS_LID_LOWER_L,
    EYE_AXIS_LID_LOWER_R,
    EYE_AXIS_COUNT
} eye_axis_t;

/* Per-axis calibration, persisted in NVS under namespace "eyemech". */
typedef struct {
    uint16_t min_us;      /* pulse width at normalized -1.0 */
    uint16_t center_us;   /* pulse width at normalized  0.0 */
    uint16_t max_us;      /* pulse width at normalized +1.0 */
    bool     inverted;    /* flip sign before mapping       */
} eye_servo_cal_t;

/* Absolute safety rails — calibration is clamped into this window. */
#define EYE_SERVO_PULSE_MIN_US 500
#define EYE_SERVO_PULSE_MAX_US 2500
#define EYE_SERVO_FREQ_HZ      50

/* Bring up the LEDC timer and all channels. Loads calibration from NVS,
 * falling back to eye_servo_default_cal() for any axis without a stored blob. */
esp_err_t eye_servo_init(void);

/* Park every axis at its calibrated center and stop driving. */
esp_err_t eye_servo_release(void);

/* Command an axis in normalized units, -1.0 .. +1.0. Values outside the
 * range are clamped, never wrapped. */
esp_err_t eye_servo_set(eye_axis_t axis, float normalized);

/* Command a raw pulse width. Intended for the calibration UI only —
 * the motion layer should always go through eye_servo_set(). */
esp_err_t eye_servo_set_us(eye_axis_t axis, uint16_t pulse_us);

/* Last commanded value for an axis. */
float    eye_servo_get(eye_axis_t axis);
uint16_t eye_servo_get_us(eye_axis_t axis);

/* Calibration access. eye_servo_set_cal() applies immediately and holds the
 * value in RAM; call eye_servo_save_cal() to commit all axes to NVS. */
esp_err_t       eye_servo_set_cal(eye_axis_t axis, const eye_servo_cal_t *cal);
eye_servo_cal_t eye_servo_get_cal(eye_axis_t axis);
eye_servo_cal_t eye_servo_default_cal(eye_axis_t axis);
esp_err_t       eye_servo_save_cal(void);
esp_err_t       eye_servo_load_cal(void);

/* Human-readable axis name, stable across the API and web UI. */
const char *eye_servo_axis_name(eye_axis_t axis);

#ifdef __cplusplus
}
#endif
