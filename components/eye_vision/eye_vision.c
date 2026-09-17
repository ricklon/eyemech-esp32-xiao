#include "eye_vision.h"
#include "board_pins.h"

#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eye_vision";

static const char INVOKE_CMD[] = "AT+INVOKE=1,0,1\r";

static char   s_buf[EYE_VISION_BUF_MAX + 1];
static size_t s_len;
static bool   s_read_flag = true;      /* true == time to re-arm and re-invoke */
static bool   s_present;
static char   s_last_boxes[128];

/* The module streams at 921600. An undersized RX buffer overruns mid-packet and
 * silently corrupts box parsing — the same failure the MicroPython port hit
 * before rxbuf was raised. Keep this generous. */
#define UART_RX_BUF 4096

esp_err_t eye_vision_init(void)
{
    uart_config_t cfg = {
        .baud_rate = BOARD_GROVE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* ESP_INTR_FLAG_IRAM must be requested here to match
     * CONFIG_UART_ISR_IN_IRAM in sdkconfig.defaults. Without it IDF logs
     * "flag not set while CONFIG_UART_ISR_IN_IRAM is enabled, flag updated"
     * on every boot and patches it after the fact. */
    ESP_RETURN_ON_ERROR(uart_driver_install(BOARD_GROVE_UART, UART_RX_BUF, 0, 0, NULL,
                                            ESP_INTR_FLAG_IRAM),
                        TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(BOARD_GROVE_UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BOARD_GROVE_UART, BOARD_PIN_GROVE_TX, BOARD_PIN_GROVE_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pins");

    uart_flush_input(BOARD_GROVE_UART);
    uart_write_bytes(BOARD_GROVE_UART, INVOKE_CMD, strlen(INVOKE_CMD));

    /* Give it a moment to answer; anything at all means a module is attached. */
    vTaskDelay(pdMS_TO_TICKS(150));
    size_t avail = 0;
    uart_get_buffered_data_len(BOARD_GROVE_UART, &avail);
    s_present = (avail > 0);

    ESP_LOGI(TAG, "grove vision module %s", s_present ? "detected" : "not detected");
    return s_present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool eye_vision_present(void) { return s_present; }

static void rearm(void)
{
    s_len = 0;
    s_buf[0] = '\0';
    s_read_flag = true;
}

esp_err_t eye_vision_poll(eye_vision_offset_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    size_t avail = 0;
    uart_get_buffered_data_len(BOARD_GROVE_UART, &avail);

    if (s_read_flag) {
        if (avail > 0) {
            uart_flush_input(BOARD_GROVE_UART);
            s_present = true;
        }
        uart_write_bytes(BOARD_GROVE_UART, INVOKE_CMD, strlen(INVOKE_CMD));
        s_len = 0;
        s_buf[0] = '\0';
        s_read_flag = false;
        return ESP_ERR_NOT_FOUND;
    }

    if (avail == 0) return ESP_ERR_NOT_FOUND;

    size_t room = EYE_VISION_BUF_MAX - s_len;
    int got = uart_read_bytes(BOARD_GROVE_UART, (uint8_t *)(s_buf + s_len),
                              (avail < room) ? avail : room, 0);
    if (got > 0) {
        s_len += (size_t)got;
        s_buf[s_len] = '\0';
    }

    /* Runaway guard: the original could grow its buffer without limit if the
     * module stopped sending the resolution key. */
    if (s_len >= EYE_VISION_BUF_MAX) {
        ESP_LOGW(TAG, "buffer full without a complete frame — resetting");
        rearm();
        return ESP_ERR_NOT_FOUND;
    }

    if (strstr(s_buf, "\"resolution\"") == NULL) return ESP_ERR_NOT_FOUND;

    const char *key = "\"boxes\":";
    char *start = strstr(s_buf, key);
    if (start == NULL) return ESP_ERR_NOT_FOUND;
    start += strlen(key);

    char *end = strchr(start, ']');
    if (end == NULL) return ESP_ERR_NOT_FOUND;

    size_t boxes_len = (size_t)(end - start) + 1;
    if (boxes_len >= sizeof(s_last_boxes)) { rearm(); return ESP_ERR_NOT_FOUND; }

    char boxes[sizeof(s_last_boxes)];
    memcpy(boxes, start, boxes_len);
    boxes[boxes_len] = '\0';

    /* Empty result, or identical to last frame: the subject is holding still. */
    if (strcmp(boxes, "[]") == 0 || strcmp(boxes, s_last_boxes) == 0) {
        rearm();
        out->x = 0.0f;
        out->y = 0.0f;
        out->is_static = true;
        return ESP_OK;
    }
    strcpy(s_last_boxes, boxes);

    /* boxes looks like [cx,cy,w,h,score,target] — the first two are what we
     * steer on. */
    const char *p = boxes;
    while (*p && (*p == '[' || *p == ' ')) p++;
    char *after = NULL;
    long cx = strtol(p, &after, 10);
    if (after == p) { rearm(); return ESP_ERR_NOT_FOUND; }
    while (*after && (*after == ',' || *after == ' ')) after++;
    const char *q = after;
    long cy = strtol(q, &after, 10);
    if (after == q) { rearm(); return ESP_ERR_NOT_FOUND; }

    out->x = (float)(cx - EYE_VISION_PIXEL_CENTRE);
    out->y = (float)(cy - EYE_VISION_PIXEL_CENTRE);
    out->is_static = false;

    rearm();
    return ESP_OK;
}
