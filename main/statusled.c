#include "statusled.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_strip.h"

#include "board.h"
#include "watchdog.h"
#include "wifi.h"

static const char *TAG = "statusled";

#define BLINK_ON_MS  125
#define BLINK_OFF_MS 125
#define BURST_GAP_MS 1000
#define LED_TICK_MS  10

/* Both pins of the two DevKitC-1 revisions (v1.1: 38, initial version: 48).
 * The unused one drives nothing. */
static const int s_pins[] = { BOARD_RGB_LED_GPIO, BOARD_RGB_LED_GPIO_ALT };

static led_strip_handle_t s_strips[sizeof(s_pins) / sizeof(s_pins[0])];
static bool s_ready;

static volatile statusled_wifi_t s_wifi_state = STATUSLED_WIFI_CONNECTING;
static volatile bool s_sensor_ok = true;

/* True during the on-phases of `count` blinks followed by a longer pause. */
static bool burst(unsigned count, uint32_t elapsed_ms)
{
    const uint32_t cycle = BLINK_ON_MS + BLINK_OFF_MS;
    const uint32_t period = count * cycle + BURST_GAP_MS;
    const uint32_t phase = elapsed_ms % period;
    return phase < count * cycle && (phase % cycle) < BLINK_ON_MS;
}

/* Continuous blinking at 4 Hz */
static bool blink_fast(uint32_t elapsed_ms)
{
    return (elapsed_ms % (BLINK_ON_MS + BLINK_OFF_MS)) < BLINK_ON_MS;
}

/* Signal strength to number of blinks; 0 means "LED off" */
static unsigned signal_blinks(int rssi)
{
    if (rssi >= -50) {
        return 3;
    }
    if (rssi >= -65) {
        return 2;
    }
    if (rssi >= -80) {
        return 1;
    }
    return 0;
}

static void show(uint32_t color)
{
    const uint8_t red = (uint8_t)(color >> 16);
    const uint8_t green = (uint8_t)(color >> 8);
    const uint8_t blue = (uint8_t)color;

    for (size_t i = 0; i < sizeof(s_strips) / sizeof(s_strips[0]); i++) {
        if (s_strips[i] == NULL) {
            continue;
        }
        led_strip_set_pixel(s_strips[i], 0, red, green, blue);
        led_strip_refresh(s_strips[i]);
    }
}

static void statusled_task(void *argument)
{
    (void)argument;

    watchdog_subscribe();

    const int64_t start_us = esp_timer_get_time();
    uint32_t elapsed_ms = 0;
    uint32_t last_poll_ms = 0;
    int rssi = 0;
    uint32_t last_color = 0xFFFFFFFF;

    for (;;) {
        /* the signal strength is only needed for the blue LED, and polling it
         * costs a call into the WiFi driver */
        if (elapsed_ms - last_poll_ms >= 1000) {
            last_poll_ms = elapsed_ms;
            rssi = s_wifi_state == STATUSLED_WIFI_CONNECTED ? wifi_rssi() : 0;
        }

        bool green = false;
        switch (s_wifi_state) {
        case STATUSLED_WIFI_CONNECTED: green = true; break;
        case STATUSLED_WIFI_AP: green = burst(2, elapsed_ms); break;
        case STATUSLED_WIFI_CONNECTING:
        default: green = blink_fast(elapsed_ms); break;
        }

        const bool red = !s_sensor_ok;

        bool blue = false;
        if (s_wifi_state == STATUSLED_WIFI_CONNECTED && rssi != 0) {
            const unsigned blinks = signal_blinks(rssi);
            blue = blinks > 0 && burst(blinks, elapsed_ms);
        }

        const uint32_t color = (red ? (uint32_t)BOARD_RGB_LED_BRIGHTNESS << 16 : 0) |
                               (green ? (uint32_t)BOARD_RGB_LED_BRIGHTNESS_GREEN << 8 : 0) |
                               (blue ? BOARD_RGB_LED_BRIGHTNESS : 0);

        if (color != last_color) {
            last_color = color;
            show(color);
        }

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
        watchdog_feed();
        elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    }
}

esp_err_t statusled_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    size_t used = 0;
    for (size_t i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); i++) {
        if (s_pins[i] <= 0) {
            continue;
        }

        const led_strip_config_t strip_config = {
            .strip_gpio_num = s_pins[i],
            .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
            .flags.invert_out = false,
        };
        const led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            /* one pixel needs 24 bits x 2 symbols; the default (48) would be
             * exactly at the limit */
            .mem_block_symbols = 64,
            .flags.with_dma = false,
        };

        const esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strips[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no LED on GPIO%d: %s", s_pins[i], esp_err_to_name(err));
            s_strips[i] = NULL;
            result = err;
            continue;
        }
        show(0x000000); /* start dark, then the task takes over */
        ESP_LOGI(TAG, "status LED found on GPIO%d", s_pins[i]);
        used++;
    }

    if (used == 0) {
        ESP_LOGW(TAG, "no status LED available");
        return result;
    }

    s_ready = true;
    xTaskCreate(statusled_task, "statusled", 3072, NULL, 2, NULL);
    return ESP_OK;
}

void statusled_set_wifi(statusled_wifi_t state)
{
    if (s_wifi_state == state) {
        return;
    }
    s_wifi_state = state;

    switch (state) {
    case STATUSLED_WIFI_CONNECTED:
        ESP_LOGI(TAG, "WLAN lamp: steady");
        break;
    case STATUSLED_WIFI_AP:
        ESP_LOGI(TAG, "WLAN lamp: 2 blinks (configuration mode)");
        break;
    case STATUSLED_WIFI_CONNECTING:
    default:
        ESP_LOGI(TAG, "WLAN lamp: 4 Hz blinking");
        break;
    }
}

void statusled_set_sensor_ok(bool ok)
{
    if (s_sensor_ok == ok) {
        return;
    }
    s_sensor_ok = ok;
    ESP_LOGI(TAG, "sensor lamp: %s", ok ? "off" : "on (sensor missing)");
}
