#include "eye_motion.h"
#include "eye_vision.h"

#include <math.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eye_motion";

/* Lid targets tracked by control_ud_and_lids(), read back by open_lid(). */
static float s_tl_target = 90.0f, s_tr_target = 90.0f;
static float s_bl_target = 90.0f, s_br_target = 90.0f;

static float s_x_target = 90.0f, s_y_target = 90.0f;
static float s_lid_trim = 0.5f;

static eye_mode_t s_mode = EYE_MODE_AUTO;
static bool       s_blink_requested;

static const char *s_mode_names[] = { "tracking", "auto", "manual", "calibration" };

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Limits may run backwards (BL, TR are mirrored), so clamping has to respect
 * whichever end is numerically lower. */
static float clamp_to_limits(eye_servo_id_t id, float v)
{
    eye_limits_t l = eye_servo_limits(id);
    float lo = fminf(l.min, l.max);
    float hi = fmaxf(l.min, l.max);
    return clampf(v, lo, hi);
}

static uint32_t rand_range(uint32_t lo, uint32_t hi)
{
    return lo + (esp_random() % (hi - lo + 1));
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* --------------------------------------------------------- primitives ---- */

esp_err_t eye_motion_calibrate(void)
{
    for (int i = 0; i < EYE_SERVO_COUNT; i++) eye_servo_write((eye_servo_id_t)i, 90.0f);
    return ESP_OK;
}

esp_err_t eye_motion_neutral(void)
{
    for (int i = 0; i < EYE_SERVO_COUNT; i++) eye_servo_write((eye_servo_id_t)i, 90.0f);
    const eye_servo_id_t lids[] = { EYE_TL, EYE_BL, EYE_TR, EYE_BR };
    for (int i = 0; i < 4; i++) {
        eye_servo_write(lids[i], eye_servo_limits(lids[i]).max);   /* open */
    }
    return ESP_OK;
}

esp_err_t eye_motion_resume_to_neutral(void)
{
    float from[EYE_SERVO_COUNT], to[EYE_SERVO_COUNT];
    bool  known = true;

    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        from[i] = eye_servo_read((eye_servo_id_t)i);
        if (isnan(from[i])) known = false;
        to[i] = 90.0f;
    }
    const eye_servo_id_t lids[] = { EYE_TL, EYE_BL, EYE_TR, EYE_BR };
    for (int i = 0; i < 4; i++) {
        to[lids[i]] = eye_servo_limits(lids[i]).max;   /* open */
    }

    if (!known) {
        ESP_LOGI(TAG, "cold start — nothing to resume from, going to neutral");
        return eye_motion_neutral();
    }

    const int period_ms = 1000 / EYE_MOTION_TICK_HZ;
    const int steps = (EYE_RESUME_MS / period_ms) > 0
                      ? (EYE_RESUME_MS / period_ms) : 1;
    ESP_LOGI(TAG, "warm start — easing to neutral over %d ms", EYE_RESUME_MS);

    for (int s = 1; s <= steps; s++) {
        float k = (float)s / (float)steps;
        for (int i = 0; i < EYE_SERVO_COUNT; i++) {
            eye_servo_write((eye_servo_id_t)i, from[i] + (to[i] - from[i]) * k);
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }

    /* Leave the tracked lid targets consistent with where the lids ended up,
     * so the first open_lid() after a blink doesn't jump. */
    s_tl_target = to[EYE_TL];
    s_bl_target = to[EYE_BL];
    s_tr_target = to[EYE_TR];
    s_br_target = to[EYE_BR];
    return ESP_OK;
}

esp_err_t eye_motion_engage(void)
{
    ESP_RETURN_ON_ERROR(eye_servo_engage(), TAG, "engage");
    /* Unavoidably a move: the mechanism has been limp, so wherever it sagged
     * to is where this starts from, and there is nothing to read back. */
    return eye_motion_neutral();
}

esp_err_t eye_motion_blink_now(void)
{
    const eye_servo_id_t lids[] = { EYE_TL, EYE_TR, EYE_BL, EYE_BR };
    for (int i = 0; i < 4; i++) {
        eye_servo_write(lids[i], eye_servo_limits(lids[i]).min);   /* closed */
    }
    return ESP_OK;
}

esp_err_t eye_motion_open_lid(void)
{
    eye_servo_write(EYE_TL, s_tl_target);
    eye_servo_write(EYE_TR, s_tr_target);
    eye_servo_write(EYE_BL, s_bl_target);
    eye_servo_write(EYE_BR, s_br_target);
    return ESP_OK;
}

/*
 * Carried over verbatim from control_ud_and_lids() in main.py. The 0.8 and 0.4
 * coefficients are what make the upper lids track gaze more strongly than the
 * lower ones — that asymmetry is most of what reads as "alive".
 */
esp_err_t eye_motion_control_ud_and_lids(float ud_angle)
{
    eye_limits_t ud = eye_servo_limits(EYE_UD);
    eye_limits_t tl = eye_servo_limits(EYE_TL);
    eye_limits_t tr = eye_servo_limits(EYE_TR);
    eye_limits_t bl = eye_servo_limits(EYE_BL);
    eye_limits_t br = eye_servo_limits(EYE_BR);

    /* A calibration that left UD's endpoints equal would divide by zero here
     * and push inf/NaN into all five writes below. Fall back to mid-travel,
     * which reads as a neutral face rather than lids slammed to one end. */
    float ud_span = ud.max - ud.min;
    float progress;
    if (fabsf(ud_span) < 1e-6f) {
        static bool warned;
        if (!warned) { warned = true; ESP_LOGW(TAG, "UD limits are equal — lid tracking disabled"); }
        progress = 0.5f;
    } else {
        progress = (ud_angle - ud.min) / ud_span;
    }

    s_tl_target = tl.max - ((tl.max - tl.min) * (0.8f * (1.0f - progress)));
    s_tr_target = tr.max + ((tr.min - tr.max) * (0.8f * (1.0f - progress)));
    s_bl_target = bl.max + ((bl.min - bl.max) * (0.4f * progress));
    s_br_target = br.max - ((br.max - br.min) * (0.4f * progress));

    s_y_target = ud_angle;

    eye_servo_write(EYE_UD, ud_angle);
    eye_servo_write(EYE_TL, s_tl_target);
    eye_servo_write(EYE_TR, s_tr_target);
    eye_servo_write(EYE_BL, s_bl_target);
    eye_servo_write(EYE_BR, s_br_target);
    return ESP_OK;
}

/*
 * Was update_eyelid_limits(trim_value) driven by the trim pot. Same ranges,
 * now fed a normalized 0..1 from the web UI.
 */
esp_err_t eye_motion_set_lid_trim(float progress)
{
    progress = clampf(progress, 0.0f, 1.0f);
    s_lid_trim = progress;

    const float tl_range[2] = { 130.0f, 170.0f };
    const float br_range[2] = { 130.0f, 170.0f };
    const float bl_range[2] = {  50.0f,  10.0f };
    const float tr_range[2] = {  50.0f,  10.0f };

    eye_servo_set_limits(EYE_TL, (eye_limits_t){ 90.0f, tl_range[0] + (tl_range[1] - tl_range[0]) * progress });
    eye_servo_set_limits(EYE_BR, (eye_limits_t){ 90.0f, br_range[0] + (br_range[1] - br_range[0]) * progress });
    eye_servo_set_limits(EYE_BL, (eye_limits_t){ 90.0f, bl_range[0] + (bl_range[1] - bl_range[0]) * progress });
    eye_servo_set_limits(EYE_TR, (eye_limits_t){ 90.0f, tr_range[0] + (tr_range[1] - tr_range[0]) * progress });
    return ESP_OK;
}

float eye_motion_get_lid_trim(void) { return s_lid_trim; }

esp_err_t eye_motion_look(float lr_angle, float ud_angle)
{
    s_x_target = clamp_to_limits(EYE_LR, lr_angle);
    eye_servo_write(EYE_LR, s_x_target);
    return eye_motion_control_ud_and_lids(clamp_to_limits(EYE_UD, ud_angle));
}

esp_err_t eye_motion_nudge(float d_lr, float d_ud)
{
    s_x_target = clamp_to_limits(EYE_LR, s_x_target + d_lr);
    s_y_target = clamp_to_limits(EYE_UD, s_y_target + d_ud);
    eye_servo_write(EYE_LR, s_x_target);
    return ESP_OK;   /* UD is applied by the loop, so lids stay in step */
}

esp_err_t eye_motion_request_blink(void) { s_blink_requested = true; return ESP_OK; }

float eye_motion_target_lr(void) { return s_x_target; }
float eye_motion_target_ud(void) { return s_y_target; }

/* --------------------------------------------------------- mode machine -- */

const char *eye_motion_mode_name(eye_mode_t mode)
{
    return (mode <= EYE_MODE_CALIBRATION) ? s_mode_names[mode] : "?";
}

int eye_motion_mode_from_name(const char *name)
{
    if (name == NULL) return -1;
    for (int i = 0; i < 4; i++) {
        if (strcmp(name, s_mode_names[i]) == 0) return i;
    }
    return -1;
}

eye_mode_t eye_motion_get_mode(void) { return s_mode; }

esp_err_t eye_motion_set_mode(eye_mode_t mode)
{
    if (mode == s_mode) return ESP_OK;
    s_mode = mode;
    /* Every mode change in the original ended with neutral() and a cleared
     * blink phase. Keep that — it is what stops a half-finished blink from
     * leaving the lids shut. Calibration seeds 90° instead, once, on entry:
     * the loop must not keep rewriting it, or direct writes cannot stick. */
    if (mode == EYE_MODE_CALIBRATION) {
        eye_motion_calibrate();
    } else {
        eye_motion_neutral();
    }
    s_blink_requested = false;
    ESP_LOGI(TAG, "mode -> %s", eye_motion_mode_name(mode));
    return ESP_OK;
}

static void motion_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / EYE_MOTION_TICK_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    enum { BLINK_IDLE, BLINK_CLOSED, BLINK_OPENING } blink_phase = BLINK_IDLE;
    int64_t blink_until   = 0;
    int64_t next_blink_at = now_ms() + rand_range(EYE_BLINK_GAP_MIN_MS, EYE_BLINK_GAP_MAX_MS);
    int64_t next_auto_at  = 0;

    for (;;) {
        int64_t t = now_ms();

        /* Blink runs in every mode except calibration, where the whole point is
         * that nothing moves off 90°. */
        if (s_mode != EYE_MODE_CALIBRATION) {
            if (blink_phase == BLINK_IDLE && (s_blink_requested || t >= next_blink_at)) {
                s_blink_requested = false;
                blink_phase = BLINK_CLOSED;
                blink_until = t + EYE_BLINK_CLOSED_MS;
                eye_motion_blink_now();
            } else if (blink_phase == BLINK_CLOSED && t >= blink_until) {
                blink_phase = BLINK_OPENING;
                blink_until = t + EYE_BLINK_OPENING_MS;
                eye_motion_open_lid();
            } else if (blink_phase == BLINK_OPENING && t >= blink_until) {
                blink_phase = BLINK_IDLE;
                next_blink_at = t + rand_range(EYE_BLINK_GAP_MIN_MS, EYE_BLINK_GAP_MAX_MS);
            }
        }

        switch (s_mode) {
        case EYE_MODE_TRACKING: {
            eye_vision_offset_t off;
            if (eye_vision_poll(&off) == ESP_OK && !off.is_static) {
                if (fabsf(off.x) > EYE_VISION_DEADZONE) {
                    /* map(x, -110..110, +factor..-factor) — sign flip is
                     * deliberate: the camera's +x is the eye's left. */
                    float adj = off.x / 110.0f * -EYE_VISION_ADJ_FACTOR;
                    eye_motion_nudge(adj, 0.0f);
                }
                if (fabsf(off.y) > EYE_VISION_DEADZONE) {
                    float adj = off.y / 110.0f * -EYE_VISION_ADJ_FACTOR;
                    s_y_target = clamp_to_limits(EYE_UD, s_y_target + adj);
                }
                if (blink_phase == BLINK_IDLE) {
                    eye_motion_control_ud_and_lids(s_y_target);
                }
            }
            break;
        }

        case EYE_MODE_AUTO:
            if (t >= next_auto_at) {
                uint32_t command = rand_range(0, 2);
                if (command == 0) {
                    s_blink_requested = true;
                    next_auto_at = t + 300;
                } else {
                    eye_limits_t ud = eye_servo_limits(EYE_UD);
                    eye_limits_t lr = eye_servo_limits(EYE_LR);
                    float ud_lo = fminf(ud.min, ud.max), ud_hi = fmaxf(ud.min, ud.max);
                    float lr_lo = fminf(lr.min, lr.max), lr_hi = fmaxf(lr.min, lr.max);
                    if (command == 1) s_blink_requested = true;
                    eye_motion_control_ud_and_lids((float)rand_range((uint32_t)ud_lo, (uint32_t)ud_hi));
                    s_x_target = (float)rand_range((uint32_t)lr_lo, (uint32_t)lr_hi);
                    eye_servo_write(EYE_LR, s_x_target);
                    next_auto_at = t + ((command == 1) ? rand_range(300, 1000)
                                                       : rand_range(200, 400));
                }
            }
            break;

        case EYE_MODE_MANUAL:
            /* eye_web writes targets directly; nothing to do per tick beyond
             * the blink state machine above. */
            break;

        case EYE_MODE_CALIBRATION:
            /* Deliberately nothing. Entering the mode already put every servo
             * at 90°; calling eye_motion_calibrate() per tick here would
             * rewrite that 100 times a second and revert every /api/servo
             * write within 10 ms — which is the only thing this mode exists
             * to allow. Nothing else moves either: the blink state machine is
             * skipped in calibration above. */
            break;
        }

        xTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t eye_motion_start(void)
{
    if (xTaskCreate(motion_task, "eye_motion", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "motion task running at %d Hz", EYE_MOTION_TICK_HZ);
    return ESP_OK;
}
