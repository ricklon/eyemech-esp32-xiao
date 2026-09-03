#pragma once
/*
 * eye_vision — port of the Comms class from micropython/main.py.
 *
 * Talks to a Grove Vision AI module over UART1 at 921600 baud, asks it to run
 * one inference at a time (AT+INVOKE=1,0,1), and pulls the first bounding box
 * out of the JSON it streams back.
 *
 * The parsing here is deliberately the same string-scraping the original did —
 * find "resolution", then "boxes":, then the first two integers. A real JSON
 * parser is a fine improvement, but it is a behavior change, so make it
 * deliberately and note it in docs/decisions.md.
 */
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EYE_VISION_PIXEL_CENTRE 112
#define EYE_VISION_DEADZONE     20.0f
#define EYE_VISION_ADJ_FACTOR   10.0f
#define EYE_VISION_BUF_MAX      4096   /* runaway guard — see eye_vision.c */

typedef struct {
    float x;          /* pixels from centre, positive = right in camera frame */
    float y;
    bool  is_static;  /* boxes identical to last frame: subject hasn't moved   */
} eye_vision_offset_t;

/* Installs the UART and probes for the module. Returns ESP_OK if a module
 * answered — the caller uses that to pick tracking vs auto mode at boot. */
esp_err_t eye_vision_init(void);

/* True if a module answered at init. */
bool eye_vision_present(void);

/* Non-blocking. ESP_OK with a filled offset when a new box arrived,
 * ESP_ERR_NOT_FOUND when there is nothing new this tick. */
esp_err_t eye_vision_poll(eye_vision_offset_t *out);

#ifdef __cplusplus
}
#endif
