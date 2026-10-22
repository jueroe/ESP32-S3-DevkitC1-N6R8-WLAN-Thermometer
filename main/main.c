/*
 * Outdoor thermometer: SHT35 on I2C, readings as a web page and as JSON.
 *
 * Startup:
 *   1. NVS + time zone + sensor (the sensor runs even without a network)
 *   2. BOOT held at reset -> configuration access point
 *   3. otherwise log into the stored network; when that fails, open the
 *      configuration access point instead
 *   4. when online: readings endpoint, then NTP in the background
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"

#include "board.h"
#include "clock.h"
#include "configmode.h"
#include "connect.h"
#include "netcfg.h"
#include "sensor.h"
#include "webserver.h"
#include "wifi.h"

static const char *TAG = "app";

/* How often the sensor is read.  The web page refreshes every 10 s, so one
 * measurement per 10 s keeps the cached value fresh. */
#define SENSOR_INTERVAL_MS 10000

static void sensor_task(void *argument)
{
    (void)argument;

    for (;;) {
        sensor_reading_t reading;
        if (sensor_read(&reading) == ESP_OK) {
            ESP_LOGI(TAG, "%.2f C, %.1f %%rH", reading.temperature_c, reading.humidity_rh);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

/* NTP can take a while and is not needed for the readings, so it runs on its
 * own task and never delays the web page. */
static void time_task(void *argument)
{
    (void)argument;

    clock_start_sntp();
    if (!clock_wait_for_sync(CONNECT_SNTP_WAIT_MS)) {
        /* the first query can get lost on a busy network */
        for (int attempt = 0; attempt < 3; attempt++) {
            ESP_LOGW(TAG, "retrying the NTP query (%d/3)", attempt + 1);
            esp_sntp_restart();
            if (clock_wait_for_sync(CONNECT_SNTP_WAIT_MS)) {
                break;
            }
        }
    }
    char stamp[32];
    ESP_LOGI(TAG, "time is %s", clock_format(stamp, sizeof(stamp)));
    vTaskDelete(NULL);
}

/* Holding the on-board BOOT button this long opens the configuration access
 * point while the unit is running.  It cannot be done "at reset": GPIO0 is the
 * download strap, so a button held during reset puts the chip into the ROM
 * download mode and the application never starts. */
#define CONFIG_BUTTON_HOLD_MS 3000
#define BUTTON_POLL_MS        50

/* GPIO0 is pulled up on the board, the button connects it to ground. */
static void config_button_task(void *argument)
{
    (void)argument;

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);

    int held_ms = 0;
    bool announced = false;
    for (;;) {
        if (gpio_get_level(BOARD_BOOT_BUTTON_GPIO) == 0) {
            held_ms += BUTTON_POLL_MS;
            if (!announced) {
                ESP_LOGI(TAG, "BOOT pressed, hold for %d s to open the configuration",
                         CONFIG_BUTTON_HOLD_MS / 1000);
                announced = true;
            }
            if (held_ms >= CONFIG_BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held - opening the configuration access point");
                if (configmode_start() != ESP_OK) {
                    ESP_LOGE(TAG, "the configuration is not available");
                }
                vTaskDelete(NULL);
            }
        } else {
            held_ms = 0;
            announced = false;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static const char *reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply too weak)";
    case ESP_RST_DEEPSLEEP: return "wakeup from deep sleep";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "outdoor thermometer starting (reset: %s)",
             reset_reason_text(esp_reset_reason()));

    if (netcfg_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS is not available, settings cannot be stored");
    }
    clock_init();

    if (sensor_init() != ESP_OK) {
        ESP_LOGW(TAG, "no sensor - the web page shows no values");
    }
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 4, NULL);

    netcfg_t cfg;
    const bool stored = netcfg_load(&cfg);
    ESP_LOGI(TAG, "WLAN \"%s\" from %s, device name \"%s\"", cfg.ssid,
             stored ? "NVS" : "connect.h", cfg.name);

    bool connected = false;
    if (wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "the WiFi driver did not come up");
    } else {
        /* Look for the network first: when it is not there, the connection
         * attempt would only cost the full timeout before the configuration
         * starts. */
        const int rssi = wifi_scan_rssi(cfg.ssid);
        if (rssi == WIFI_RSSI_NOT_FOUND) {
            ESP_LOGW(TAG, "\"%s\" is not in the scan result", cfg.ssid);
        } else if (rssi < CONNECT_MIN_RSSI) {
            ESP_LOGW(TAG, "\"%s\" is too weak: %d dBm, at least %d dBm needed", cfg.ssid, rssi,
                     CONNECT_MIN_RSSI);
        } else {
            ESP_LOGI(TAG, "\"%s\" found with %d dBm", cfg.ssid, rssi);
            connected = wifi_connect(cfg.ssid, cfg.password, CONNECT_WIFI_TIMEOUT_MS);
        }
    }

    if (!connected) {
        ESP_LOGW(TAG, "no network - opening the configuration access point");
        if (configmode_start() != ESP_OK) {
            ESP_LOGE(TAG, "the configuration is not available either");
        }
        return;
    }

    wifi_log_link();
    if (webserver_start() != ESP_OK) {
        ESP_LOGW(TAG, "the readings endpoint is not available");
    }
    /* runs from now on: holding BOOT for 3 s switches to the configuration */
    xTaskCreate(config_button_task, "button", 3072, NULL, 2, NULL);
    xTaskCreate(time_task, "time", 4096, NULL, 3, NULL);
}