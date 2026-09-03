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

static eye_servo_cfg_t default_cfg(void)
{
    return (eye_servo_cfg_t){
        .min_us = 500, .max_us = 2500,
        .min_angle = 0.0f, .max_angle = 180.0f,
        .trim_us = 0,
    };
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

float eye_servo_read(eye_servo_id_t id)
{
    return (id < EYE_SERVO_COUNT) ? s_last[id] : NAN;
}

esp_err_t eye_servo_release(eye_servo_id_t id)
{
    if (id >= EYE_SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    s_last[id] = NAN;
    return pca9685_set_us(s_pca, (uint8_t)id, 0);
}

esp_err_t eye_servo_release_all(void)
{
    for (int i = 0; i < EYE_SERVO_COUNT; i++) s_last[i] = NAN;
    return pca9685_all_off(s_pca);
}

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
