#pragma once
/*
 * eye_servo — port of micropython/servo.py, plus the servo_limits table that
 * lived at the top of main.py and the NVS persistence the MicroPython version
 * never had (it reflashed instead).
 *
 * Angles in degrees are the currency everywhere above this layer, exactly as in
 * the original. Microseconds exist here and in pca9685 only.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "pca9685.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PCA9685 channel order, from docs/WIRING.md. */
typedef enum {
    EYE_LR = 0,   /* ch 0 — eye left/right   */
    EYE_UD,       /* ch 1 — eye up/down      */
    EYE_TL,       /* ch 2 — top-left lid     */
    EYE_BL,       /* ch 3 — bottom-left lid  */
    EYE_TR,       /* ch 4 — top-right lid    */
    EYE_BR,       /* ch 5 — bottom-right lid */
    EYE_SERVO_COUNT
} eye_servo_id_t;

/*
 * Travel limits. IMPORTANT: max may be numerically SMALLER than min — that is
 * how BL and TR encode being mounted mirrored relative to their partners. Never
 * assume min < max, never "normalize" this by swapping them.
 */
typedef struct {
    float min;
    float max;
} eye_limits_t;

/* Per-servo pulse mapping. trim_us is the mechanical centring offset — use it
 * rather than fudging the limits. */
typedef struct {
    uint16_t min_us;      /* default 500  */
    uint16_t max_us;      /* default 2500 */
    float    min_angle;   /* default 0    */
    float    max_angle;   /* default 180  */
    int16_t  trim_us;
} eye_servo_cfg_t;

esp_err_t eye_servo_init(pca9685_t *dev);

/* Clamps to [min_angle, max_angle] rather than erroring, and skips redundant
 * writes — the motion loop rewrites identical lid targets constantly and I²C is
 * the bottleneck. Do not remove the skip. */
esp_err_t eye_servo_write(eye_servo_id_t id, float angle);

/* Last commanded angle, or NAN if never written. */
float eye_servo_read(eye_servo_id_t id);

/* Stop driving one servo / all servos. They go limp. */
esp_err_t eye_servo_release(eye_servo_id_t id);
esp_err_t eye_servo_release_all(void);

eye_limits_t eye_servo_limits(eye_servo_id_t id);
esp_err_t    eye_servo_set_limits(eye_servo_id_t id, eye_limits_t limits);

eye_servo_cfg_t eye_servo_cfg(eye_servo_id_t id);
esp_err_t       eye_servo_set_cfg(eye_servo_id_t id, eye_servo_cfg_t cfg);

/* Persist limits + cfg to NVS under namespace "eyemech". */
esp_err_t eye_servo_save(void);
esp_err_t eye_servo_load(void);
esp_err_t eye_servo_reset_defaults(void);

const char *eye_servo_name(eye_servo_id_t id);
int         eye_servo_from_name(const char *name);  /* -1 if unknown */

#ifdef __cplusplus
}
#endif
