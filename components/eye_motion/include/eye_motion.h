#pragma once
/*
 * eye_motion — port of the motion primitives and mode machine from
 * micropython/main.py. The arithmetic in control_ud_and_lids() and
 * update_eyelid_limits() is carried over unchanged; if you change a coefficient
 * here, say so in docs/decisions.md, because it changes how the face reads.
 *
 * Modes differ from the MicroPython build in one way: "controller" was driven by
 * three ADC pots and two switches. Those are gone — eye_web drives manual mode
 * and mode selection now.
 */
#include <stdbool.h>
#include "esp_err.h"
#include "eye_servo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wall-clock blink timing. The original used random.randrange(20000) over loop
 * iterations, which does not survive a different core at a different clock. */
#define EYE_BLINK_GAP_MIN_MS 2000
#define EYE_BLINK_GAP_MAX_MS 7000
#define EYE_BLINK_CLOSED_MS  70
#define EYE_BLINK_OPENING_MS 70

#define EYE_MOTION_TICK_HZ   100

/* How long the boot ramp from the recovered position to neutral takes. Long
 * enough that six servos moving at once look deliberate rather than startled;
 * short enough that boot is not annoying. */
#define EYE_RESUME_MS        600

typedef enum {
    EYE_MODE_TRACKING = 0,  /* follow the Grove Vision module, blink on a timer */
    EYE_MODE_AUTO,          /* random gaze and blink — no vision module present  */
    EYE_MODE_MANUAL,        /* eye_web is driving                                */
    EYE_MODE_CALIBRATION,   /* everything to 90° for fitting horns and linkages  */
} eye_mode_t;

esp_err_t eye_motion_start(void);

esp_err_t  eye_motion_set_mode(eye_mode_t mode);   /* transitions call neutral() */
eye_mode_t eye_motion_get_mode(void);
const char *eye_motion_mode_name(eye_mode_t mode);
int         eye_motion_mode_from_name(const char *name);  /* -1 if unknown */

/* --- primitives, one-for-one with the MicroPython functions --------------- */

esp_err_t eye_motion_calibrate(void);   /* all servos to 90°                    */
esp_err_t eye_motion_neutral(void);     /* 90°, then lids to their open limit   */

/* Boot path: ease from wherever the mechanism is actually holding to neutral.
 *
 * eye_servo_resume_from_hardware() must have run first. When it recovered a
 * position (warm reset — the PCA9685 kept driving through the reboot) this
 * interpolates over EYE_RESUME_MS instead of commanding neutral outright, which
 * would drive all six servos there at full speed. When nothing was recovered
 * (cold power-on, servos limp and genuinely unknown) it falls back to
 * eye_motion_neutral(), because there is no start point to ramp from. */
esp_err_t eye_motion_resume_to_neutral(void);
esp_err_t eye_motion_blink_now(void);   /* lids to their closed limit           */
esp_err_t eye_motion_open_lid(void);    /* lids back to their tracked targets   */

/* Move UD and have the four lids follow its position. */
esp_err_t eye_motion_control_ud_and_lids(float ud_angle);

/* Manual gaze, in servo degrees, clamped to each axis's limits. */
esp_err_t eye_motion_look(float lr_angle, float ud_angle);

/* Live trim of how wide the eyes open — the old trim pot, 0.0 .. 1.0.
 * Rewrites the four lid limits. */
esp_err_t eye_motion_set_lid_trim(float progress);
float     eye_motion_get_lid_trim(void);

/* Queue one blink; the state machine picks it up on the next tick. */
esp_err_t eye_motion_request_blink(void);

/* Nudge the gaze by a delta, clamped — how the vision tracker steers. */
esp_err_t eye_motion_nudge(float d_lr, float d_ud);

float eye_motion_target_lr(void);
float eye_motion_target_ud(void);

#ifdef __cplusplus
}
#endif
