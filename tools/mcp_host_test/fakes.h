#pragma once
#include <stdbool.h>
#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_vision.h"

typedef struct {
    eye_mode_t   mode;
    bool         released, safe_boot, vision;
    eye_limits_t limits[EYE_SERVO_COUNT];
    float        angle[EYE_SERVO_COUNT];
    float        lr, ud;
    const char  *playing;
    int          last_repeat, looks, blinks, anim_stops, release_calls, set_mode_calls;
} fake_t;

extern fake_t fake;
void fake_reset(void);
