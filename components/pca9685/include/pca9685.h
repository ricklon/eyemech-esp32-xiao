#pragma once
/*
 * pca9685 — direct port of micropython/pca9685.py.
 *
 * Driving hobby servos only: one shared frequency across all 16 channels and
 * ON-time always 0, so only the OFF tick is ever computed.
 *
 * Clone boards frequently do NOT have a true 25 MHz oscillator — 24 to 27 MHz
 * is common. If servos land consistently off-centre, measure the actual output
 * period on a scope while commanding 50 Hz and pass the corrected value to
 * pca9685_trim_oscillator().
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCA9685_DEFAULT_ADDR   0x40
#define PCA9685_DEFAULT_OSC_HZ 25000000
#define PCA9685_CHANNELS       16

typedef struct {
    uint8_t  address;
    uint32_t osc_hz;
    float    period_us;
} pca9685_t;

/* Resets the chip (auto-increment on, totem-pole outputs) and sets freq. */
esp_err_t pca9685_init(pca9685_t *dev, uint8_t address, uint32_t osc_hz, uint32_t freq_hz);

esp_err_t pca9685_set_freq(pca9685_t *dev, uint32_t freq_hz);
esp_err_t pca9685_set_pwm(pca9685_t *dev, uint8_t channel, uint16_t on, uint16_t off);

/* Pulse width in microseconds. us <= 0 drives the channel full off (limp). */
esp_err_t pca9685_set_us(pca9685_t *dev, uint8_t channel, int us);

/* Last pulse width actually loaded into a channel's registers, in microseconds.
 *
 * The PCA9685 keeps its LEDn registers across an ESP32 reset — nothing ties the
 * two resets together — so this recovers where the mechanism was left. On
 * feedback-free servos it is the only position knowledge that survives a reboot.
 *
 * Returns 0 when the channel is not driving (full-off bit set, or a cold
 * power-on that zeroed the registers), and -1 on an I2C error. */
int pca9685_get_us(pca9685_t *dev, uint8_t channel);

/* Release every channel at once. */
esp_err_t pca9685_all_off(pca9685_t *dev);

esp_err_t pca9685_sleep(pca9685_t *dev);
esp_err_t pca9685_wake(pca9685_t *dev);

/* Command 50 Hz, measure the real frequency, pass it here for the true osc_hz.
 * e.g. measuring 52.4 Hz means the oscillator runs ~4.8% fast -> ~26.2 MHz. */
uint32_t pca9685_trim_oscillator(float measured_hz_at_50hz);

/* /OE control. Active low: enabled == outputs driving.
 *
 * This build has a 10k pull-DOWN on /OE, so outputs are enabled by default and
 * stay that way through reset. Do NOT use this to gate the boot sequence:
 * disabling outputs before positions are seeded makes the servos sag and then
 * snap back. Its job is the emergency release — pca9685_oe_set(false) drops
 * every output low instantly with no I2C transaction, which is the only stop
 * that still works with a wedged bus or a crashed MCU. See docs/WIRING.md. */
esp_err_t pca9685_oe_init(void);
esp_err_t pca9685_oe_set(bool enabled);

#ifdef __cplusplus
}
#endif
