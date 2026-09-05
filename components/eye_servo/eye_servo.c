#include "eye_servo.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "eye_servo";
#define NVS_NAMESPACE "eyemech"
#define NVS_KEY_CAL   "servo_cal_v1"

static const char *s_names[EYE_SERVO_COUNT] = { "LR", "UD", "TL", "BL", "TR", "BR" };

/* Unchanged from the MicroPython original. BL and TR run backwards because
 * those two lid servos are mounted mirrored relative to their partners. */
static const eye_limits_t s_default_limits[EYE_SERVO_COUNT] = {
    [EYE_LR] = { 40.0f, 140.0f },
    [EYE_UD] = { 40.0f, 140.0f },
    [EYE_TL] = { 90.0f, 170.0f },
    [EYE_BL] = { 90.0f,  10.0f },
    [EYE_TR] = { 90.0f,  10.0f },
    [EYE_BR] = { 90.0f, 160.0f },
};

/* Persisted as one blob so a partial write can't leave axes inconsistent. */
typedef struct {
    eye_limits_t    limits[EYE_SERVO_COUNT];
    eye_servo_cfg_t cfg[EYE_SERVO_COUNT];
} eye_servo_store_t;

static eye_servo_store_t s_store;
static float             s_last[EYE_SERVO_COUNT];
static pca9685_t        *s_pca;
static bool              s_released;

static eye_servo_cfg_t default_cfg(void)
{
    return (eye_servo_cfg_t){
        .min_us = 500, .max_us = 2500,
        .min_angle = 0.0f, .max_angle = 180.0f,
        .trim_us = 0,
    };
}

/* A blob written by an older build, or a partly-erased one, can carry spans of
 * zero. Those divide by zero in eye_servo_write() and produce exactly the
 * non-finite angles guarded against there, so reject the blob instead of
 * trusting it. Deliberately does NOT check min < max on the limits: mirrored
 * servos legitimately invert (see the header). */
static bool store_is_sane(void)
{
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        const eye_servo_cfg_t *c = &s_store.cfg[i];
        if (c->min_us < 300 || c->max_us > 3000 || c->min_us >= c->max_us) return false;
        if (!(c->max_angle > c->min_angle)) return false;
        if (!isfinite(s_store.limits[i].min) || !isfinite(s_store.limits[i].max)) return false;
    }
    return true;
}

esp_err_t eye_servo_reset_defaults(void)
{
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        s_store.limits[i] = s_default_limits[i];
        s_store.cfg[i]    = default_cfg();
        s_last[i]         = NAN;
    }
    return ESP_OK;
}

esp_err_t eye_servo_init(pca9685_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    s_pca = dev;
    eye_servo_reset_defaults();

    esp_err_t err = eye_servo_load();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no stored calibration (%s) — using defaults from the "
                      "MicroPython build", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t eye_servo_write(eye_servo_id_t id, float angle)
{
    if (id >= EYE_SERVO_COUNT || s_pca == NULL) return ESP_ERR_INVALID_ARG;

    /* NaN and inf must be rejected before the clamp, not after: every
     * comparison against NaN is false, so the clamp below passes it straight
     * through, the isnan() skip below lets it past, and (int)(NaN + 0.5f) is
     * undefined -- on RISC-V it yields INT_MAX, which pca9685_set_us() clamps
     * to 4095 ticks, i.e. a ~20 ms pulse in a 20 ms period.
     *
     * This is not a theoretical input. NAN is how this module encodes "position
     * unknown", which is a normal state on feedback-free servos: before the
     * first write, after a release, and for any channel that
     * eye_servo_resume_from_hardware() found idle. Anything computing a delta
     * from eye_servo_read() will produce NaN in those cases. */
    if (!isfinite(angle)) return ESP_ERR_INVALID_ARG;

    /* Refuse everything while released. The latch lives here, at the only
     * layer that touches the chip, so every caller above is covered without
     * having to know about it -- including the blink state machine, which is
     * what silently re-energised the lids a few seconds after a release. */
    if (s_released) return ESP_ERR_INVALID_STATE;

    const eye_servo_cfg_t *c = &s_store.cfg[id];

    if (angle < c->min_angle) angle = c->min_angle;
    else if (angle > c->max_angle) angle = c->max_angle;

    /* Skip no-op writes: the motion loop rewrites identical lid targets
     * thousands of times a second and I²C is now the bottleneck. Worth roughly
     * an order of magnitude in loop rate. */
    if (!isnan(s_last[id]) && angle == s_last[id]) return ESP_OK;
    s_last[id] = angle;

    float span_us    = (float)(c->max_us - c->min_us);
    float span_angle = c->max_angle - c->min_angle;
    float us = (float)c->min_us + span_us * ((angle - c->min_angle) / span_angle)
               + (float)c->trim_us;

    return pca9685_set_us(s_pca, (uint8_t)id, (int)(us + 0.5f));
}

esp_err_t eye_servo_resume_from_hardware(void)
{
    if (s_pca == NULL) return ESP_ERR_INVALID_STATE;

    int recovered = 0;
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        int us = pca9685_get_us(s_pca, (uint8_t)i);
        if (us <= 0) { s_last[i] = NAN; continue; }   /* limp or unreadable */

        /* Inverse of the mapping in eye_servo_write(). */
        const eye_servo_cfg_t *c = &s_store.cfg[i];
        float span_us    = (float)(c->max_us - c->min_us);
        float span_angle = c->max_angle - c->min_angle;
        float angle = c->min_angle +
                      ((float)us - (float)c->trim_us - (float)c->min_us) *
                      span_angle / span_us;

        if (angle < c->min_angle) angle = c->min_angle;
        else if (angle > c->max_angle) angle = c->max_angle;

        s_last[i] = angle;
        recovered++;
    }

    ESP_LOGI(TAG, "resumed %d/%d channels from PCA9685 registers",
             recovered, EYE_SERVO_COUNT);
    return (recovered > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

float eye_servo_read(eye_servo_id_t id)
{
    return (id < EYE_SERVO_COUNT) ? s_last[id] : NAN;
}

esp_err_t eye_servo_release(eye_servo_id_t id)
{
    if (id >= EYE_SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_released) return ESP_OK;
    s_last[id] = NAN;
    return pca9685_set_us(s_pca, (uint8_t)id, 0);
}

esp_err_t eye_servo_release_all(void)
{
    s_released = true;

    /* /OE first: it is a wire, so it acts immediately and works even if the
     * bus below is wedged. all_off() is then belt and braces. */
    esp_err_t oe  = pca9685_oe_set(false);
    esp_err_t off = pca9685_all_off(s_pca);

    for (int i = 0; i < EYE_SERVO_COUNT; i++) s_last[i] = NAN;

    ESP_LOGW(TAG, "RELEASED — servos limp and latched (oe %s, all_off %s)",
             esp_err_to_name(oe), esp_err_to_name(off));
    return (oe == ESP_OK) ? off : oe;
}

esp_err_t eye_servo_engage(void)
{
    if (!s_released) return ESP_OK;
    s_released = false;
    esp_err_t err = pca9685_oe_set(true);
    ESP_LOGI(TAG, "engaged — outputs live, position unknown until commanded");
    return err;
}

bool eye_servo_is_released(void) { return s_released; }

eye_limits_t eye_servo_limits(eye_servo_id_t id)
{
    return (id < EYE_SERVO_COUNT) ? s_store.limits[id] : (eye_limits_t){ 90.0f, 90.0f };
}

esp_err_t eye_servo_set_limits(eye_servo_id_t id, eye_limits_t limits)
{
    if (id >= EYE_SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    /* No min<max check on purpose — mirrored servos legitimately invert. */
    s_store.limits[id] = limits;
    return ESP_OK;
}

eye_servo_cfg_t eye_servo_cfg(eye_servo_id_t id)
{
    return (id < EYE_SERVO_COUNT) ? s_store.cfg[id] : default_cfg();
}

esp_err_t eye_servo_set_cfg(eye_servo_id_t id, eye_servo_cfg_t cfg)
{
    if (id >= EYE_SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    if (cfg.min_us < 300 || cfg.max_us > 3000 || cfg.min_us >= cfg.max_us) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.max_angle <= cfg.min_angle) return ESP_ERR_INVALID_ARG;
    s_store.cfg[id] = cfg;
    s_last[id] = NAN;   /* force the next write through */
    return ESP_OK;
}

esp_err_t eye_servo_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_CAL, &s_store, sizeof(s_store));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "calibration save: %s", esp_err_to_name(err));
    return err;
}

esp_err_t eye_servo_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(s_store);
    err = nvs_get_blob(h, NVS_KEY_CAL, &s_store, &len);
    nvs_close(h);
    if (err == ESP_OK && len != sizeof(s_store)) {
        eye_servo_reset_defaults();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (err == ESP_OK && !store_is_sane()) {
        ESP_LOGW(TAG, "stored calibration failed validation — using defaults");
        eye_servo_reset_defaults();
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

const char *eye_servo_name(eye_servo_id_t id)
{
    return (id < EYE_SERVO_COUNT) ? s_names[id] : "??";
}

int eye_servo_from_name(const char *name)
{
    if (name == NULL) return -1;
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        if (strcmp(name, s_names[i]) == 0) return i;
    }
    return -1;
}
