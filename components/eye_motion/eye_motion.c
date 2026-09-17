#include "eye_motion.h"
#include "eye_vision.h"

#include <math.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eye_motion";

/* Lid targets tracked by control_ud_and_lids(), read back by open_lid(). */
static float s_tl_target = 90.0f, s_tr_target = 90.0f;
static float s_bl_target = 90.0f, s_br_target = 90.0f;

static float s_x_target = 90.0f, s_y_target = 90.0f;
/* Chosen on hardware 2026-09-05: at level gaze the 0.8 coefficient hoods the
 * upper lids 40% from open, so a trim of 0.5 leaves them only ~45% open, which
 * reads as sleepy. 0.85 puts them near 56%. Persisted, so this is only the
 * value a board with empty NVS starts from. */
#define LID_TRIM_DEFAULT 0.85f
#define NVS_NAMESPACE    "eyemech"
#define NVS_KEY_TRIM     "lid_trim"
#define NVS_KEY_COEFF    "lid_coeff"
#define NVS_KEY_BLINK    "blink_hold"

/* How strongly each lid pair tracks vertical gaze. The reference values are
 * 0.8 upper / 0.4 lower, and the asymmetry between them is most of what reads
 * as alive — the upper lids hooding with a downward look is the expressive
 * part. Runtime-tunable so it can be dialled while watching the mechanism
 * rather than guessed at; whatever it settles on belongs in docs/decisions.md. */
#define LID_COEFF_UPPER_DEFAULT 0.8f
#define LID_COEFF_LOWER_DEFAULT 0.4f

static float s_lid_trim = LID_TRIM_DEFAULT;
static float s_coeff_upper = LID_COEFF_UPPER_DEFAULT;
static float s_coeff_lower = LID_COEFF_LOWER_DEFAULT;
static int   s_blink_hold_ms = EYE_BLINK_CLOSED_MS;

static eye_mode_t s_mode = EYE_MODE_AUTO;
static bool       s_blink_requested;

/* Indexed by eye_mode_t. */
static const char *s_mode_names[] = { "tracking", "auto", "manual", "calibration", "anim", "standby" };
#define MODE_COUNT (int)(sizeof(s_mode_names) / sizeof(s_mode_names[0]))

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
    if (s_mode == EYE_MODE_CALIBRATION || s_mode == EYE_MODE_STANDBY) {
        ESP_LOGI(TAG, "engaged in %s — no channel driven until something writes one",
                 eye_motion_mode_name(s_mode));
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
     * The upper/lower coefficients are the character of the face — see
     * CLAUDE.md — and are tunable at runtime rather than compiled in. */
    float tl_open = lid_open(EYE_TL), tr_open = lid_open(EYE_TR);
    float bl_open = lid_open(EYE_BL), br_open = lid_open(EYE_BR);

    s_tl_target = tl_open + (tl.min - tl_open) * (s_coeff_upper * (1.0f - progress));
    s_tr_target = tr_open + (tr.min - tr_open) * (s_coeff_upper * (1.0f - progress));
    s_bl_target = bl_open + (bl.min - bl_open) * (s_coeff_lower * progress);
    s_br_target = br_open + (br.min - br_open) * (s_coeff_lower * progress);

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

/* Stored under its own key rather than inside eye_servo's calibration blob:
 * adding a field to that struct changes its size, and eye_servo_load() rejects
 * a blob whose length does not match, which would silently reset every measured
 * limit. */
float eye_motion_get_coeff_upper(void) { return s_coeff_upper; }
float eye_motion_get_coeff_lower(void) { return s_coeff_lower; }

esp_err_t eye_motion_set_lid_coeff(float upper, float lower)
{
    if (!isfinite(upper) || !isfinite(lower)) return ESP_ERR_INVALID_ARG;
    s_coeff_upper = clampf(upper, 0.0f, 1.0f);
    s_coeff_lower = clampf(lower, 0.0f, 1.0f);
    return ESP_OK;
}

esp_err_t eye_motion_save_lid_coeff(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    float v[2] = { s_coeff_upper, s_coeff_lower };
    err = nvs_set_blob(h, NVS_KEY_COEFF, v, sizeof(v));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "lid coefficients %.2f/%.2f saved: %s",
             (double)s_coeff_upper, (double)s_coeff_lower, esp_err_to_name(err));
    return err;
}

static void load_lid_coeff(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    float v[2];
    size_t len = sizeof(v);
    if (nvs_get_blob(h, NVS_KEY_COEFF, v, &len) == ESP_OK && len == sizeof(v) &&
        isfinite(v[0]) && isfinite(v[1])) {
        s_coeff_upper = clampf(v[0], 0.0f, 1.0f);
        s_coeff_lower = clampf(v[1], 0.0f, 1.0f);
        ESP_LOGI(TAG, "lid coefficients %.2f/%.2f from NVS",
                 (double)s_coeff_upper, (double)s_coeff_lower);
    }
    nvs_close(h);
}

esp_err_t eye_motion_set_blink_hold_ms(int ms)
{
    if (ms < EYE_BLINK_HOLD_MIN_MS || ms > EYE_BLINK_HOLD_MAX_MS) return ESP_ERR_INVALID_ARG;
    s_blink_hold_ms = ms;
    return ESP_OK;
}

int eye_motion_get_blink_hold_ms(void) { return s_blink_hold_ms; }

/* Its own key, like the trim and the coefficients, and for the same reason. */
esp_err_t eye_motion_save_blink_hold(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, NVS_KEY_BLINK, (uint16_t)s_blink_hold_ms);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "blink hold %d ms saved: %s", s_blink_hold_ms, esp_err_to_name(err));
    return err;
}

static void load_blink_hold(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v;
    if (nvs_get_u16(h, NVS_KEY_BLINK, &v) == ESP_OK &&
        eye_motion_set_blink_hold_ms(v) == ESP_OK) {
        ESP_LOGI(TAG, "blink hold %u ms from NVS", (unsigned)v);
    }
    nvs_close(h);
}

esp_err_t eye_motion_save_lid_trim(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_TRIM, &s_lid_trim, sizeof(s_lid_trim));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "lid trim %.2f saved: %s", (double)s_lid_trim, esp_err_to_name(err));
    return err;
}

static void load_lid_trim(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    float v = LID_TRIM_DEFAULT;
    size_t len = sizeof(v);
    if (nvs_get_blob(h, NVS_KEY_TRIM, &v, &len) == ESP_OK &&
        len == sizeof(v) && isfinite(v) && v >= 0.0f && v <= 1.0f) {
        s_lid_trim = v;
        ESP_LOGI(TAG, "lid trim %.2f from NVS", (double)v);
    }
    nvs_close(h);
}

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
    { 0.50f, 0.50f, 1.00f, 1.00f, 400 },   /* open, centred             */
    { 0.00f, NAN,   NAN,   NAN,   700 },   /* look left                 */
    { 1.00f, NAN,   NAN,   NAN,  1100 },   /* sweep across to the right */
    { 0.50f, NAN,   NAN,   NAN,   600 },   /* back to centre            */
    { NAN,   NAN,   0.00f, 0.00f, 400 },   /* close                     */
};

/* A circle in gaze space. Lids left to the coupling throughout, so the eyes
 * hood through the bottom and widen over the top — that is most of what makes
 * it read as a roll rather than a mechanical sweep. */
static const eye_frame_t s_frames_roll[] = {
    { 0.50f, 0.50f, 1.00f, 1.00f, 500 },   /* open, centred   */
    { 0.50f, 0.92f, NAN,   NAN,   500 },   /* up              */
    { 0.80f, 0.80f, NAN,   NAN,   350 },
    { 0.92f, 0.50f, NAN,   NAN,   350 },   /* right           */
    { 0.80f, 0.20f, NAN,   NAN,   350 },
    { 0.50f, 0.08f, NAN,   NAN,   350 },   /* down            */
    { 0.20f, 0.20f, NAN,   NAN,   350 },
    { 0.08f, 0.50f, NAN,   NAN,   350 },   /* left            */
    { 0.20f, 0.80f, NAN,   NAN,   350 },
    { 0.50f, 0.92f, NAN,   NAN,   350 },   /* back to the top */
    { 0.50f, 0.50f, NAN,   NAN,   600 },   /* settle centred  */
};

/* Suspicion. The snap across is quick, the lids narrow to a slit, and then it
 * HOLDS — the hold is the whole emote. Coming back is slower than going. */
static const eye_frame_t s_frames_side_eye[] = {
    { 0.50f, 0.50f, 0.90f, 0.90f, 300 },
    { 0.12f, 0.56f, 0.45f, 0.45f, 350 },   /* dart across, lids narrow */
    { 0.12f, 0.56f, 0.45f, 0.45f,1300 },   /* hold the look            */
    { 0.50f, 0.50f, 0.90f, 0.90f, 550 },   /* unhurried return         */
};

/* Only possible because the lids are per-eye while the gaze is shared. */
static const eye_frame_t s_frames_wink[] = {
    { 0.50f, 0.50f, 0.95f, 0.95f, 300 },   /* both open        */
    { NAN,   NAN,   0.00f, 0.95f, 170 },   /* left shuts, fast */
    { NAN,   NAN,   0.00f, 0.95f, 200 },   /* held shut        */
    { NAN,   NAN,   0.95f, 0.95f, 260 },   /* and back         */
};

/* Fast attack, long hold, slow release — the shape of a startle. */
static const eye_frame_t s_frames_surprise[] = {
    { 0.50f, 0.50f, 0.55f, 0.55f, 250 },   /* half-lidded first, for contrast */
    { 0.50f, 0.70f, 1.00f, 1.00f, 110 },   /* snap wide, gaze lifts           */
    { 0.50f, 0.70f, 1.00f, 1.00f, 950 },   /* hold                            */
    { 0.50f, 0.50f, 0.85f, 0.85f, 800 },   /* settle back down                */
};

/* Everything slow. Heaviness is pace, not position. */
static const eye_frame_t s_frames_sleepy[] = {
    { 0.50f, 0.50f, 0.85f, 0.85f, 600 },
    { 0.47f, 0.35f, 0.35f, 0.35f,1500 },   /* lids droop, gaze sinks */
    { 0.46f, 0.30f, 0.00f, 0.00f, 900 },   /* slow close             */
    { 0.46f, 0.30f, 0.00f, 0.00f, 800 },   /* stays shut a beat      */
    { 0.50f, 0.44f, 0.55f, 0.55f,1200 },   /* half open, still heavy */
};

#define ANIM(id, d) { #id, d, s_frames_##id, \
                      (int)(sizeof(s_frames_##id) / sizeof(s_frames_##id[0])) }

static const eye_anim_t s_anims[] = {
    ANIM(look,      "open, look left and right, close"),
    ANIM(roll,      "roll the eyes in a full circle"),
    ANIM(side_eye,  "suspicious glance to the side, held"),
    ANIM(wink,      "left eye winks"),
    ANIM(surprise,  "snap wide and hold, then settle"),
    ANIM(sleepy,    "lids droop, gaze sinks, slow close"),
};
#define ANIM_COUNT ((int)(sizeof(s_anims) / sizeof(s_anims[0])))

/* Blinks are suppressed while an animation plays, so its timer expires during
 * playback and fires the instant the mode reverts -- the eyes blink before the
 * pose has settled. Hold them off briefly on the way out. */
static int64_t s_settle_until;

static const eye_anim_t *s_anim;
static int       s_anim_frame;
static int64_t   s_anim_started;
static float     s_from_lr, s_from_ud, s_from_ll, s_from_lr_lid;
static float     s_ll_now = NAN, s_lr_now = NAN;  /* last commanded, NAN = coupled */
static eye_mode_t s_anim_return = EYE_MODE_AUTO;
static int        s_anim_repeat = 1;   /* <0 loops until stopped */

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

static void anim_apply(float lr01, float ud01, float ll, float rl)
{
    if (!isnan(lr01)) {
        s_x_target = clamp_to_limits(EYE_LR, denorm(EYE_LR, lr01));
        eye_servo_write(EYE_LR, s_x_target);
    }
    if (!isnan(ud01)) {
        s_y_target = clamp_to_limits(EYE_UD, denorm(EYE_UD, ud01));
    }

    /* Run the coupling when either side is unspecified — it drives UD and the
     * lids together and keeps the tracked targets coherent. When both sides are
     * commanded, only UD needs writing and the explicit values follow. */
    if (isnan(ll) || isnan(rl)) {
        eye_motion_control_ud_and_lids(s_y_target);
    } else {
        eye_servo_write(EYE_UD, s_y_target);
    }

    if (!isnan(ll)) {
        eye_servo_write(EYE_TL, denorm(EYE_TL, ll));
        eye_servo_write(EYE_BL, denorm(EYE_BL, ll));
    }
    if (!isnan(rl)) {
        eye_servo_write(EYE_TR, denorm(EYE_TR, rl));
        eye_servo_write(EYE_BR, denorm(EYE_BR, rl));
    }
    s_ll_now = ll;
    s_lr_now = rl;
}

static void anim_begin_frame(int64_t t)
{
    s_anim_started = t;
    s_from_lr     = norm01(EYE_LR, s_x_target);
    s_from_ud     = norm01(EYE_UD, s_y_target);
    s_from_ll     = s_ll_now;
    s_from_lr_lid = s_lr_now;
}

esp_err_t eye_motion_play(const char *name, int repeat)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mode == EYE_MODE_STANDBY) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < ANIM_COUNT; i++) {
        if (strcmp(name, s_anims[i].name) != 0) continue;

        /* Remember where to go back to, but never stack animations. */
        if (s_mode != EYE_MODE_ANIM) s_anim_return = s_mode;
        s_anim = &s_anims[i];
        s_anim_repeat = repeat;      /* <0 loops; N plays N times */
        s_anim_frame = 0;
        s_blink_requested = false;
        s_mode = EYE_MODE_ANIM;          /* deliberately not set_mode(): that
                                          * would run neutral() and jump */
        anim_begin_frame(now_ms());
        if (repeat < 0) ESP_LOGI(TAG, "looping '%s' (%d frames) — !anim stop to end",
                                 s_anim->name, s_anim->count);
        else            ESP_LOGI(TAG, "playing '%s' (%d frames) x%d",
                                 s_anim->name, s_anim->count, repeat);
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

const char *eye_motion_anim_playing(void)
{
    const eye_anim_t *a = s_anim;   /* one read: the motion task clears it */
    return (s_mode == EYE_MODE_ANIM && a != NULL) ? a->name : NULL;
}

/* End a loop. Finishes the frame in flight rather than stopping mid-move, so
 * the mechanism settles somewhere deliberate instead of wherever it happened to
 * be when the command arrived. */
esp_err_t eye_motion_anim_stop(void)
{
    if (s_anim == NULL) return ESP_ERR_INVALID_STATE;
    s_anim_repeat = 1;
    return ESP_OK;
}

/* One tick of the player. Returns false when the sequence is done. */
static bool anim_tick(int64_t t)
{
    if (s_anim == NULL) return false;

    const eye_frame_t *f = &s_anim->frames[s_anim_frame];
    int64_t elapsed = t - s_anim_started;
    float k = (f->ms == 0) ? 1.0f : (float)elapsed / (float)f->ms;
    if (k > 1.0f) k = 1.0f;

    anim_apply(lerp01(s_from_lr,     f->lr,    k),
               lerp01(s_from_ud,     f->ud,    k),
               lerp01(s_from_ll,     f->lid_l, k),
               lerp01(s_from_lr_lid, f->lid_r, k));

    if (k < 1.0f) return true;

    if (++s_anim_frame >= s_anim->count) {
        if (s_anim_repeat > 0) s_anim_repeat--;
        if (s_anim_repeat == 0) {
            s_anim = NULL;
            s_settle_until = t + EYE_ANIM_SETTLE_MS;
            return false;
        }
        s_anim_frame = 0;            /* round again */
        anim_begin_frame(t);
        return true;
    }
    anim_begin_frame(t);
    return true;
}

/* --------------------------------------------------------- mode machine -- */

const char *eye_motion_mode_name(eye_mode_t mode)
{
    return ((int)mode < MODE_COUNT) ? s_mode_names[mode] : "?";
}

int eye_motion_mode_from_name(const char *name)
{
    if (name == NULL) return -1;
    for (int i = 0; i < MODE_COUNT; i++) {
        if (strcmp(name, s_mode_names[i]) == 0) return i;
    }
    return -1;
}

eye_mode_t eye_motion_get_mode(void) { return s_mode; }

esp_err_t eye_motion_set_mode(eye_mode_t mode)
{
    if (mode == s_mode) return ESP_OK;
    eye_mode_t from = s_mode;
    s_mode = mode;
    /* Every mode change in the original ended with neutral() and a cleared
     * blink phase. Keep that — it is what stops a half-finished blink from
     * leaving the lids shut. Calibration seeds 90° instead, once, on entry:
     * the loop must not keep rewriting it, or direct writes cannot stick. */
    if (mode == EYE_MODE_STANDBY) {
        /* Nothing. Standby holds whatever is already there and schedules
         * nothing, so entering it must not move anything either. */
    } else if (mode == EYE_MODE_CALIBRATION) {
        /* From standby the servos may be engaged but limp at unknown angles, and
         * seeding 90 would drive all six at once -- exactly what calibration is
         * one-servo-at-a-time to avoid. So only seed when coming from a mode
         * that was already driving them. */
        if (from != EYE_MODE_STANDBY) eye_motion_calibrate();
    } else {
        /* Leaving standby lands here too, at full speed: after a release there is
         * no known position to ease from. */
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
        if (s_mode != EYE_MODE_CALIBRATION && s_mode != EYE_MODE_ANIM &&
            s_mode != EYE_MODE_STANDBY &&
            t >= s_settle_until) {
            if (blink_phase == BLINK_IDLE && (s_blink_requested || t >= next_blink_at)) {
                s_blink_requested = false;
                blink_phase = BLINK_CLOSED;
                blink_until = t + s_blink_hold_ms;
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

        case EYE_MODE_STANDBY:
            /* Nothing, and no blinks: see the blink gate above. */
            break;
        }

        xTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t eye_motion_start(void)
{
    load_lid_trim();
    load_lid_coeff();
    load_blink_hold();
    if (xTaskCreate(motion_task, "eye_motion", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "motion task running at %d Hz", EYE_MOTION_TICK_HZ);
    return ESP_OK;
}
