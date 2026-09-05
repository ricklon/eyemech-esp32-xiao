#pragma once
/*
 * eye_console — line-oriented serial control over the USB cable.
 *
 * The point is that it does not need the network. eye_web is the whole control
 * surface otherwise, so a failed association, a changed WiFi password, or a
 * venue that hands out no DHCP lease leaves a mechanism that can strip its own
 * gears with no way to stop it. `!release` here works with no association, no
 * DHCP, and no browser.
 *
 * Commands are `!`-prefixed, matching the convention in the sibling projects
 * (esp/fab26-fubar-bot-demo, xiaozhi-esp32). Anything not starting with `!` is
 * ignored, so log noise pasted into the terminal cannot move a servo.
 *
 *   !help                    this list
 *   !status                  mode, release latch, vision, trim, heap, servos
 *   !release                 everything limp, LATCHED — /OE high plus all_off
 *   !engage                  clear the latch and go to neutral
 *   !mode <name>             tracking | auto | manual | calibration
 *   !blink                   queue one blink
 *   !trim <0..1>             lid openness
 *   !look <lr> <ud>          manual gaze, degrees
 *
 * Calibration (calibration mode only, one servo at a time):
 *   !servo <name> <angle>    absolute angle — seats a servo at a known place
 *   !jog <name> <±deg>       step from the last commanded angle
 *   !mark <name> min|max     record where it is now as an endpoint
 *   !limits <name> <min> <max>
 *   !cfg <name> <min_us|max_us|trim_us> <value>
 *   !save                    commit limits + cfg to NVS
 *   !defaults                back to the built-in table (does not save)
 *
 *   !reboot
 *
 * Servo names are LR UD TL BL TR BR.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t eye_console_start(void);

#ifdef __cplusplus
}
#endif
