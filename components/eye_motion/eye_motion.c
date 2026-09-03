#include "eye_motion.h"
#include "eye_servo.h"

#include <math.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eye_motion";

static eye_pose_t s_current = { 0.0f, 0.0f, 1.0f, 1.0f };
static eye_pose_t s_target  = { 0.0f, 0.0f, 1.0f, 1.0f };
static float      s_speed   = 0.15f;
static eye_mode_t s_mode    = EYE_MODE_IDLE;
static bool       s_blink_pending;

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float ease(float from, float to, float k)
{
    return from + (to - from) * k;
}

/* Push the current pose down to the servo layer.
 *
 * TODO(hardware): the lid mapping assumes both upper lids move together and
 * both lower lids move together. If the mechanism gives independent left/right
 * lids, widen eye_pose_t rather than special-casing here. */
static void apply_pose(const eye_pose_t *p)
{
    eye_servo_set(EYE_AXIS_PAN,  p->gaze_x);
    eye_servo_set(EYE_AXIS_TILT, p->gaze_y);

    /* Lids: 0..1 open maps onto -1..+1 servo travel. */
    float upper = p->lid_upper * 2.0f - 1.0f;
    float lower = p->lid_lower * 2.0f - 1.0f;
    eye_servo_set(EYE_AXIS_LID_UPPER_L, upper);
    eye_servo_set(EYE_AXIS_LID_UPPER_R, upper);
    eye_servo_set(EYE_AXIS_LID_LOWER_L, lower);
    eye_servo_set(EYE_AXIS_LID_LOWER_R, lower);
}

static float frand(void)
{
    return (float)esp_random() / (float)UINT32_MAX;
}

/* Idle behavior: hold a fixation point for a while, then saccade elsewhere,
 * blinking at irregular intervals. Deliberately simple — replace with
 * something better once the mechanism is moving. */
static void idle_tick(int64_t *next_saccade_ms, int64_t *next_blink_ms, int64_t now_ms)
{
    if (now_ms >= *next_saccade_ms) {
        s_target.gaze_x = frand() * 1.6f - 0.8f;
        s_target.gaze_y = frand() * 1.0f - 0.5f;
        s_speed = 0.35f;                        /* saccades are fast */
        *next_saccade_ms = now_ms + 800 + (int64_t)(frand() * 2500.0f);
    }
    if (now_ms >= *next_blink_ms) {
        s_blink_pending = true;
        *next_blink_ms = now_ms + 2000 + (int64_t)(frand() * 4000.0f);
    }
}

static void motion_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / EYE_MOTION_TICK_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    int64_t next_saccade_ms = 0;
    int64_t next_blink_ms   = 3000;
    int     blink_frames    = 0;
    float   blink_restore_u = 1.0f;
    float   blink_restore_l = 1.0f;

    for (;;) {
        int64_t now_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (s_mode == EYE_MODE_IDLE) {
            idle_tick(&next_saccade_ms, &next_blink_ms, now_ms);
        }

        if (s_blink_pending && blink_frames == 0) {
            s_blink_pending = false;
            blink_restore_u = s_target.lid_upper;
            blink_restore_l = s_target.lid_lower;
            blink_frames = EYE_MOTION_TICK_HZ / 6;   /* ~160 ms closed */
            s_target.lid_upper = 0.0f;
            s_target.lid_lower = 0.0f;
            s_speed = 0.6f;
        } else if (blink_frames > 0 && --blink_frames == 0) {
            s_target.lid_upper = blink_restore_u;
            s_target.lid_lower = blink_restore_l;
        }

        if (s_mode != EYE_MODE_CALIBRATE) {
            float k = clampf(s_speed, 0.01f, 1.0f);
            s_current.gaze_x    = ease(s_current.gaze_x,    s_target.gaze_x,    k);
            s_current.gaze_y    = ease(s_current.gaze_y,    s_target.gaze_y,    k);
            s_current.lid_upper = ease(s_current.lid_upper, s_target.lid_upper, k);
            s_current.lid_lower = ease(s_current.lid_lower, s_target.lid_lower, k);
            apply_pose(&s_current);
        }

        xTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t eye_motion_start(void)
{
    BaseType_t ok = xTaskCreate(motion_task, "eye_motion", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "motion task running at %d Hz", EYE_MOTION_TICK_HZ);
    return ESP_OK;
}

esp_err_t eye_motion_set_mode(eye_mode_t mode)
{
    s_mode = mode;
    if (mode == EYE_MODE_CALIBRATE) {
        ESP_LOGW(TAG, "calibrate mode — easing disabled, raw pulses only");
    }
    return ESP_OK;
}

eye_mode_t eye_motion_get_mode(void) { return s_mode; }

esp_err_t eye_motion_look_at(float gaze_x, float gaze_y, float speed)
{
    s_target.gaze_x = clampf(gaze_x, -1.0f, 1.0f);
    s_target.gaze_y = clampf(gaze_y, -1.0f, 1.0f);
    s_speed = clampf(speed, 0.01f, 1.0f);
    return ESP_OK;
}

esp_err_t eye_motion_set_lids(float upper, float lower, float speed)
{
    s_target.lid_upper = clampf(upper, 0.0f, 1.0f);
    s_target.lid_lower = clampf(lower, 0.0f, 1.0f);
    s_speed = clampf(speed, 0.01f, 1.0f);
    return ESP_OK;
}

esp_err_t eye_motion_blink(void)
{
    s_blink_pending = true;
    return ESP_OK;
}

eye_pose_t eye_motion_current_pose(void) { return s_current; }
eye_pose_t eye_motion_target_pose(void)  { return s_target; }
