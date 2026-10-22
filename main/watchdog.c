#include "watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "watchdog";

/* Longest gap between two feeds: the sensor task sleeps 10 s. */
#define WATCHDOG_TIMEOUT_MS 60000

void watchdog_init(void)
{
    const esp_task_wdt_config_t config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        /* only the tasks below are watched - the idle tasks are left alone so
         * that long flash/erase operations do not trip the watchdog */
        .idle_core_mask = 0,
        .trigger_panic = true,
    };

    /* CONFIG_ESP_TASK_WDT_INIT already initialised it with the 5 s default */
    esp_err_t err = esp_task_wdt_reconfigure(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot configure the task watchdog: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "task watchdog: %d s, panic on timeout", WATCHDOG_TIMEOUT_MS / 1000);
}

void watchdog_subscribe(void)
{
    const esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "cannot watch this task: %s", esp_err_to_name(err));
    }
}

void watchdog_unsubscribe(void)
{
    esp_task_wdt_delete(NULL);
}

void watchdog_feed(void)
{
    esp_task_wdt_reset();
}
