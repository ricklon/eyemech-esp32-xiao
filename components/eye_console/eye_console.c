#include "eye_console.h"
#include "board_pins.h"
#include "eye_motion.h"
#include "eye_net.h"
#include "eye_servo.h"
#include "eye_vision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eye_console";

#define EYE_LINE_MAX 128
#define TASK_STACK  4096
#define TASK_PRIO   2

/* ------------------------------------------------------------------ helpers */

/* Every calibration verb is gated the same way /api/servo is: direct writes
 * bypass the motion layer, which is what you want while fitting horns and
 * exactly what you do not want while something else is driving. */
static bool require_calibration(void)
{
    if (eye_motion_get_mode() == EYE_MODE_CALIBRATION) return true;
    printf("not in calibration mode — try: !mode calibration\r\n");
    return false;
}

static int servo_arg(const char *name)
{
    int id = eye_servo_from_name(name ? name : "");
    if (id < 0) printf("unknown servo '%s' (LR UD TL BL TR BR)\r\n", name ? name : "");
    return id;
}

/* strtok_r over a mutable line; returns NULL when exhausted. */
static char *next_tok(char **save)
{
    return strtok_r(NULL, " \t", save);
}

static void report(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        printf("%s ok\r\n", what);
    } else if (err == ESP_ERR_INVALID_STATE) {
        printf("%s refused: position unknown — seat it first with "
               "!servo <name> <angle>\r\n", what);
    } else {
        printf("%s failed: %s\r\n", what, esp_err_to_name(err));
    }
}

/* --------------------------------------------------------------- commands */

static void cmd_help(void)
{
    printf(
    "\r\ncommands (servo names: LR UD TL BL TR BR)\r\n"
    "  !status                    mode, latch, vision, trim, heap, servos\r\n"
    "  !release                   ALL SERVOS LIMP, latched\r\n"
    "  !engage                    clear the latch, go to neutral\r\n"
    "  !mode <name>               tracking | auto | manual | calibration\r\n"
    "  !blink                     queue one blink\r\n"
    "  !trim <0..1>               lid openness\r\n"
    "  !look <lr> <ud>            manual gaze, degrees\r\n"
    "networking:\r\n"
    "  !wifi                      link, SSID, IP, access point\r\n"
    "  !wifi list                 saved profiles\r\n"
    "  !wifi scan                 nearby networks\r\n"
    "  !wifi set <n> <ssid> [pw]  save a profile (omit pw if open)\r\n"
    "  !wifi connect <n>          switch to profile n\r\n"
    "  !wifi clear <n>            forget profile n\r\n"
    "  !wifi ap                   stop roaming, stay on the access point\r\n"
    "calibration mode only:\r\n"
    "  !servo <name> <angle>      absolute angle\r\n"
    "  !jog <name> <+/-deg>       step from the last commanded angle\r\n"
    "  !mark <name> min|max       record where it is now as an endpoint\r\n"
    "  !limits <name> <min> <max>\r\n"
    "  !cfg <name> min_us|max_us|trim_us <value>\r\n"
    "  !save                      commit to NVS\r\n"
    "  !defaults                  restore the built-in table (not saved)\r\n"
    "  !safeboot [on|off]         boot released instead of into motion\r\n"
    "  !reboot\r\n\r\n");
}

static void cmd_status(void)
{
    printf("\r\n=== eyemech %s on %s ===\r\n", EYEMECH_VERSION, BOARD_NAME);
    printf("mode      : %s%s\r\n", eye_motion_mode_name(eye_motion_get_mode()),
           eye_servo_is_released() ? "   *** RELEASED ***" : "");
    printf("vision    : %s\r\n", eye_vision_present() ? "detected" : "absent");
    printf("lid trim  : %.2f\r\n", (double)eye_motion_get_lid_trim());
    printf("safe boot : %s\r\n", eye_servo_safe_boot() ? "ON (boots released)" : "OFF");
    printf("free heap : %u bytes\r\n", (unsigned)esp_get_free_heap_size());
    printf("%-5s %-3s %9s %8s %8s %8s %8s %8s\r\n",
           "name", "ch", "angle", "min", "max", "min_us", "max_us", "trim_us");
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        eye_limits_t l    = eye_servo_limits((eye_servo_id_t)i);
        eye_servo_cfg_t c = eye_servo_cfg((eye_servo_id_t)i);
        float a = eye_servo_read((eye_servo_id_t)i);
        char angle[12];
        /* "?" rather than a number: with no feedback, an unwritten or released
         * channel's position is genuinely unknown, not zero. */
        if (a != a) snprintf(angle, sizeof(angle), "%9s", "?");
        else        snprintf(angle, sizeof(angle), "%9.1f", (double)a);
        printf("%-5s %-3d %s %8.1f %8.1f %8u %8u %8d\r\n",
               eye_servo_name((eye_servo_id_t)i), i, angle,
               (double)l.min, (double)l.max,
               (unsigned)c.min_us, (unsigned)c.max_us, (int)c.trim_us);
    }
    printf("\r\n");
}

/* !wifi — the reason this console exists. Credentials go in over the cable,
 * never over the air, and the password is never echoed back. */
static void cmd_wifi(char **save)
{
    const char *sub = next_tok(save);
    char buf[33];

    if (sub == NULL || !strcmp(sub, "status")) {
        eye_net_ssid(buf, sizeof(buf));
        printf("link    : %s\r\n",
               eye_net_state() == EYE_NET_STA_CONNECTED ? "station" :
               eye_net_state() == EYE_NET_AP_ONLY       ? "access point only" : "connecting");
        printf("ssid    : %s\r\n", buf);
        eye_net_ip(buf, sizeof(buf));
        printf("ip      : %s\r\n", buf[0] ? buf : "(none)");
        printf("ap      : %s (%d client(s))\r\n", eye_net_ap_ssid(), eye_net_ap_clients());
        int act = eye_net_active_profile();
        if (act < 0) printf("active  : none yet\r\n");
        else         printf("active  : profile %d\r\n", act + 1);
        return;
    }

    if (!strcmp(sub, "list")) {
        for (int i = 0; i < EYE_NET_PROFILES; i++) {
            eye_net_profile_ssid(i, buf, sizeof(buf));
            printf("  [%d] %s%s\r\n", i + 1, buf[0] ? buf : "(empty)",
                   i == eye_net_active_profile() ? "   <- boot default" : "");
        }
        return;
    }

    if (!strcmp(sub, "scan")) {
        if (eye_net_scan_start() != ESP_OK) { printf("scan busy\r\n"); return; }
        printf("scanning...\r\n");
        fflush(stdout);
        for (int i = 0; i < 40 && eye_net_scan_busy(); i++) vTaskDelay(pdMS_TO_TICKS(250));
        static eye_net_scan_entry_t found[EYE_NET_SCAN_MAX];
        int n = eye_net_scan_results(found, EYE_NET_SCAN_MAX);
        if (n == 0) { printf("no networks found\r\n"); return; }
        for (int i = 0; i < n; i++) {
            printf("  %-32s %4d dBm  %s\r\n", found[i].ssid, found[i].rssi,
                   found[i].secure ? "secured" : "open");
        }
        return;
    }

    if (!strcmp(sub, "set")) {
        const char *slot = next_tok(save);
        const char *ssid = next_tok(save);
        const char *pass = next_tok(save);   /* absent = open network */
        if (!slot || !ssid) {
            printf("usage: !wifi set <1-%d> <ssid> [password]\r\n", EYE_NET_PROFILES);
            return;
        }
        int n = atoi(slot) - 1;
        esp_err_t err = eye_net_set_profile(n, ssid, pass ? pass : "");
        if (err == ESP_OK) printf("profile %d saved — !wifi connect %d\r\n", n + 1, n + 1);
        else               printf("could not save: %s\r\n", esp_err_to_name(err));
        return;
    }

    if (!strcmp(sub, "connect") || !strcmp(sub, "clear")) {
        const char *slot = next_tok(save);
        if (!slot) { printf("usage: !wifi %s <1-%d>\r\n", sub, EYE_NET_PROFILES); return; }
        int n = atoi(slot) - 1;
        esp_err_t err = (sub[0] == 'c' && sub[1] == 'o')
                        ? eye_net_connect_profile(n) : eye_net_clear_profile(n);
        report(sub, err);
        return;
    }

    if (!strcmp(sub, "ap")) { report("ap", eye_net_force_ap()); return; }

    printf("usage: !wifi [status|list|scan|set|connect|clear|ap]\r\n");
}

static void cmd_mode(char **save)
{
    const char *name = next_tok(save);
    int m = name ? eye_motion_mode_from_name(name) : -1;
    if (m < 0) { printf("usage: !mode tracking|auto|manual|calibration\r\n"); return; }
    eye_motion_set_mode((eye_mode_t)m);
    printf("mode -> %s\r\n", eye_motion_mode_name((eye_mode_t)m));
}

static void cmd_servo(char **save)
{
    if (!require_calibration()) return;
    const char *name = next_tok(save);
    const char *ang  = next_tok(save);
    if (!name || !ang) { printf("usage: !servo <name> <angle>\r\n"); return; }
    int id = servo_arg(name);
    if (id < 0) return;
    report("servo", eye_servo_write((eye_servo_id_t)id, strtof(ang, NULL)));
}

static void cmd_jog(char **save)
{
    if (!require_calibration()) return;
    const char *name = next_tok(save);
    const char *d    = next_tok(save);
    if (!name || !d) { printf("usage: !jog <name> <+/-deg>\r\n"); return; }
    int id = servo_arg(name);
    if (id < 0) return;
    esp_err_t err = eye_servo_jog((eye_servo_id_t)id, strtof(d, NULL));
    if (err == ESP_OK) {
        printf("%s -> %.1f\r\n", eye_servo_name((eye_servo_id_t)id),
               (double)eye_servo_read((eye_servo_id_t)id));
    } else {
        report("jog", err);
    }
}

static void cmd_mark(char **save)
{
    if (!require_calibration()) return;
    const char *name = next_tok(save);
    const char *end  = next_tok(save);
    if (!name || !end || (strcmp(end, "min") != 0 && strcmp(end, "max") != 0)) {
        printf("usage: !mark <name> min|max\r\n");
        return;
    }
    int id = servo_arg(name);
    if (id < 0) return;
    esp_err_t err = eye_servo_mark((eye_servo_id_t)id, strcmp(end, "max") == 0);
    if (err == ESP_OK) {
        eye_limits_t l = eye_servo_limits((eye_servo_id_t)id);
        printf("%s limits now min=%.1f max=%.1f (use !save to keep)\r\n",
               eye_servo_name((eye_servo_id_t)id), (double)l.min, (double)l.max);
    } else {
        report("mark", err);
    }
}

static void cmd_limits(char **save)
{
    if (!require_calibration()) return;
    const char *name = next_tok(save);
    const char *mn   = next_tok(save);
    const char *mx   = next_tok(save);
    if (!name || !mn || !mx) { printf("usage: !limits <name> <min> <max>\r\n"); return; }
    int id = servo_arg(name);
    if (id < 0) return;
    /* No min<max check: mirrored servos legitimately invert. */
    eye_limits_t l = { strtof(mn, NULL), strtof(mx, NULL) };
    report("limits", eye_servo_set_limits((eye_servo_id_t)id, l));
}

static void cmd_cfg(char **save)
{
    if (!require_calibration()) return;
    const char *name  = next_tok(save);
    const char *field = next_tok(save);
    const char *val   = next_tok(save);
    if (!name || !field || !val) {
        printf("usage: !cfg <name> min_us|max_us|trim_us <value>\r\n");
        return;
    }
    int id = servo_arg(name);
    if (id < 0) return;

    eye_servo_cfg_t c = eye_servo_cfg((eye_servo_id_t)id);
    long v = strtol(val, NULL, 10);
    if      (strcmp(field, "min_us")  == 0) c.min_us  = (uint16_t)v;
    else if (strcmp(field, "max_us")  == 0) c.max_us  = (uint16_t)v;
    else if (strcmp(field, "trim_us") == 0) c.trim_us = (int16_t)v;
    else { printf("unknown field '%s'\r\n", field); return; }

    report("cfg", eye_servo_set_cfg((eye_servo_id_t)id, c));
}

static void handle(char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (cmd == NULL) return;
    cmd++;   /* skip the '!' */

    if      (!strcmp(cmd, "help"))     cmd_help();
    else if (!strcmp(cmd, "status"))   cmd_status();
    else if (!strcmp(cmd, "release"))  report("release", eye_servo_release_all());
    else if (!strcmp(cmd, "engage"))   report("engage",  eye_motion_engage());
    else if (!strcmp(cmd, "blink"))    report("blink",   eye_motion_request_blink());
    else if (!strcmp(cmd, "mode"))     cmd_mode(&save);
    else if (!strcmp(cmd, "wifi"))     cmd_wifi(&save);
    else if (!strcmp(cmd, "servo"))    cmd_servo(&save);
    else if (!strcmp(cmd, "jog"))      cmd_jog(&save);
    else if (!strcmp(cmd, "mark"))     cmd_mark(&save);
    else if (!strcmp(cmd, "limits"))   cmd_limits(&save);
    else if (!strcmp(cmd, "cfg"))      cmd_cfg(&save);
    else if (!strcmp(cmd, "save"))     report("save",     eye_servo_save());
    else if (!strcmp(cmd, "safeboot")) {
        const char *v = next_tok(&save);
        if (v && (!strcmp(v, "on") || !strcmp(v, "off"))) {
            report("safeboot", eye_servo_set_safe_boot(!strcmp(v, "on")));
        } else if (v) {
            printf("usage: !safeboot [on|off]\r\n");
        }
        printf("safe boot is %s\r\n", eye_servo_safe_boot() ? "ON" : "OFF");
    }
    else if (!strcmp(cmd, "defaults")) report("defaults", eye_servo_reset_defaults());
    else if (!strcmp(cmd, "trim")) {
        const char *v = next_tok(&save);
        if (!v) { printf("usage: !trim <0..1>\r\n"); return; }
        report("trim", eye_motion_set_lid_trim(strtof(v, NULL)));
    } else if (!strcmp(cmd, "look")) {
        const char *a = next_tok(&save), *b = next_tok(&save);
        if (!a || !b) { printf("usage: !look <lr> <ud>\r\n"); return; }
        eye_motion_set_mode(EYE_MODE_MANUAL);
        report("look", eye_motion_look(strtof(a, NULL), strtof(b, NULL)));
    } else if (!strcmp(cmd, "reboot")) {
        printf("rebooting...\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        printf("unknown command '%s' — try !help\r\n", cmd);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------- task */

static void console_task(void *arg)
{
    (void)arg;
    char buf[EYE_LINE_MAX];
    size_t len = 0;

    vTaskDelay(pdMS_TO_TICKS(500));   /* let the monitor attach */
    printf("\r\n[eye_console] ready — type !help\r\n");
    fflush(stdout);

    for (;;) {
        int c = getchar();
        if (c == EOF) {
            /* No VFS driver is installed on purpose (see sdkconfig.defaults),
             * so stdin is non-blocking and idles here rather than parking on a
             * read. 10 ms is far below human typing rates. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') {
            fputs("\r\n", stdout);
            if (len > 0) {
                buf[len] = '\0';
                /* Only act on '!' lines. Pasted log output cannot move a servo. */
                if (buf[0] == '!') handle(buf);
                len = 0;
            }
            fflush(stdout);
        } else if (c == '\b' || c == 0x7f) {
            if (len > 0) { len--; fputs("\b \b", stdout); fflush(stdout); }
        } else if (c >= 0x20 && c < 0x7f && len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
            fputc(c, stdout);
            fflush(stdout);
        }
    }
}

esp_err_t eye_console_start(void)
{
    if (xTaskCreate(console_task, "eye_console", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial console ready — type !help");
    return ESP_OK;
}
