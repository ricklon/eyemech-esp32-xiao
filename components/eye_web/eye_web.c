#include "eye_web.h"
#include "eye_mcp.h"
#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_net.h"
#include "eye_vision.h"
#include "board_pins.h"
#define EYEMECH_BOARD_NAME BOARD_NAME

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "eye_web";

/* Generated from index.html at configure time — see this component's
 * CMakeLists.txt for why it is not EMBED_TXTFILES. */
extern const unsigned char index_html_start[];
extern const unsigned int  index_html_len;

/* ---------------------------------------------------------------- helpers */

static esp_err_t read_json(httpd_req_t *req, cJSON **out)
{
    char buf[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }
    buf[received] = '\0';
    *out = cJSON_Parse(buf);
    return (*out != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "{}");
    cJSON_free(body);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* esp_http_server's error enum has no 409. Wrong mode, or a servo whose
 * position is unknown, is a conflict rather than a malformed request, so set
 * the status line directly — eyectl surfaces the code verbatim. */
static esp_err_t send_409(httpd_req_t *req, const char *why)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", why);
    httpd_resp_set_status(req, "409 Conflict");
    return send_json(req, root);
}

static float jnum(const cJSON *o, const char *key, float fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : fallback;
}

static int jservo(const cJSON *o)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "servo");
    return cJSON_IsString(v) ? eye_servo_from_name(v->valuestring) : -1;
}

/* --------------------------------------------------------------- handlers */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_len);
}

static esp_err_t state_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", eye_motion_mode_name(eye_motion_get_mode()));
    cJSON_AddStringToObject(root, "board", EYEMECH_BOARD_NAME);
    cJSON_AddBoolToObject(root, "vision", eye_vision_present());
    cJSON_AddBoolToObject(root, "released", eye_servo_is_released());
    cJSON_AddBoolToObject(root, "safe_boot", eye_servo_safe_boot());

    /* Objects rather than bare names: the page has nothing to label a button
     * with otherwise, and the descriptions already exist in the table. */
    cJSON *anims = cJSON_AddArrayToObject(root, "anims");
    for (const char *const *a = eye_motion_anim_names(); *a; a++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", *a);
        cJSON_AddStringToObject(o, "desc", eye_motion_anim_desc(*a));
        cJSON_AddItemToArray(anims, o);
    }
    /* Empty rather than null, so the page can just test truthiness. */
    const char *playing = eye_motion_anim_playing();
    cJSON_AddStringToObject(root, "anim", playing ? playing : "");

    char buf[33];
    eye_net_ssid(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "ssid", buf);
    eye_net_ip(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "ip", buf);
    cJSON_AddStringToObject(root, "link",
        eye_net_state() == EYE_NET_STA_CONNECTED ? "station" :
        eye_net_state() == EYE_NET_AP_ONLY       ? "ap" : "connecting");
    cJSON_AddNumberToObject(root, "lid_trim", eye_motion_get_lid_trim());
    cJSON_AddNumberToObject(root, "coeff_upper", eye_motion_get_coeff_upper());
    cJSON_AddNumberToObject(root, "coeff_lower", eye_motion_get_coeff_lower());
    cJSON_AddNumberToObject(root, "blink_hold_ms", eye_motion_get_blink_hold_ms());
    cJSON_AddNumberToObject(root, "lr", eye_motion_target_lr());
    cJSON_AddNumberToObject(root, "ud", eye_motion_target_ud());

    cJSON *servos = cJSON_AddArrayToObject(root, "servos");
    for (int i = 0; i < EYE_SERVO_COUNT; i++) {
        eye_limits_t l = eye_servo_limits((eye_servo_id_t)i);
        eye_servo_cfg_t c = eye_servo_cfg((eye_servo_id_t)i);
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", eye_servo_name((eye_servo_id_t)i));
        cJSON_AddNumberToObject(s, "channel", i);
        cJSON_AddNumberToObject(s, "angle", eye_servo_read((eye_servo_id_t)i));
        cJSON_AddNumberToObject(s, "min", l.min);
        cJSON_AddNumberToObject(s, "max", l.max);
        cJSON_AddNumberToObject(s, "min_us", c.min_us);
        cJSON_AddNumberToObject(s, "max_us", c.max_us);
        cJSON_AddNumberToObject(s, "trim_us", c.trim_us);
        cJSON_AddItemToArray(servos, s);
    }
    return send_json(req, root);
}

static esp_err_t mode_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "mode");
    int mode = cJSON_IsString(m) ? eye_motion_mode_from_name(m->valuestring) : -1;
    cJSON_Delete(body);
    if (mode < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown mode");
    if (eye_motion_set_mode((eye_mode_t)mode) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "follow is entered by sending a pose");
    }
    return send_ok(req);
}

static esp_err_t look_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    float lr = jnum(body, "lr", eye_motion_target_lr());
    float ud = jnum(body, "ud", eye_motion_target_ud());
    cJSON_Delete(body);

    eye_motion_set_mode(EYE_MODE_MANUAL);
    eye_motion_look(lr, ud);
    return send_ok(req);
}

static esp_err_t lid_trim_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    eye_motion_set_lid_trim(jnum(body, "value", 0.5f));
    cJSON_Delete(body);
    return send_ok(req);
}

/* How hard each lid pair tracks vertical gaze. CLAUDE.md calls these the
 * character of the face and asks for changes to be recorded in
 * docs/decisions.md — hence the note on the page rather than a bare slider.
 * Omitted fields keep their current value, so one slider can move alone. */
static esp_err_t lid_coeff_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    float upper = jnum(body, "upper", eye_motion_get_coeff_upper());
    float lower = jnum(body, "lower", eye_motion_get_coeff_lower());
    cJSON_Delete(body);
    if (eye_motion_set_lid_coeff(upper, lower) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad coefficient");
    }
    return send_ok(req);   /* not persisted — /api/save keeps it */
}

/* How long a blink stays shut. Not persisted — /api/save keeps it. */
static esp_err_t blink_hold_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, "ms");
    esp_err_t err = cJSON_IsNumber(v) ? eye_motion_set_blink_hold_ms((int)v->valuedouble)
                                      : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ms out of range");
    }
    return send_ok(req);
}

static esp_err_t anim_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(body, "name");
    int repeat = (int)jnum(body, "repeat", 1);   /* negative loops */
    esp_err_t err = cJSON_IsString(n) ? eye_motion_play(n->valuestring, repeat)
                                      : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) return send_409(req, "not while in standby or following");
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown animation");
    }
    return send_ok(req);
}

/* Ending a loop. Idempotent on purpose: a page that has just missed the end of
 * a sequence should not get an error for tidying up after it. */
static esp_err_t anim_stop_post(httpd_req_t *req)
{
    (void)eye_motion_anim_stop();
    return send_ok(req);
}

static esp_err_t blink_post(httpd_req_t *req)
{
    eye_motion_request_blink();
    return send_ok(req);
}

/* Direct angle write. Calibration mode only — this bypasses the motion layer,
 * which is exactly what you want when fitting horns and exactly what you do not
 * want while something else is driving. */
static esp_err_t servo_post(httpd_req_t *req)
{
    if (eye_motion_get_mode() != EYE_MODE_CALIBRATION) {
        return send_409(req, "not in calibration mode");
    }
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    int id = jservo(body);
    float angle = jnum(body, "angle", 90.0f);
    cJSON_Delete(body);
    if (id < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad servo");
    eye_servo_write((eye_servo_id_t)id, angle);
    return send_ok(req);
}

/* Relative steps. The safe primitive after any mechanical change: take one
 * small step and watch which way the linkage actually goes, rather than typing
 * an absolute angle at a lid that has no headroom in the closing direction. */
static esp_err_t jog_post(httpd_req_t *req)
{
    if (eye_motion_get_mode() != EYE_MODE_CALIBRATION) {
        return send_409(req, "not in calibration mode");
    }
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    int id = jservo(body);
    float delta = jnum(body, "delta", 0.0f);
    cJSON_Delete(body);
    if (id < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad servo");

    esp_err_t err = eye_servo_jog((eye_servo_id_t)id, delta);
    if (err == ESP_ERR_INVALID_STATE) {
        /* No feedback on these servos: there is nothing to step from until
         * something has been commanded. */
        return send_409(req, "position unknown — set an angle first");
    }
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "jog refused");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "angle", eye_servo_read((eye_servo_id_t)id));
    return send_json(req, root);
}

/* Command-and-confirm, never capture-and-record: the angle being marked is the
 * one the firmware commanded, and a human decides it is the endpoint. */
static esp_err_t mark_post(httpd_req_t *req)
{
    if (eye_motion_get_mode() != EYE_MODE_CALIBRATION) {
        return send_409(req, "not in calibration mode");
    }
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    int id = jservo(body);
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(body, "end");
    bool as_max = cJSON_IsString(e) && strcmp(e->valuestring, "max") == 0;
    bool as_min = cJSON_IsString(e) && strcmp(e->valuestring, "min") == 0;
    cJSON_Delete(body);   /* e is dead from here */
    if (id < 0)                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad servo");
    if (!as_max && !as_min)    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "end must be min or max");

    esp_err_t err = eye_servo_mark((eye_servo_id_t)id, as_max);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_409(req, "position unknown — set an angle first");
    }
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mark refused");

    eye_limits_t l = eye_servo_limits((eye_servo_id_t)id);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "angle", eye_servo_read((eye_servo_id_t)id));
    cJSON_AddNumberToObject(root, "min", l.min);
    cJSON_AddNumberToObject(root, "max", l.max);
    return send_json(req, root);   /* not persisted — /api/save keeps it */
}

static esp_err_t limits_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    int id = jservo(body);
    if (id < 0) { cJSON_Delete(body); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad servo"); }
    eye_limits_t l = eye_servo_limits((eye_servo_id_t)id);
    l.min = jnum(body, "min", l.min);
    l.max = jnum(body, "max", l.max);
    cJSON_Delete(body);
    eye_servo_set_limits((eye_servo_id_t)id, l);
    return send_ok(req);
}

static esp_err_t cfg_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    int id = jservo(body);
    if (id < 0) { cJSON_Delete(body); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad servo"); }
    eye_servo_cfg_t c = eye_servo_cfg((eye_servo_id_t)id);
    c.min_us  = (uint16_t)jnum(body, "min_us",  c.min_us);
    c.max_us  = (uint16_t)jnum(body, "max_us",  c.max_us);
    c.trim_us = (int16_t) jnum(body, "trim_us", c.trim_us);
    cJSON_Delete(body);
    if (eye_servo_set_cfg((eye_servo_id_t)id, c) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cfg");
    }
    return send_ok(req);
}

/* Whether the board comes up released. Persisted the moment it is set, unlike
 * everything else here — it has to survive the crash it exists to protect
 * against, so it does not wait for /api/save. */
static esp_err_t safeboot_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, "on");
    bool valid = cJSON_IsBool(v);
    bool on    = cJSON_IsTrue(v);
    cJSON_Delete(body);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "on must be true or false");
    if (eye_servo_set_safe_boot(on) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
    }
    return send_ok(req);
}

/* Throws away every measured limit and pulse range. Two things stand between a
 * stray POST and a mechanism's calibration: it only works in calibration mode,
 * and it needs the confirm token. It is also RAM-only — eye_servo_reset_defaults()
 * does not touch NVS — so a reboot undoes it and only /api/save makes it real. */
static esp_err_t defaults_post(httpd_req_t *req)
{
    if (eye_motion_get_mode() != EYE_MODE_CALIBRATION) {
        return send_409(req, "not in calibration mode");
    }
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(body, "confirm");
    bool confirmed = cJSON_IsString(c) && strcmp(c->valuestring, "defaults") == 0;
    cJSON_Delete(body);
    if (!confirmed) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "send {\"confirm\":\"defaults\"} — this discards every measured limit");
    }

    esp_err_t err = eye_servo_reset_defaults();
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset failed");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "saved", false);   /* reboot undoes it; /api/save keeps it */
    return send_json(req, root);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (eye_motion_save_lid_trim() != ESP_OK ||
        eye_motion_save_lid_coeff() != ESP_OK ||
        eye_motion_save_blink_hold() != ESP_OK ||
        eye_servo_save() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
    }
    return send_ok(req);
}

static esp_err_t release_post(httpd_req_t *req)
{
    eye_servo_release_all();
    return send_ok(req);
}

static esp_err_t engage_post(httpd_req_t *req)
{
    if (eye_motion_engage() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "engage failed");
    }
    return send_ok(req);
}

/* ------------------------------------------------------------------ poses */

/* All four fields required, each a number; eye_motion_follow() checks ranges. */
static bool pose_from_json(const cJSON *o, eye_pose_t *p)
{
    const char *keys[] = { "lr", "ud", "lid_l", "lid_r" };
    float *dst[] = { &p->lr, &p->ud, &p->lid_l, &p->lid_r };
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, keys[i]);
        if (!cJSON_IsNumber(v)) return false;
        *dst[i] = (float)v->valuedouble;
    }
    return true;
}

static const char *follow_error(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG)   return "lr, ud, lid_l and lid_r are required, each 0..1";
    if (err == ESP_ERR_INVALID_STATE) return "not accepting poses: released, or in standby, calibration or an animation";
    return "pose refused";
}

/* One pose over plain HTTP: for testing and for senders that cannot hold a
 * WebSocket. Follow times out after EYE_FOLLOW_TIMEOUT_MS unless more arrive. */
static esp_err_t pose_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    eye_pose_t p;
    esp_err_t err = pose_from_json(body, &p) ? eye_motion_follow(&p) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) return send_409(req, follow_error(err));
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, follow_error(err));
    return send_ok(req);
}

static esp_err_t follow_stop_post(httpd_req_t *req)
{
    if (eye_motion_follow_stop() != ESP_OK) return send_409(req, "not following");
    return send_ok(req);
}

/* A browser on another origin must not get a socket that drives servos: same
 * rule, and the same reason, as /mcp. Runs before the upgrade is answered. */
static esp_err_t ws_origin_check(httpd_req_t *req)
{
    size_t olen = httpd_req_get_hdr_value_len(req, "Origin");
    if (olen == 0) return ESP_OK;
    char origin[128], host[96];
    if (olen >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return ESP_FAIL;
    }
    const char *o = origin;
    if (strncmp(o, "http://", 7) == 0)       o += 7;
    else if (strncmp(o, "https://", 8) == 0) o += 8;
    else return ESP_FAIL;
    if (strcasecmp(o, host) != 0) {
        ESP_LOGW(TAG, "refused /ws/pose from origin %s", origin);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_send_error(httpd_req_t *req, const char *why)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", why);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text) };
    esp_err_t err = httpd_ws_send_frame(req, &f);
    cJSON_free(text);
    return err;
}

/* The streaming path. Each text frame is a pose object, or {"stop":true}.
 * Accepted poses get no reply, to keep a 30 Hz stream cheap; refusals get an
 * {"error":...} frame so the sender can see why nothing moves. */
#define WS_MAX_FRAME 256

static esp_err_t ws_pose(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK;   /* the upgrade itself */

    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    if (f.len == 0 || f.len > WS_MAX_FRAME) return ws_send_error(req, "frame too large");

    char buf[WS_MAX_FRAME + 1];
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, WS_MAX_FRAME);
    if (err != ESP_OK) return err;
    buf[f.len] = '\0';

    cJSON *o = cJSON_Parse(buf);
    if (!o) return ws_send_error(req, "bad json");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "stop"))) {
        cJSON_Delete(o);
        (void)eye_motion_follow_stop();   /* idempotent from the sender's side */
        return ESP_OK;
    }
    eye_pose_t p;
    err = pose_from_json(o, &p) ? eye_motion_follow(&p) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(o);
    return (err == ESP_OK) ? ESP_OK : ws_send_error(req, follow_error(err));
}

/* ------------------------------------------------------------------- /mcp */

/* Big enough for any MCP request this server accepts; tool arguments here are a
 * few numbers and a name. Held on the heap, not the httpd task's stack. */
#define MCP_MAX_BODY 4096

/* NULL when absent. Caller frees. */
static char *header_dup(httpd_req_t *req, const char *name)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0) return NULL;
    char *v = malloc(len + 1);
    if (v && httpd_req_get_hdr_value_str(req, name, v, len + 1) != ESP_OK) {
        free(v);
        v = NULL;
    }
    return v;
}

/* The protocol lives in eye_mcp.c; this only moves bytes and headers across. */
static esp_err_t mcp_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > MCP_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, NULL, 0);
    }
    char *body = malloc((size_t)total);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(body); return ESP_FAIL; }
        received += r;
    }

    eye_mcp_headers_t hdr = {
        .protocol_version = header_dup(req, "MCP-Protocol-Version"),
        .method           = header_dup(req, "Mcp-Method"),
        .name             = header_dup(req, "Mcp-Name"),
        .origin           = header_dup(req, "Origin"),
        .host             = header_dup(req, "Host"),
    };
    eye_mcp_reply_t reply;
    eye_mcp_handle(body, (size_t)total, &hdr, &reply);
    free(body);
    free((char *)hdr.protocol_version);
    free((char *)hdr.method);
    free((char *)hdr.name);
    free((char *)hdr.origin);
    free((char *)hdr.host);

    static const struct { int code; const char *line; } s_status[] = {
        { 200, "200 OK" }, { 202, "202 Accepted" }, { 400, "400 Bad Request" },
        { 403, "403 Forbidden" }, { 404, "404 Not Found" },
    };
    const char *line = "500 Internal Server Error";
    for (size_t i = 0; i < sizeof(s_status) / sizeof(s_status[0]); i++) {
        if (s_status[i].code == reply.status) line = s_status[i].line;
    }
    httpd_resp_set_status(req, line);
    esp_err_t err;
    if (reply.body) {
        httpd_resp_set_type(req, "application/json");
        err = httpd_resp_sendstr(req, reply.body);
        free(reply.body);
    } else {
        err = httpd_resp_send(req, NULL, 0);
    }
    return err;
}

/* No server-initiated stream in either protocol era this server speaks, and no
 * sessions to DELETE. */
static esp_err_t mcp_not_allowed(httpd_req_t *req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    return httpd_resp_send(req, NULL, 0);
}

static const httpd_uri_t s_routes[] = {
    { .uri = "/",             .method = HTTP_GET,  .handler = root_get },
    { .uri = "/api/state",    .method = HTTP_GET,  .handler = state_get },
    { .uri = "/api/mode",     .method = HTTP_POST, .handler = mode_post },
    { .uri = "/api/look",     .method = HTTP_POST, .handler = look_post },
    { .uri = "/api/lid_trim", .method = HTTP_POST, .handler = lid_trim_post },
    { .uri = "/api/lid_coeff",.method = HTTP_POST, .handler = lid_coeff_post },
    { .uri = "/api/blink",    .method = HTTP_POST, .handler = blink_post },
    { .uri = "/api/blink_hold",.method = HTTP_POST, .handler = blink_hold_post },
    { .uri = "/api/anim",     .method = HTTP_POST, .handler = anim_post },
    { .uri = "/api/anim/stop",.method = HTTP_POST, .handler = anim_stop_post },
    { .uri = "/api/servo",    .method = HTTP_POST, .handler = servo_post },
    { .uri = "/api/jog",      .method = HTTP_POST, .handler = jog_post },
    { .uri = "/api/mark",     .method = HTTP_POST, .handler = mark_post },
    { .uri = "/api/limits",   .method = HTTP_POST, .handler = limits_post },
    { .uri = "/api/cfg",      .method = HTTP_POST, .handler = cfg_post },
    { .uri = "/api/save",     .method = HTTP_POST, .handler = save_post },
    { .uri = "/api/safeboot", .method = HTTP_POST, .handler = safeboot_post },
    { .uri = "/api/defaults", .method = HTTP_POST, .handler = defaults_post },
    { .uri = "/api/release",  .method = HTTP_POST, .handler = release_post },
    { .uri = "/api/engage",   .method = HTTP_POST, .handler = engage_post },
    { .uri = "/api/pose",     .method = HTTP_POST, .handler = pose_post },
    { .uri = "/api/follow/stop", .method = HTTP_POST, .handler = follow_stop_post },
    { .uri = "/ws/pose",      .method = HTTP_GET,  .handler = ws_pose,
      .is_websocket = true, .ws_pre_handshake_cb = ws_origin_check },
    { .uri = "/mcp",          .method = HTTP_POST,   .handler = mcp_post },
    { .uri = "/mcp",          .method = HTTP_GET,    .handler = mcp_not_allowed },
    { .uri = "/mcp",          .method = HTTP_DELETE, .handler = mcp_not_allowed },
};

esp_err_t eye_web_start(void)
{
    /* Networking belongs to eye_net, which must already be running: it owns
     * the APSTA bring-up, so this server is reachable on the recovery AP even
     * when no station profile works. */
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(s_routes) / sizeof(s_routes[0]) + 2;
    config.lru_purge_enable = true;
    /* /mcp builds its tool list and state with cJSON; the default 4096 is tight. */
    config.stack_size = 6144;

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd");
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_routes[i]),
                            TAG, "route %s", s_routes[i].uri);
    }
    ESP_LOGI(TAG, "http server listening on :80");
    return ESP_OK;
}
