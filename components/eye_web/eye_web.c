#include "eye_web.h"
#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_vision.h"
#include "board_pins.h"
#define EYEMECH_BOARD_NAME BOARD_NAME

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#if __has_include("secrets.h")
#  include "secrets.h"
#else
#  warning "secrets.h missing — copy secrets.h.example and fill in WiFi credentials"
#  define EYEMECH_WIFI_SSID     ""
#  define EYEMECH_WIFI_PASSWORD ""
#  define EYEMECH_HOSTNAME      "eyemech"
#endif

#ifndef EYEMECH_SETUP_AP_SSID
#  define EYEMECH_SETUP_AP_SSID "eyemech-setup"
#endif
#ifndef EYEMECH_SETUP_AP_PASSWORD
#  define EYEMECH_SETUP_AP_PASSWORD ""
#endif

static const char *TAG = "eye_web";
#define NVS_NAMESPACE "eyemech"
#define NVS_WIFI_SSID "wifi_ssid"
#define NVS_WIFI_PASS "wifi_pass"
#define STA_CONNECTED_BIT BIT0
#define STA_FAILED_BIT    BIT1
#define WIFI_MAX_RETRIES  8
#define WIFI_WAIT_MS      15000

/* Generated from index.html at configure time — see this component's
 * CMakeLists.txt for why it is not EMBED_TXTFILES. */
extern const unsigned char index_html_start[];
extern const unsigned int  index_html_len;

static EventGroupHandle_t s_wifi_events;
static int                s_wifi_retry;
static bool               s_setup_ap;
static bool               s_wifi_started;
static char               s_wifi_ssid[33];
static char               s_wifi_pass[65];
static uint8_t            s_last_wifi_reason;
static esp_ip4_addr_t     s_sta_ip;

static const char *wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "no_ap_found";
    case WIFI_REASON_AUTH_FAIL: return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL: return "assoc_fail";
    case WIFI_REASON_CONNECTION_FAIL: return "connection_fail";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "handshake_timeout";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon_timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake_timeout";
    default: return "other";
    }
}

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

static esp_err_t nvs_get_string(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    size_t len = out_len;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK && out_len > 0) out[0] = '\0';
    return err;
}

static esp_err_t wifi_credentials_load(void)
{
    s_wifi_ssid[0] = '\0';
    s_wifi_pass[0] = '\0';

    if (nvs_get_string(NVS_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid)) == ESP_OK) {
        nvs_get_string(NVS_WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass));
    } else {
        strncpy(s_wifi_ssid, EYEMECH_WIFI_SSID, sizeof(s_wifi_ssid) - 1);
        strncpy(s_wifi_pass, EYEMECH_WIFI_PASSWORD, sizeof(s_wifi_pass) - 1);
    }

    s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
    s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
    if (strcmp(s_wifi_ssid, "your-ssid") == 0) s_wifi_ssid[0] = '\0';
    return s_wifi_ssid[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t wifi_credentials_save(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 ||
        (pass != NULL && strlen(pass) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_WIFI_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_WIFI_PASS, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void wifi_print_status(bool reveal_password)
{
    printf("\r\n");
    printf("wifi.mode=%s\r\n", s_setup_ap ? "setup_ap" : "station");
    printf("wifi.configured_ssid=%s\r\n", s_wifi_ssid[0] ? s_wifi_ssid : "(none)");
    if (reveal_password) {
        printf("wifi.password=%s\r\n", s_wifi_pass[0] ? s_wifi_pass : "(empty)");
    } else {
        printf("wifi.password=%s\r\n", s_wifi_pass[0] ? "(set; use !wifi reveal)" : "(empty)");
    }
    printf("wifi.retry=%d/%d\r\n", s_wifi_retry, WIFI_MAX_RETRIES);
    printf("wifi.last_disconnect=%u (%s)\r\n",
           s_last_wifi_reason, wifi_reason_name(s_last_wifi_reason));
    if (s_setup_ap) {
        printf("wifi.ap_ssid=%s\r\n", EYEMECH_SETUP_AP_SSID);
        printf("wifi.ap_url=http://192.168.4.1/\r\n");
    } else if (s_sta_ip.addr) {
        printf("wifi.url=http://" IPSTR "/\r\n", IP2STR(&s_sta_ip));
    } else {
        printf("wifi.url=(not connected)\r\n");
    }
    printf("\r\n");
}

static void serial_command_task(void *arg)
{
    (void)arg;
    char line[64];
    size_t len = 0;

    printf("\r\nserial commands: !wifi, !wifi reveal\r\n");
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            if (strcmp(line, "!wifi") == 0) {
                wifi_print_status(false);
            } else if (strcmp(line, "!wifi reveal") == 0) {
                wifi_print_status(true);
            } else if (len > 0) {
                printf("unknown command: %s\r\n", line);
                printf("serial commands: !wifi, !wifi reveal\r\n");
            }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        }
    }
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
    cJSON_AddStringToObject(root, "wifi_mode", s_setup_ap ? "setup_ap" : "station");
    cJSON_AddStringToObject(root, "wifi_ssid", s_setup_ap ? EYEMECH_SETUP_AP_SSID : s_wifi_ssid);
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
    if (eye_servo_save() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
    }
    return send_ok(req);
}

static esp_err_t release_post(httpd_req_t *req)
{
    eye_servo_release_all();
    return send_ok(req);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_json(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid)) {
        err = wifi_credentials_save(ssid->valuestring,
                                    cJSON_IsString(pass) ? pass->valuestring : "");
    }
    cJSON_Delete(body);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid wifi");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "restarting", true);
    send_json(req, root);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------- wifi */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_setup_ap) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_setup_ap) return;
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
        s_last_wifi_reason = evt->reason;
        s_sta_ip.addr = 0;
        ESP_LOGW(TAG, "wifi disconnect from '%s': reason=%u (%s)",
                 s_wifi_ssid, evt->reason, wifi_reason_name(evt->reason));
        if (s_wifi_retry++ < WIFI_MAX_RETRIES) {
            ESP_LOGW(TAG, "wifi disconnected, retrying (%d/%d)", s_wifi_retry, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "wifi connect failed");
            xEventGroupSetBits(s_wifi_events, STA_FAILED_BIT);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGW(TAG, "setup AP: connect to %s, then open http://192.168.4.1/",
                 EYEMECH_SETUP_AP_SSID);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_wifi_retry = 0;
        s_last_wifi_reason = 0;
        s_sta_ip = evt->ip_info.ip;
        xEventGroupSetBits(s_wifi_events, STA_CONNECTED_BIT);
        ESP_LOGI(TAG, "control page: http://" IPSTR "/", IP2STR(&evt->ip_info.ip));
    }
}

static esp_err_t setup_ap_start(void)
{
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, EYEMECH_SETUP_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(EYEMECH_SETUP_AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    if (EYEMECH_SETUP_AP_PASSWORD[0]) {
        strncpy((char *)ap.ap.password, EYEMECH_SETUP_AP_PASSWORD, sizeof(ap.ap.password) - 1);
        ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_setup_ap = true;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_wifi_started = true;
    }
    return ESP_OK;
}

static esp_err_t wifi_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) return ESP_ERR_NO_MEM;

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, EYEMECH_HOSTNAME);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    if (wifi_credentials_load() != ESP_OK) {
        ESP_LOGW(TAG, "no wifi credentials; starting setup AP");
        return setup_ap_start();
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     s_wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_wifi_pass, sizeof(wc.sta.password) - 1);
    ESP_LOGI(TAG, "joining wifi '%s'", s_wifi_ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "sta config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_wifi_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, STA_CONNECTED_BIT | STA_FAILED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_WAIT_MS));
    if (bits & STA_CONNECTED_BIT) return ESP_OK;

    ESP_LOGW(TAG, "station wifi unavailable; starting setup AP");
    return setup_ap_start();
}

static const httpd_uri_t s_routes[] = {
    { .uri = "/",             .method = HTTP_GET,  .handler = root_get },
    { .uri = "/api/state",    .method = HTTP_GET,  .handler = state_get },
    { .uri = "/api/mode",     .method = HTTP_POST, .handler = mode_post },
    { .uri = "/api/look",     .method = HTTP_POST, .handler = look_post },
    { .uri = "/api/lid_trim", .method = HTTP_POST, .handler = lid_trim_post },
    { .uri = "/api/blink",    .method = HTTP_POST, .handler = blink_post },
    { .uri = "/api/servo",    .method = HTTP_POST, .handler = servo_post },
    { .uri = "/api/limits",   .method = HTTP_POST, .handler = limits_post },
    { .uri = "/api/cfg",      .method = HTTP_POST, .handler = cfg_post },
    { .uri = "/api/save",     .method = HTTP_POST, .handler = save_post },
    { .uri = "/api/release",  .method = HTTP_POST, .handler = release_post },
    { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
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
    xTaskCreate(serial_command_task, "serial_cmd", 3072, NULL, 1, NULL);
    ESP_LOGI(TAG, "http server listening on :80");
    return ESP_OK;
}
