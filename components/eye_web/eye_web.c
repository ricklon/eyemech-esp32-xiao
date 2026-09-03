#include "eye_web.h"
#include "eye_motion.h"
#include "eye_servo.h"

#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#if __has_include("secrets.h")
#  include "secrets.h"
#else
#  warning "secrets.h missing — copy secrets.h.example and fill in your WiFi credentials"
#  define EYEMECH_WIFI_SSID     ""
#  define EYEMECH_WIFI_PASSWORD ""
#  define EYEMECH_HOSTNAME      "eyemech"
#endif

static const char *TAG = "eye_web";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

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

static float json_float(const cJSON *o, const char *key, float fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : fallback;
}

static int axis_from_name(const char *name)
{
    if (name == NULL) return -1;
    for (int i = 0; i < EYE_AXIS_COUNT; i++) {
        if (strcmp(name, eye_servo_axis_name((eye_axis_t)i)) == 0) return i;
    }
    return -1;
}

/* --------------------------------------------------------------- handlers */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t state_get(httpd_req_t *req)
{
    eye_pose_t cur = eye_motion_current_pose();
    eye_pose_t tgt = eye_motion_target_pose();

    cJSON *root = cJSON_CreateObject();
    switch (eye_motion_get_mode()) {
        case EYE_MODE_MANUAL:    cJSON_AddStringToObject(root, "mode", "manual");    break;
        case EYE_MODE_CALIBRATE: cJSON_AddStringToObject(root, "mode", "calibrate"); break;
        default:                 cJSON_AddStringToObject(root, "mode", "idle");      break;
    }

    cJSON *pose = cJSON_AddObjectToObject(root, "pose");
    cJSON_AddNumberToObject(pose, "gaze_x",    cur.gaze_x);
    cJSON_AddNumberToObject(pose, "gaze_y",    cur.gaze_y);
    cJSON_AddNumberToObject(pose, "lid_upper", cur.lid_upper);
    cJSON_AddNumberToObject(pose, "lid_lower", cur.lid_lower);

    cJSON *target = cJSON_AddObjectToObject(root, "target");
    cJSON_AddNumberToObject(target, "gaze_x", tgt.gaze_x);
    cJSON_AddNumberToObject(target, "gaze_y", tgt.gaze_y);

    cJSON *axes = cJSON_AddArrayToObject(root, "axes");
    for (int i = 0; i < EYE_AXIS_COUNT; i++) {
        eye_servo_cal_t c = eye_servo_get_cal((eye_axis_t)i);
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", eye_servo_axis_name((eye_axis_t)i));
        cJSON_AddNumberToObject(a, "value",     eye_servo_get((eye_axis_t)i));
        cJSON_AddNumberToObject(a, "pulse_us",  eye_servo_get_us((eye_axis_t)i));
        cJSON_AddNumberToObject(a, "min_us",    c.min_us);
        cJSON_AddNumberToObject(a, "center_us", c.center_us);
        cJSON_AddNumberToObject(a, "max_us",    c.max_us);
        cJSON_AddBoolToObject(a,   "inverted",  c.inverted);
        cJSON_AddItemToArray(axes, a);
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
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(m)) {
        if      (strcmp(m->valuestring, "idle") == 0)      err = eye_motion_set_mode(EYE_MODE_IDLE);
        else if (strcmp(m->valuestring, "manual") == 0)    err = eye_motion_set_mode(EYE_MODE_MANUAL);
        else if (strcmp(m->valuestring, "calibrate") == 0) err = eye_motion_set_mode(EYE_MODE_CALIBRATE);
    }
    cJSON_Delete(body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown mode");
    return send_ok(req);
}

static esp_err_t look_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    eye_motion_set_mode(EYE_MODE_MANUAL);
    eye_motion_look_at(json_float(body, "x", 0.0f),
                       json_float(body, "y", 0.0f),
                       json_float(body, "speed", 0.2f));
    cJSON_Delete(body);
    return send_ok(req);
}

static esp_err_t lids_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    eye_motion_set_mode(EYE_MODE_MANUAL);
    eye_motion_set_lids(json_float(body, "upper", 1.0f),
                        json_float(body, "lower", 1.0f),
                        json_float(body, "speed", 0.3f));
    cJSON_Delete(body);
    return send_ok(req);
}

static esp_err_t blink_post(httpd_req_t *req)
{
    eye_motion_blink();
    return send_ok(req);
}

static esp_err_t jog_post(httpd_req_t *req)
{
    if (eye_motion_get_mode() != EYE_MODE_CALIBRATE) {
        return httpd_resp_send_err(req, HTTPD_409_CONFLICT, "not in calibrate mode");
    }
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(body, "axis");
    int axis = axis_from_name(cJSON_IsString(name) ? name->valuestring : NULL);
    int us   = (int)json_float(body, "us", 0.0f);
    cJSON_Delete(body);

    if (axis < 0 || us <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad axis or us");
    eye_servo_set_us((eye_axis_t)axis, (uint16_t)us);
    return send_ok(req);
}

static esp_err_t cal_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(body, "axis");
    int axis = axis_from_name(cJSON_IsString(name) ? name->valuestring : NULL);
    if (axis < 0) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad axis");
    }
    eye_servo_cal_t c = eye_servo_get_cal((eye_axis_t)axis);
    c.min_us    = (uint16_t)json_float(body, "min_us",    c.min_us);
    c.center_us = (uint16_t)json_float(body, "center_us", c.center_us);
    c.max_us    = (uint16_t)json_float(body, "max_us",    c.max_us);
    const cJSON *inv = cJSON_GetObjectItemCaseSensitive(body, "inverted");
    if (cJSON_IsBool(inv)) c.inverted = cJSON_IsTrue(inv);
    cJSON_Delete(body);

    if (eye_servo_set_cal((eye_axis_t)axis, &c) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration");
    }
    return send_ok(req);
}

static esp_err_t cal_save_post(httpd_req_t *req)
{
    if (eye_servo_save_cal() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
    }
    return send_ok(req);
}

/* ------------------------------------------------------------------- wifi */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "control page: http://" IPSTR "/", IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, EYEMECH_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     EYEMECH_WIFI_SSID,     sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, EYEMECH_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

/* ------------------------------------------------------------------ start */

static const httpd_uri_t s_routes[] = {
    { .uri = "/",              .method = HTTP_GET,  .handler = root_get },
    { .uri = "/api/state",     .method = HTTP_GET,  .handler = state_get },
    { .uri = "/api/mode",      .method = HTTP_POST, .handler = mode_post },
    { .uri = "/api/look",      .method = HTTP_POST, .handler = look_post },
    { .uri = "/api/lids",      .method = HTTP_POST, .handler = lids_post },
    { .uri = "/api/blink",     .method = HTTP_POST, .handler = blink_post },
    { .uri = "/api/jog",       .method = HTTP_POST, .handler = jog_post },
    { .uri = "/api/cal",       .method = HTTP_POST, .handler = cal_post },
    { .uri = "/api/cal/save",  .method = HTTP_POST, .handler = cal_save_post },
};

esp_err_t eye_web_start(void)
{
    ESP_RETURN_ON_ERROR(wifi_start(), TAG, "wifi");

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
