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

/* Release every channel at once. */
esp_err_t pca9685_all_off(pca9685_t *dev);

esp_err_t pca9685_sleep(pca9685_t *dev);
esp_err_t pca9685_wake(pca9685_t *dev);

/* Command 50 Hz, measure the real frequency, pass it here for the true osc_hz.
 * e.g. measuring 52.4 Hz means the oscillator runs ~4.8% fast -> ~26.2 MHz. */
uint32_t pca9685_trim_oscillator(float measured_hz_at_50hz);

/* /OE control. Active low: enabled == outputs driving. Hold disabled until
 * every channel has a sane commanded position — see docs/WIRING.md. */
esp_err_t pca9685_oe_init(void);
esp_err_t pca9685_oe_set(bool enabled);

#ifdef __cplusplus
}
#endif
