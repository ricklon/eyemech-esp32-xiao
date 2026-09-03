/*
 * eyemech-esp32-xiao — Will Cogley's eye mechanism, ported from MicroPython to
 * ESP-IDF. See micropython/ for the original, docs/PORTING.md for the mapping.
 *
 * Boot order matters. The PCA9685's /OE stays HIGH (outputs Hi-Z, servos limp)
 * until every channel holds a sane position. Reordering this reintroduces the
 * full-speed slam through the linkages at reset that the /OE pull-up exists to
 * prevent — see docs/WIRING.md.
 */
#include "board_pins.h"
#include "eye_motion.h"
#include "eye_servo.h"
#include "eye_vision.h"
#include "eye_web.h"
#include "pca9685.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";
static pca9685_t s_pca;

static void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_PIN_LED, BOARD_LED_ACTIVE_LOW ? 1 : 0);   /* off */
}

static void led_set(bool on)
{
    gpio_set_level(BOARD_PIN_LED, BOARD_LED_ACTIVE_LOW ? !on : on);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    led_init();
    led_set(true);   /* on while probing */

    /* 1. /OE disabled BEFORE the chip is touched. */
    ESP_ERROR_CHECK(pca9685_oe_init());

    /* 2. Bring up the driver and load calibration. */
    ESP_ERROR_CHECK(pca9685_init(&s_pca, PCA9685_DEFAULT_ADDR,
                                 PCA9685_DEFAULT_OSC_HZ, 50));
    ESP_ERROR_CHECK(eye_servo_init(&s_pca));

    /* 3. Sane positions in every channel... */
    eye_motion_neutral();

    /* 4. ...and only now let the outputs drive. */
    ESP_ERROR_CHECK(pca9685_oe_set(true));

    /* 5. Vision decides the default mode, exactly as the original did. */
    bool vision = (eye_vision_init() == ESP_OK);
    eye_motion_set_mode(vision ? EYE_MODE_TRACKING : EYE_MODE_AUTO);

    ESP_ERROR_CHECK(eye_motion_start());

    /* WiFi failure is not fatal — the mechanism should still run standalone. */
    err = eye_web_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web control unavailable: %s", esp_err_to_name(err));
    }

    led_set(false);
    ESP_LOGI(TAG, "eyemech " EYEMECH_VERSION " up on " BOARD_NAME);
}
