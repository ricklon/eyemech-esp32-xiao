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

static const char *s_mode_names[] = { "tracking", "auto", "manual", "calibration", "anim" };

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

/*
 * Where a lid sits when "open", after the trim.
 *
 * The calibrated limits describe the mechanism: .min is fully closed, .max is
 * fully open, and either may be numerically larger (BL and TR are mirrored).
 * The trim then picks a point between half-open and fully open, so it scales
 * within the calibration instead of replacing it.
 *
 * The 0.5 floor is what makes this reproduce the original: on the default
 * table it lands on exactly the hardcoded ranges the trim pot used to write
 * for TL, BL and TR. BR differs slightly — see docs/decisions.md.
 */
static float lid_open(eye_servo_id_t id)
{
    eye_limits_t l = eye_servo_limits(id);
    return l.min + (l.max - l.min) * (0.5f + 0.5f * s_lid_trim);
}

static uint32_t rand_range(uint32_t lo, uint32_t hi)
{
    return lo + (esp_random() % (hi - lo + 1));
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* --------------------------------------------------------- primitives ---- */

/*
 * "Everything to 90" is the horn-fitting pose from the original. It has to be
 * clamped now: once a lid's closed end is measured, 90 is not necessarily
 * inside its range. Measured on this build BL closes at 97 and BR at 78, so a
 * raw 90 would drive both 7-12 degrees PAST closed, into the opposing lid --
 * two powered MG90S pushing against each other with nothing able to report it.
 */
esp_err_t eye_motion_calibrate(void)
{
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        eye_servo_write((eye_servo_id_t)i, clamp_to_limits((eye_servo_id_t)i, 90.0f));
    }
    return ESP_OK;
}

esp_err_t eye_motion_neutral(void)
{
    /* Gaze to centre, lids to their trimmed open position. The lids are written
     * once, directly to where they belong — the original wrote 90 to all six
     * first and then corrected the lids, which now means a transient command
     * outside their measured range. */
    eye_servo_write(EYE_LR, clamp_to_limits(EYE_LR, 90.0f));
    eye_servo_write(EYE_UD, clamp_to_limits(EYE_UD, 90.0f));

    const eye_servo_id_t lids[] = { EYE_TL, EYE_BL, EYE_TR, EYE_BR };
    for (int i = 0; i < 4; i++) {
        eye_servo_write(lids[i], lid_open(lids[i]));
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
        to[lids[i]] = lid_open(lids[i]);
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

    /* Calibration mode is one servo at a time by definition, and bringing up a
     * new axis is exactly when commanding all six at once is most likely to
     * drive something into a hard stop. Release left every channel's full-off
     * bit set, so leaving them alone here means no pulses at all until an
     * explicit write -- which is what makes powering the servo rail safe. */
    if (s_mode == EYE_MODE_CALIBRATION) {
        ESP_LOGI(TAG, "engaged in calibration — no channel driven until you write one");
        return ESP_OK;
    }

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

    /* Each lid interpolates from open toward closed. The four expressions were
     * written two ways in the original (max - (max-min)*k and max + (min-max)*k)
     * which are the same thing; unified here, with the open end now coming from
     * lid_open() so the trim no longer has to rewrite the limits. Substituting
     * the calibrated .max for lid_open() recovers the original exactly.
     *
     * 0.8 for the upper lids, 0.4 for the lower ones. These are the character
     * of the face and are unchanged — see CLAUDE.md. */
    float tl_open = lid_open(EYE_TL), tr_open = lid_open(EYE_TR);
    float bl_open = lid_open(EYE_BL), br_open = lid_open(EYE_BR);

    s_tl_target = tl_open + (tl.min - tl_open) * (0.8f * (1.0f - progress));
    s_tr_target = tr_open + (tr.min - tr_open) * (0.8f * (1.0f - progress));
    s_bl_target = bl_open + (bl.min - bl_open) * (0.4f * progress);
    s_br_target = br_open + (br.min - br_open) * (0.4f * progress);

    s_y_target = ud_angle;

    eye_servo_write(EYE_UD, ud_angle);
    eye_servo_write(EYE_TL, s_tl_target);
    eye_servo_write(EYE_TR, s_tr_target);
    eye_servo_write(EYE_BL, s_bl_target);
    eye_servo_write(EYE_BR, s_br_target);
    return ESP_OK;
}

/*
 * Was update_eyelid_limits(trim_value), driven by the trim pot, which rewrote
 * the four lid entries in servo_limits outright.
 *
 * It cannot do that any more. In the MicroPython build the pot was the only
 * source of lid limits, so overwriting them was harmless; here servo_limits is
 * also the calibration table that eye_servo_save() persists, and having the
 * trim write into it meant one drag of the openness slider replaced measured
 * endpoints with hardcoded numbers, which the next Save then committed to NVS.
 *
 * The trim is now just a stored 0..1 that lid_open() applies. Limits belong to
 * calibration; openness scales within them.
 */
esp_err_t eye_motion_set_lid_trim(float progress)
{
    s_lid_trim = clampf(progress, 0.0f, 1.0f);
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


/* ------------------------------------------------------------- animations */

typedef struct {
    const char        *name;
    const char        *desc;
    const eye_frame_t *frames;
    int                count;
} eye_anim_t;

/* Frames are normalised: 0 is an axis's `min` end, 1 its `max`. For a lid that
 * is 0 closed, 1 open, whichever numeric direction that is on this build — so
 * these sequences survive recalibration and work on the inverted lids. */

static const eye_frame_t s_frames_look[] = {
    { 0.50f, 0.50f, 1.00f, 400 },   /* open, centred                */
    { 0.00f, NAN,   NAN,   700 },   /* look left                    */
    { 1.00f, NAN,   NAN,  1100 },   /* sweep across to the right    */
    { 0.50f, NAN,   NAN,   600 },   /* back to centre               */
    { NAN,   NAN,   0.00f, 400 },   /* close                        */
};

/* A circle in gaze space. lid is NAN throughout so the lids keep tracking UD
 * through control_ud_and_lids() — the eyes hood as they pass the bottom and
 * widen over the top, which is most of what makes it read as a roll rather
 * than a mechanical sweep. */
static const eye_frame_t s_frames_roll[] = {
    { 0.50f, 0.50f, 1.00f, 350 },   /* open, centred */
    { 0.50f, 0.92f, NAN,   350 },   /* up            */
    { 0.80f, 0.80f, NAN,   200 },
    { 0.92f, 0.50f, NAN,   200 },   /* right         */
    { 0.80f, 0.20f, NAN,   200 },
    { 0.50f, 0.08f, NAN,   200 },   /* down          */
    { 0.20f, 0.20f, NAN,   200 },
    { 0.08f, 0.50f, NAN,   200 },   /* left          */
    { 0.20f, 0.80f, NAN,   200 },
    { 0.50f, 0.92f, NAN,   200 },   /* back to the top */
    { 0.50f, 0.50f, NAN,   400 },   /* settle centred  */
};

#define ANIM(id, d) { #id, d, s_frames_##id, \
                      (int)(sizeof(s_frames_##id) / sizeof(s_frames_##id[0])) }

static const eye_anim_t s_anims[] = {
    ANIM(look, "open, look left and right, close"),
    ANIM(roll, "roll the eyes in a full circle"),
};
#define ANIM_COUNT ((int)(sizeof(s_anims) / sizeof(s_anims[0])))

static const eye_anim_t *s_anim;
static int       s_anim_frame;
static int64_t   s_anim_started;
static float     s_from_lr, s_from_ud, s_from_lid;
static float     s_lid_now = NAN;          /* last commanded lid, NAN = coupled */
static eye_mode_t s_anim_return = EYE_MODE_AUTO;

static float norm01(eye_servo_id_t id, float deg)
{
    eye_limits_t l = eye_servo_limits(id);
    float span = l.max - l.min;
    return (fabsf(span) < 1e-6f) ? 0.5f : (deg - l.min) / span;
}

static float denorm(eye_servo_id_t id, float v)
{
    eye_limits_t l = eye_servo_limits(id);
    return l.min + (l.max - l.min) * v;
}

static float lerp01(float from, float to, float k)
{
    if (isnan(to))   return NAN;    /* frame says hold */
    if (isnan(from)) return to;     /* nothing to travel from */
    return from + (to - from) * k;
}

static void anim_apply(float lr01, float ud01, float lid01)
{
    if (!isnan(lr01)) {
        s_x_target = clamp_to_limits(EYE_LR, denorm(EYE_LR, lr01));
        eye_servo_write(EYE_LR, s_x_target);
    }
    if (!isnan(ud01)) {
        s_y_target = clamp_to_limits(EYE_UD, denorm(EYE_UD, ud01));
    }

    if (isnan(lid01)) {
        /* Let the usual coupling drive the lids off the gaze. */
        eye_motion_control_ud_and_lids(s_y_target);
        s_lid_now = NAN;
    } else {
        eye_servo_write(EYE_UD, s_y_target);
        const eye_servo_id_t lids[] = { EYE_TL, EYE_BL, EYE_TR, EYE_BR };
        for (int i = 0; i < 4; i++) {
            eye_servo_write(lids[i], denorm(lids[i], lid01));
        }
        s_lid_now = lid01;
    }
}

static void anim_begin_frame(int64_t t)
{
    s_anim_started = t;
    s_from_lr  = norm01(EYE_LR, s_x_target);
    s_from_ud  = norm01(EYE_UD, s_y_target);
    s_from_lid = s_lid_now;
}

esp_err_t eye_motion_play(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < ANIM_COUNT; i++) {
        if (strcmp(name, s_anims[i].name) != 0) continue;

        /* Remember where to go back to, but never stack animations. */
        if (s_mode != EYE_MODE_ANIM) s_anim_return = s_mode;
        s_anim = &s_anims[i];
        s_anim_frame = 0;
        s_blink_requested = false;
        s_mode = EYE_MODE_ANIM;          /* deliberately not set_mode(): that
                                          * would run neutral() and jump */
        anim_begin_frame(now_ms());
        ESP_LOGI(TAG, "playing '%s' (%d frames)", s_anim->name, s_anim->count);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

const char *const *eye_motion_anim_names(void)
{
    static const char *names[ANIM_COUNT + 1];
    for (int i = 0; i < ANIM_COUNT; i++) names[i] = s_anims[i].name;
    names[ANIM_COUNT] = NULL;
    return names;
}

const char *eye_motion_anim_desc(const char *name)
{
    for (int i = 0; i < ANIM_COUNT; i++) {
        if (strcmp(name, s_anims[i].name) == 0) return s_anims[i].desc;
    }
    return "";
}

bool eye_motion_anim_busy(void) { return s_mode == EYE_MODE_ANIM && s_anim != NULL; }

/* One tick of the player. Returns false when the sequence is done. */
static bool anim_tick(int64_t t)
{
    if (s_anim == NULL) return false;

    const eye_frame_t *f = &s_anim->frames[s_anim_frame];
    int64_t elapsed = t - s_anim_started;
    float k = (f->ms == 0) ? 1.0f : (float)elapsed / (float)f->ms;
    if (k > 1.0f) k = 1.0f;

    anim_apply(lerp01(s_from_lr,  f->lr,  k),
               lerp01(s_from_ud,  f->ud,  k),
               lerp01(s_from_lid, f->lid, k));

    if (k < 1.0f) return true;

    if (++s_anim_frame >= s_anim->count) {
        s_anim = NULL;
        return false;
    }
    anim_begin_frame(t);
    return true;
}

/* --------------------------------------------------------- mode machine -- */

const char *eye_motion_mode_name(eye_mode_t mode)
{
    return (mode <= EYE_MODE_ANIM) ? s_mode_names[mode] : "?";
}

int eye_motion_mode_from_name(const char *name)
{
    if (name == NULL) return -1;
    for (int i = 0; i < 5; i++) {
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
        if (s_mode != EYE_MODE_CALIBRATION && s_mode != EYE_MODE_ANIM) {
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

        case EYE_MODE_ANIM:
            if (!anim_tick(t)) {
                ESP_LOGI(TAG, "animation done — back to %s",
                         eye_motion_mode_name(s_anim_return));
                s_mode = EYE_MODE_ANIM;      /* force set_mode to act */
                eye_motion_set_mode(s_anim_return);
            }
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
