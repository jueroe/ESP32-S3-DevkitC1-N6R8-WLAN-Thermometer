#include "discovery.h"

#include <stdbool.h>

#include "esp_log.h"
#include "mdns.h"

#include "netcfg.h"

static const char *TAG = "discovery";

static bool s_running;

esp_err_t discovery_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    char hostname[33];
    netcfg_hostname(hostname, sizeof(hostname));

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set(\"%s\"): %s", hostname, esp_err_to_name(err));
        mdns_free();
        return err;
    }

    /* the readable name with spaces and umlauts; not used for addressing */
    mdns_instance_name_set(netcfg_device_name());

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "announcing the HTTP service failed: %s", esp_err_to_name(err));
    }

    s_running = true;
    ESP_LOGI(TAG, "announced as http://%s.local/ (\"%s\")", hostname, netcfg_device_name());
    return ESP_OK;
}

void discovery_stop(void)
{
    if (!s_running) {
        return;
    }
    mdns_free();
    s_running = false;
    ESP_LOGI(TAG, "announcement stopped");
}
