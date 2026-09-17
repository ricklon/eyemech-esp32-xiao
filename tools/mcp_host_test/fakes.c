/* Fake eye_motion / eye_servo / eye_vision: state the tests can set and read. */
#include "fakes.h"
#include <math.h>
#include <string.h>

fake_t fake;

static const char *const s_anims[] = { "look", "roll", NULL };
static const char *const s_modes[] = { "tracking", "auto", "manual", "calibration", "anim", "standby" };
static const char *const s_servos[] = { "LR", "UD", "TL", "BL", "TR", "BR" };

void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.mode = EYE_MODE_AUTO;
    /* The rebuilt mechanism's measured limits, TL and BR running backwards. */
    const eye_limits_t l[EYE_SERVO_COUNT] = { {42, 138}, {40, 140}, {90, 13}, {93, 172}, {90, 172}, {90, 15} };
    memcpy(fake.limits, l, sizeof(l));
    fake.lr = 90;
    fake.ud = 90;
    for (int i = 0; i < EYE_SERVO_COUNT; i++) fake.angle[i] = NAN;
}

bool        eye_servo_is_released(void)            { return fake.released; }
bool        eye_servo_safe_boot(void)              { return fake.safe_boot; }
esp_err_t   eye_servo_release_all(void)            { fake.released = true; fake.release_calls++; return ESP_OK; }
eye_limits_t eye_servo_limits(eye_servo_id_t id)   { return fake.limits[id]; }
float       eye_servo_read(eye_servo_id_t id)      { return fake.angle[id]; }
const char *eye_servo_name(eye_servo_id_t id)      { return s_servos[id]; }

eye_mode_t  eye_motion_get_mode(void)              { return fake.mode; }
esp_err_t   eye_motion_set_mode(eye_mode_t m)      { fake.mode = m; fake.set_mode_calls++; return ESP_OK; }
const char *eye_motion_mode_name(eye_mode_t m)     { return s_modes[m]; }
float       eye_motion_target_lr(void)             { return fake.lr; }
float       eye_motion_target_ud(void)             { return fake.ud; }
float       eye_motion_get_lid_trim(void)          { return 0.85f; }
const char *eye_motion_anim_playing(void)          { return fake.playing; }
const char *const *eye_motion_anim_names(void)     { return s_anims; }
esp_err_t   eye_motion_request_blink(void)         { fake.blinks++; return ESP_OK; }
esp_err_t   eye_motion_anim_stop(void)             { fake.anim_stops++; return ESP_OK; }

esp_err_t eye_motion_look(float lr, float ud)
{
    fake.lr = lr;
    fake.ud = ud;
    fake.looks++;
    return ESP_OK;
}

esp_err_t eye_motion_play(const char *name, int repeat)
{
    for (const char *const *a = s_anims; *a; a++) {
        if (strcmp(*a, name) == 0) {
            fake.playing = *a;
            fake.last_repeat = repeat;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool eye_vision_present(void) { return fake.vision; }
