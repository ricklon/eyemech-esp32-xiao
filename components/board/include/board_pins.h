#pragma once
/*
 * board_pins.h — the one place hardware differences live.
 *
 * Both XIAO variants expose the same D0..D10 silk, but the GPIO numbers behind
 * them differ, so everything else in the firmware refers to these names and
 * never to a raw GPIO. Adding a third board means adding a block here and
 * nothing else.
 *
 * Pin roles come from docs/WIRING.md. The pots and switches of the MicroPython
 * build are gone — the web control surface replaced them — so D0..D3, D8 and D9
 * are free. They are listed as BOARD_PIN_FREE_* so the freed pins stay visible
 * rather than being silently forgotten.
 */
#include "driver/gpio.h"

#if defined(EYEMECH_BOARD_XIAO_C6) || defined(CONFIG_IDF_TARGET_ESP32C6)

#define BOARD_NAME          "XIAO ESP32-C6"
#define BOARD_PIN_I2C_SDA   GPIO_NUM_22   /* D4 */
#define BOARD_PIN_I2C_SCL   GPIO_NUM_23   /* D5 */
#define BOARD_PIN_PCA_OE    GPIO_NUM_18   /* D10 — active low, 10k pull-up to 3V3 */
#define BOARD_PIN_GROVE_TX  GPIO_NUM_16   /* D6 -> Grove Vision RX */
#define BOARD_PIN_GROVE_RX  GPIO_NUM_17   /* D7 <- Grove Vision TX */
#define BOARD_PIN_LED       GPIO_NUM_15   /* user LED, ACTIVE LOW */
#define BOARD_LED_ACTIVE_LOW 1
/* Freed by dropping the pots/switches: D0=0 D1=1 D2=2 D3=21 D8=19 D9=20 */

#elif defined(EYEMECH_BOARD_XIAO_S3) || defined(CONFIG_IDF_TARGET_ESP32S3)

#define BOARD_NAME          "XIAO ESP32-S3"
#define BOARD_PIN_I2C_SDA   GPIO_NUM_5    /* D4 */
#define BOARD_PIN_I2C_SCL   GPIO_NUM_6    /* D5 */
#define BOARD_PIN_PCA_OE    GPIO_NUM_9    /* D10 — active low, 10k pull-up to 3V3 */
#define BOARD_PIN_GROVE_TX  GPIO_NUM_43   /* D6 -> Grove Vision RX */
#define BOARD_PIN_GROVE_RX  GPIO_NUM_44   /* D7 <- Grove Vision TX */
#define BOARD_PIN_LED       GPIO_NUM_21   /* user LED, ACTIVE LOW */
#define BOARD_LED_ACTIVE_LOW 1
/* Freed by dropping the pots/switches: D0=1 D1=2 D2=3 D3=4 D8=7 D9=8 */

#else
#error "Unknown board — add a block to board_pins.h"
#endif

/* Shared across boards */
#define BOARD_I2C_PORT      I2C_NUM_0
#define BOARD_I2C_FREQ_HZ   400000    /* 100 kHz caps the loop near 330 Hz */
#define BOARD_GROVE_UART    UART_NUM_1
#define BOARD_GROVE_BAUD    921600
