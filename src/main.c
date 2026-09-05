/*
 * eyemech-esp32-xiao — Will Cogley's eye mechanism, ported from MicroPython to
 * ESP-IDF. See micropython/ for the original, docs/PORTING.md for the mapping.
 *
 * Boot order matters, but /OE is not what makes it matter. This build has a
 * 10k pull-DOWN on /OE, so outputs are live throughout boot: on a cold start
 * the PCA9685 has nothing loaded and emits no pulses, and on a warm reset it
 * keeps emitting the last ones, so the servos hold. The slam risk is writing
 * new positions to all six channels at once — see docs/WIRING.md.
 */
#include "board_pins.h"
#include "eye_console.h"
#include "eye_motion.h"
#include "eye_net.h"
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

    /* 1. /OE as an output, held ENABLED to match the board's 10k pull-down.
     *    This is not a gate on the boot sequence — it only claims the pin so
     *    the release path can drive it later. */
    ESP_ERROR_CHECK(pca9685_oe_init());

    /* 2. Bring up the driver and load calibration. Note that pca9685_init()
     *    deliberately does NOT clear the LEDn registers: on a warm reset they
     *    still hold the last commanded pulses, and they are the only record of
     *    where the mechanism is, since these servos have no feedback. */
    ESP_ERROR_CHECK(pca9685_init(&s_pca, PCA9685_DEFAULT_ADDR,
                                 PCA9685_DEFAULT_OSC_HZ, 50));
    ESP_ERROR_CHECK(eye_servo_init(&s_pca));

    /* 3. Recover that position and ease into neutral rather than commanding it
     *    outright, which would drive all six servos there at full speed. A
     *    cold start recovers nothing and goes straight to neutral. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(eye_servo_resume_from_hardware());
    ESP_ERROR_CHECK(eye_motion_resume_to_neutral());

    /* 4. Vision decides the default mode, exactly as the original did. */
    bool vision = (eye_vision_init() == ESP_OK);
    eye_motion_set_mode(vision ? EYE_MODE_TRACKING : EYE_MODE_AUTO);

    ESP_ERROR_CHECK(eye_motion_start());

    /* 5. Console before WiFi, and never fatal. It is the control surface that
     *    survives a network that does not come up, which is exactly when you
     *    most want to be able to type !release. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(eye_console_start());

    /* 6. Networking, then the HTTP surface on top of it. Neither is fatal —
     *    the mechanism runs standalone, and the console above is the control
     *    surface that does not need either of them. */
    err = eye_net_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "networking unavailable: %s", esp_err_to_name(err));
    } else {
        err = eye_web_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web control unavailable: %s", esp_err_to_name(err));
        }
    }

    led_set(false);
    ESP_LOGI(TAG, "eyemech " EYEMECH_VERSION " up on " BOARD_NAME);
}
