#include "eye_web.h"
#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_net.h"
#include "eye_vision.h"
#include "board_pins.h"
#define EYEMECH_BOARD_NAME BOARD_NAME

#include <string.h>
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

    cJSON *anims = cJSON_AddArrayToObject(root, "anims");
    for (const char *const *a = eye_motion_anim_names(); *a; a++) {
        cJSON_AddItemToArray(anims, cJSON_CreateString(*a));
    }

    char buf[33];
    eye_net_ssid(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "ssid", buf);
    eye_net_ip(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "ip", buf);
    cJSON_AddStringToObject(root, "link",
        eye_net_state() == EYE_NET_STA_CONNECTED ? "station" :
        eye_net_state() == EYE_NET_AP_ONLY       ? "ap" : "connecting");
    cJSON_AddNumberToObject(root, "lid_trim", eye_motion_get_lid_trim());
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
    eye_motion_set_mode((eye_mode_t)mode);
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
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown animation");
    }
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
        /* esp_http_server's error enum has no 409, so set the status line
         * directly. "Wrong mode" is a conflict, not a malformed request, and
         * eyectl surfaces the code verbatim. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"not in calibration mode\"}");
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

static esp_err_t save_post(httpd_req_t *req)
{
    if (eye_motion_save_lid_trim() != ESP_OK ||
        eye_motion_save_lid_coeff() != ESP_OK ||
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

static const httpd_uri_t s_routes[] = {
    { .uri = "/",             .method = HTTP_GET,  .handler = root_get },
    { .uri = "/api/state",    .method = HTTP_GET,  .handler = state_get },
    { .uri = "/api/mode",     .method = HTTP_POST, .handler = mode_post },
    { .uri = "/api/look",     .method = HTTP_POST, .handler = look_post },
    { .uri = "/api/lid_trim", .method = HTTP_POST, .handler = lid_trim_post },
    { .uri = "/api/blink",    .method = HTTP_POST, .handler = blink_post },
    { .uri = "/api/anim",     .method = HTTP_POST, .handler = anim_post },
    { .uri = "/api/servo",    .method = HTTP_POST, .handler = servo_post },
    { .uri = "/api/limits",   .method = HTTP_POST, .handler = limits_post },
    { .uri = "/api/cfg",      .method = HTTP_POST, .handler = cfg_post },
    { .uri = "/api/save",     .method = HTTP_POST, .handler = save_post },
    { .uri = "/api/release",  .method = HTTP_POST, .handler = release_post },
    { .uri = "/api/engage",   .method = HTTP_POST, .handler = engage_post },
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

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd");
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_routes[i]),
                            TAG, "route %s", s_routes[i].uri);
    }
    ESP_LOGI(TAG, "http server listening on :80");
    return ESP_OK;
}
