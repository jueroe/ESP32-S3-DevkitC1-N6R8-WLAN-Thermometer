#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"

#include "connect.h"

static const char *TAG = "clock";

/* Below this year the clock counts from 1970 (or 2000), so NTP has not
 * answered yet */
#define CLOCK_VALID_YEAR 2024

static void sync_callback(struct timeval *tv)
{
    (void)tv;
    char stamp[32];
    ESP_LOGI(TAG, "NTP answer received, %s (%s)", clock_format(stamp, sizeof(stamp)),
             CONNECT_TZ_NAME);
}

void clock_init(void)
{
    setenv("TZ", CONNECT_TZ, 1);
    tzset();
    ESP_LOGI(TAG, "time zone %s (%s)", CONNECT_TZ_NAME, CONNECT_TZ);
}

void clock_start_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONNECT_SNTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(sync_callback);
    esp_sntp_init();
    ESP_LOGI(TAG, "NTP server %s, querying ...", CONNECT_SNTP_SERVER);
}

bool clock_wait_for_sync(uint32_t timeout_ms)
{
    const uint32_t step_ms = 100;
    for (uint32_t waited = 0; waited < timeout_ms; waited += step_ms) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(TAG, "clock synchronised after %u ms", (unsigned)waited);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    ESP_LOGW(TAG, "no NTP answer within %u ms", (unsigned)timeout_ms);
    return false;
}

bool clock_is_valid(void)
{
    struct tm local;
    return clock_local_now(&local);
}

bool clock_local_now(struct tm *out)
{
    const time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (out != NULL) {
        *out = local;
    }
    return local.tm_year + 1900 >= CLOCK_VALID_YEAR;
}

const char *clock_format(char *buffer, size_t size)
{
    struct tm local = { 0 };
    clock_local_now(&local);
    snprintf(buffer, size, "%02d.%02d.%04d %02d:%02d:%02d", local.tm_mday, local.tm_mon + 1,
             local.tm_year + 1900, local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}
