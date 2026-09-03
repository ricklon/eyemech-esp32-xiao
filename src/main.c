/*
 * eyemech-esp32-xiao — animatronic eye mechanism on a Seeed XIAO ESP32-S3.
 *
 * Boot order: NVS -> servos (loads calibration, parks at center) ->
 * motion task -> WiFi + HTTP control surface.
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_web.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(eye_servo_init());
    ESP_ERROR_CHECK(eye_motion_start());

    /* WiFi failure is not fatal — the mechanism should still idle. */
    err = eye_web_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web control unavailable: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "eyemech " EYEMECH_VERSION " up");
}
