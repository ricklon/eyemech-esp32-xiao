#pragma once
/*
 * eye_web — the HTTP control surface that replaced the three ADC pots, the
 * enable switch, the mode switch and the blink button.
 *
 * Networking is eye_net's job, not this component's; call eye_net_start()
 * first. That split is what lets the control page stay reachable on the
 * recovery access point when no station profile works.
 *
 *   GET  /                control + calibration page (embedded, no CDN)
 *   GET  /api/state       mode, per-servo angle/limits/cfg, lid trim, vision
 *   POST /api/mode        {"mode":"tracking"|"auto"|"manual"|"calibration"|"standby"}
 *   POST /api/look        {"lr":deg,"ud":deg}          switches to manual
 *   POST /api/lid_trim    {"value":0..1}               was the trim pot
 *   POST /api/blink       —                            was the blink button
 *   POST /api/blink_hold  {"ms":30..400}               how long a blink stays shut
 *   POST /api/anim        {"name":"look"|"roll","repeat":1}  -1 loops
 *   POST /api/servo       {"servo":"TL","angle":120}   calibration mode only
 *   POST /api/limits      {"servo":"TL","min":90,"max":170}
 *   POST /api/cfg         {"servo":"TL","min_us":500,"max_us":2500,"trim_us":0}
 *   POST /api/save        commit limits + cfg to NVS
 *   POST /api/release     all servos limp — LATCHES, nothing moves until engage
 *   POST /api/engage      clear the release latch and go to neutral
 *   POST /api/pose        {"lr","ud"} + {"lid_l","lid_r"} or {"lid_tl","lid_bl","lid_tr","lid_br"},
 *                         each 0..1 — enters follow
 *   POST /api/follow/stop ease to neutral and return to the previous mode
 *   GET  /ws/pose         WebSocket: a pose per text frame, or {"stop":true}
 *   POST /mcp             MCP server (Streamable HTTP, 2026-07-28 and 2025-03-26..
 *                         2025-11-25). Gaze, blink, animations, mode and release;
 *                         never engage or calibration. See eye_mcp.c.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t eye_web_start(void);

#ifdef __cplusplus
}
#endif
