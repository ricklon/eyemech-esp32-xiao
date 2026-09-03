#include "eye_servo.h"

#include <string.h>
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "eye_servo";

#define LEDC_MODE       LEDC_LOW_SPEED_MODE   /* ESP32-S3 has low-speed only */
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_RES        LEDC_TIMER_14_BIT
#define LEDC_MAX_DUTY   ((1 << 14) - 1)
#define PERIOD_US       (1000000 / EYE_SERVO_FREQ_HZ)

#define NVS_NAMESPACE   "eyemech"
#define NVS_CAL_KEY     "servo_cal"

/*
 * TODO(hardware): confirm GPIO assignment against the wiring harness.
 * XIAO ESP32-S3 silk -> GPIO:  D0=1 D1=2 D2=3 D3=4 D4=5 D5=6
 *                              D6=43(TX) D7=44(RX) D8=7 D9=8 D10=9
 * D6/D7 are left free for the USB-serial console; D8..D10 for SPI.
 */
static const int s_gpio[EYE_AXIS_COUNT] = {
    [EYE_AXIS_PAN]         = 1,   /* D0 */
    [EYE_AXIS_TILT]        = 2,   /* D1 */
    [EYE_AXIS_LID_UPPER_L] = 3,   /* D2 */
    [EYE_AXIS_LID_UPPER_R] = 4,   /* D3 */
    [EYE_AXIS_LID_LOWER_L] = 5,   /* D4 */
    [EYE_AXIS_LID_LOWER_R] = 6,   /* D5 */
};

static const char *s_names[EYE_AXIS_COUNT] = {
    "pan", "tilt", "lid_upper_l", "lid_upper_r", "lid_lower_l", "lid_lower_r",
};

static eye_servo_cal_t s_cal[EYE_AXIS_COUNT];
static float           s_value[EYE_AXIS_COUNT];
static uint16_t        s_pulse[EYE_AXIS_COUNT];
static bool            s_ready;

static inline uint16_t clamp_us(int us)
{
    if (us < EYE_SERVO_PULSE_MIN_US) return EYE_SERVO_PULSE_MIN_US;
    if (us > EYE_SERVO_PULSE_MAX_US) return EYE_SERVO_PULSE_MAX_US;
    return (uint16_t)us;
}

static esp_err_t write_pulse(eye_axis_t axis, uint16_t pulse_us)
{
    uint32_t duty = ((uint32_t)pulse_us * (LEDC_MAX_DUTY + 1)) / PERIOD_US;
    esp_err_t err = ledc_set_duty(LEDC_MODE, (ledc_channel_t)axis, duty);
    if (err != ESP_OK) return err;
    err = ledc_update_duty(LEDC_MODE, (ledc_channel_t)axis);
    if (err == ESP_OK) s_pulse[axis] = pulse_us;
    return err;
}

eye_servo_cal_t eye_servo_default_cal(eye_axis_t axis)
{
    /* TODO(hardware): these are placeholder endpoints. Jog each axis with the
     * web calibration page, find the mechanical limits, and save. */
    (void)axis;
    return (eye_servo_cal_t){
        .min_us    = 1000,
        .center_us = 1500,
        .max_us    = 2000,
        .inverted  = false,
    };
}

const char *eye_servo_axis_name(eye_axis_t axis)
{
    return (axis < EYE_AXIS_COUNT) ? s_names[axis] : "invalid";
}

esp_err_t eye_servo_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RES,
        .freq_hz         = EYE_SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    for (int i = 0; i < EYE_AXIS_COUNT; i++) {
        s_cal[i] = eye_servo_default_cal((eye_axis_t)i);
        ledc_channel_config_t ch = {
            .speed_mode = LEDC_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = LEDC_TIMER,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = s_gpio[i],
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc ch %d", i);
    }

    s_ready = true;
    esp_err_t loaded = eye_servo_load_cal();
    if (loaded != ESP_OK) {
        ESP_LOGW(TAG, "no stored calibration (%s) — using defaults",
                 esp_err_to_name(loaded));
    }

    for (int i = 0; i < EYE_AXIS_COUNT; i++) {
        eye_servo_set((eye_axis_t)i, 0.0f);
    }
    ESP_LOGI(TAG, "initialized %d axes at %d Hz", EYE_AXIS_COUNT, EYE_SERVO_FREQ_HZ);
    return ESP_OK;
}

esp_err_t eye_servo_set(eye_axis_t axis, float normalized)
{
    if (!s_ready || axis >= EYE_AXIS_COUNT) return ESP_ERR_INVALID_ARG;

    if (normalized < -1.0f) normalized = -1.0f;
    if (normalized >  1.0f) normalized =  1.0f;
    s_value[axis] = normalized;

    const eye_servo_cal_t *c = &s_cal[axis];
    float v = c->inverted ? -normalized : normalized;
    float us = (v >= 0.0f)
        ? c->center_us + v * (float)(c->max_us - c->center_us)
        : c->center_us + v * (float)(c->center_us - c->min_us);

    return write_pulse(axis, clamp_us((int)(us + 0.5f)));
}

esp_err_t eye_servo_set_us(eye_axis_t axis, uint16_t pulse_us)
{
    if (!s_ready || axis >= EYE_AXIS_COUNT) return ESP_ERR_INVALID_ARG;
    return write_pulse(axis, clamp_us(pulse_us));
}

float    eye_servo_get(eye_axis_t axis)    { return axis < EYE_AXIS_COUNT ? s_value[axis] : 0.0f; }
uint16_t eye_servo_get_us(eye_axis_t axis) { return axis < EYE_AXIS_COUNT ? s_pulse[axis] : 0; }

esp_err_t eye_servo_release(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < EYE_AXIS_COUNT; i++) {
        ledc_set_duty(LEDC_MODE, (ledc_channel_t)i, 0);
        ledc_update_duty(LEDC_MODE, (ledc_channel_t)i);
    }
    return ESP_OK;
}

esp_err_t eye_servo_set_cal(eye_axis_t axis, const eye_servo_cal_t *cal)
{
    if (axis >= EYE_AXIS_COUNT || cal == NULL) return ESP_ERR_INVALID_ARG;
    eye_servo_cal_t c = *cal;
    c.min_us    = clamp_us(c.min_us);
    c.center_us = clamp_us(c.center_us);
    c.max_us    = clamp_us(c.max_us);
    if (c.min_us >= c.max_us) return ESP_ERR_INVALID_ARG;
    if (c.center_us <= c.min_us || c.center_us >= c.max_us) return ESP_ERR_INVALID_ARG;
    s_cal[axis] = c;
    return eye_servo_set(axis, s_value[axis]);
}

eye_servo_cal_t eye_servo_get_cal(eye_axis_t axis)
{
    return (axis < EYE_AXIS_COUNT) ? s_cal[axis] : eye_servo_default_cal(0);
}

esp_err_t eye_servo_save_cal(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_CAL_KEY, s_cal, sizeof(s_cal));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "calibration save: %s", esp_err_to_name(err));
    return err;
}

esp_err_t eye_servo_load_cal(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(s_cal);
    err = nvs_get_blob(h, NVS_CAL_KEY, s_cal, &len);
    nvs_close(h);
    if (err == ESP_OK && len != sizeof(s_cal)) return ESP_ERR_NVS_INVALID_LENGTH;
    return err;
}
