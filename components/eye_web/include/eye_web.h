#pragma once
/*
 * eye_web — WiFi station bring-up plus an HTTP control surface.
 *
 * Endpoints:
 *   GET  /              embedded control + calibration page
 *   GET  /api/state     current mode, pose, and per-axis pulse widths
 *   POST /api/mode      {"mode":"idle"|"manual"|"calibrate"}
 *   POST /api/look      {"x":-1..1,"y":-1..1,"speed":0..1}
 *   POST /api/lids      {"upper":0..1,"lower":0..1,"speed":0..1}
 *   POST /api/blink     (no body)
 *   POST /api/jog       {"axis":"pan","us":1500}     calibrate mode only
 *   POST /api/cal       {"axis":"pan","min_us":..,"center_us":..,"max_us":..,"inverted":false}
 *   POST /api/cal/save  commit all axis calibration to NVS
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connects to WiFi (credentials from secrets.h) and starts the HTTP server.
 * Returns once the server is listening; WiFi may still be reconnecting. */
esp_err_t eye_web_start(void);

#ifdef __cplusplus
}
#endif
