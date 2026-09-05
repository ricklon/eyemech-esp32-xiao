#include "pca9685.h"
#include "board_pins.h"

#include <inttypes.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pca9685";

#define REG_MODE1       0x00
#define REG_MODE2       0x01
#define REG_PRESCALE    0xFE
#define REG_LED0_ON_L   0x06
#define REG_ALL_LED_ON_L 0xFA

#define MODE1_RESTART   0x80
#define MODE1_AI        0x20   /* register auto-increment */
#define MODE1_SLEEP     0x10
#define MODE1_ALLCALL   0x01
#define MODE2_OUTDRV    0x04   /* totem-pole outputs, required for servos */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t bus_init(uint8_t address)
{
    if (s_bus != NULL) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

static esp_err_t w8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t r8(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, pdMS_TO_TICKS(50));
}

esp_err_t pca9685_init(pca9685_t *dev, uint8_t address, uint32_t osc_hz, uint32_t freq_hz)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    dev->address = address;
    dev->osc_hz  = osc_hz;

    ESP_RETURN_ON_ERROR(bus_init(address), TAG, "bus");
    ESP_RETURN_ON_ERROR(w8(REG_MODE1, MODE1_AI | MODE1_ALLCALL), TAG, "mode1");
    ESP_RETURN_ON_ERROR(w8(REG_MODE2, MODE2_OUTDRV), TAG, "mode2");
    vTaskDelay(pdMS_TO_TICKS(5));

    ESP_RETURN_ON_ERROR(pca9685_set_freq(dev, freq_hz), TAG, "freq");
    ESP_LOGI(TAG, "ready at 0x%02x, %" PRIu32 " Hz, osc %" PRIu32, address, freq_hz, osc_hz);
    return ESP_OK;
}

esp_err_t pca9685_set_freq(pca9685_t *dev, uint32_t freq_hz)
{
    /* Datasheet: prescale = round(osc / (4096 * freq)) - 1 */
    int prescale = (int)((float)dev->osc_hz / (4096.0f * (float)freq_hz) + 0.5f) - 1;
    if (prescale < 3)   prescale = 3;
    if (prescale > 255) prescale = 255;

    /* PRESCALE is only writable while the oscillator is asleep. */
    uint8_t old = 0;
    ESP_RETURN_ON_ERROR(r8(REG_MODE1, &old), TAG, "read mode1");
    ESP_RETURN_ON_ERROR(w8(REG_MODE1, (old & ~MODE1_RESTART) | MODE1_SLEEP), TAG, "sleep");
    ESP_RETURN_ON_ERROR(w8(REG_PRESCALE, (uint8_t)prescale), TAG, "prescale");
    ESP_RETURN_ON_ERROR(w8(REG_MODE1, old), TAG, "restore");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(w8(REG_MODE1, old | MODE1_RESTART | MODE1_AI), TAG, "restart");

    dev->period_us = 1000000.0f / (float)freq_hz;
    return ESP_OK;
}

esp_err_t pca9685_set_pwm(pca9685_t *dev, uint8_t channel, uint16_t on, uint16_t off)
{
    (void)dev;
    if (channel >= PCA9685_CHANNELS) return ESP_ERR_INVALID_ARG;
    uint8_t buf[5] = {
        (uint8_t)(REG_LED0_ON_L + 4 * channel),
        (uint8_t)(on & 0xFF),
        (uint8_t)((on >> 8) & 0x1F),
        (uint8_t)(off & 0xFF),
        (uint8_t)((off >> 8) & 0x1F),
    };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

esp_err_t pca9685_set_us(pca9685_t *dev, uint8_t channel, int us)
{
    if (us <= 0) {
        return pca9685_set_pwm(dev, channel, 0, 0x1000);  /* bit 12 = full off */
    }
    int ticks = (int)((float)us * 4096.0f / dev->period_us);
    if (ticks > 4095) ticks = 4095;
    return pca9685_set_pwm(dev, channel, 0, (uint16_t)ticks);
}

int pca9685_get_us(pca9685_t *dev, uint8_t channel)
{
    if (dev == NULL || channel >= PCA9685_CHANNELS) return -1;

    /* OFF_L/OFF_H sit two bytes past this channel's ON_L. */
    uint8_t reg = (uint8_t)(REG_LED0_ON_L + 4 * channel + 2);
    uint8_t buf[2] = { 0, 0 };
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf),
                                    pdMS_TO_TICKS(50)) != ESP_OK) {
        return -1;
    }

    if (buf[1] & 0x10) return 0;   /* full-off bit: channel is released */

    /* 12-bit count: OFF_H bits 3:0 are the high nibble, bit 4 is the flag. */
    uint16_t off = (uint16_t)buf[0] | (uint16_t)((buf[1] & 0x0F) << 8);
    if (off == 0) return 0;        /* nothing ever loaded (cold power-on) */

    return (int)((float)off * dev->period_us / 4096.0f + 0.5f);
}

esp_err_t pca9685_all_off(pca9685_t *dev)
{
    (void)dev;
    uint8_t buf[5] = { REG_ALL_LED_ON_L, 0, 0, 0, 0x10 };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

esp_err_t pca9685_sleep(pca9685_t *dev)
{
    (void)dev;
    uint8_t m = 0;
    ESP_RETURN_ON_ERROR(r8(REG_MODE1, &m), TAG, "read mode1");
    return w8(REG_MODE1, m | MODE1_SLEEP);
}

esp_err_t pca9685_wake(pca9685_t *dev)
{
    (void)dev;
    uint8_t m = 0;
    ESP_RETURN_ON_ERROR(r8(REG_MODE1, &m), TAG, "read mode1");
    ESP_RETURN_ON_ERROR(w8(REG_MODE1, m & ~MODE1_SLEEP), TAG, "wake");
    vTaskDelay(pdMS_TO_TICKS(1));
    return ESP_OK;
}

uint32_t pca9685_trim_oscillator(float measured_hz_at_50hz)
{
    return (uint32_t)(25000000.0f * (measured_hz_at_50hz / 50.0f));
}

/* ------------------------------------------------------------------- /OE */

esp_err_t pca9685_oe_init(void)
{
    /* No internal pull: the board carries an external 10k pull-down, and an
     * internal pull-up against it would only make a ~0.6 V divider. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PCA_OE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "oe gpio");
    /* Start ENABLED, matching the pull-down's default. Holding outputs off
     * across init would drop the servos limp and let them sag, then snap them
     * back when re-enabled; the pull-down means they instead hold whatever the
     * PCA9685 was already emitting. See docs/WIRING.md. */
    return gpio_set_level(BOARD_PIN_PCA_OE, 0);
}

esp_err_t pca9685_oe_set(bool enabled)
{
    return gpio_set_level(BOARD_PIN_PCA_OE, enabled ? 0 : 1);
}
