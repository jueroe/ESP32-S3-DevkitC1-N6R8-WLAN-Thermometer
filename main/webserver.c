#include "webserver.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "clock.h"
#include "netcfg.h"
#include "sensor.h"
#include "textutil.h"
#include "wifi.h"

static const char *TAG = "webserver";

static httpd_handle_t s_server;

/* value * 10 to an integer, rounded */
static int tenths(float value)
{
    return (int)lroundf(value * 10.0f);
}

/* Write `scaled` (value * 10) as a JSON number, e.g. 236 -> "23.6" */
static void json_number(char *out, size_t size, int scaled)
{
    const bool negative = scaled < 0;
    if (negative) {
        scaled = -scaled;
    }
    snprintf(out, size, "%s%d.%d", negative ? "-" : "", scaled / 10, scaled % 10);
}

/* Same number with a German decimal comma, for the HTML page */
static void html_number(char *out, size_t size, int scaled)
{
    const bool negative = scaled < 0;
    if (negative) {
        scaled = -scaled;
    }
    snprintf(out, size, "%s%d,%d", negative ? "-" : "", scaled / 10, scaled % 10);
}

/* "dd.mm. hh:mm", or the fallback while NTP has not answered */
static void local_stamp(char *out, size_t size)
{
    struct tm now;
    if (clock_local_now(&now)) {
        snprintf(out, size, "%02d.%02d. %02d:%02d", now.tm_mday, now.tm_mon + 1, now.tm_hour,
                 now.tm_min);
    } else {
        snprintf(out, size, "%s", "01.01. 00:00");
    }
}

static long uptime_seconds(void)
{
    return (long)(esp_timer_get_time() / 1000000);
}

static esp_err_t handler_values(httpd_req_t *req)
{
    sensor_reading_t reading;
    int64_t age_ms = 0;
    const bool valid = sensor_last(&reading, &age_ms);

    char temperature[16] = "null";
    char humidity[16] = "null";
    long age_s = 0;
    if (valid) {
        json_number(temperature, sizeof(temperature), tenths(reading.temperature_c));
        json_number(humidity, sizeof(humidity), tenths(reading.humidity_rh));
        age_s = (long)(age_ms / 1000);
    }

    char stamp[16];
    local_stamp(stamp, sizeof(stamp));

    char name[NETCFG_NAME_MAX * 2 + 1];
    json_escape(netcfg_device_name(), name, sizeof(name));

    char json[384];
    snprintf(json, sizeof(json),
             "{\"name\":\"%s\",\"valid\":%s,\"temperature_c\":%s,\"humidity_rh\":%s,"
             "\"age_s\":%ld,\"time\":\"%s\",\"ip\":\"%s\",\"rssi_dbm\":%d,\"uptime_s\":%ld}",
             name, valid ? "true" : "false", temperature, humidity, age_s, stamp,
             wifi_ip_address(), wifi_rssi(), uptime_seconds());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static const char *PAGE_HEAD =
    "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"refresh\" content=\"10\">"
    "<title>Thermometer</title><style>"
    "body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;margin:0;padding:20px;"
    "background:#101418;color:#e8eef5;text-align:center}"
    "h1{font-size:18px;font-weight:600;margin:0 0 24px;color:#9fb0c0}"
    ".value{font-size:52px;font-weight:600;margin:0 0 6px}"
    ".small{font-size:22px;color:#c6d2de;margin:0 0 20px}"
    ".hint{font-size:14px;color:#9fb0c0;margin:0 0 4px}"
    "code{background:#1b232b;padding:3px 7px;border-radius:6px;font-size:14px}"
    "</style></head><body>";

static esp_err_t handler_index(httpd_req_t *req)
{
    sensor_reading_t reading;
    int64_t age_ms = 0;
    const bool valid = sensor_last(&reading, &age_ms);

    char temperature[16] = "--";
    char humidity[16] = "--";
    if (valid) {
        html_number(temperature, sizeof(temperature), tenths(reading.temperature_c));
        html_number(humidity, sizeof(humidity), tenths(reading.humidity_rh));
    }

    char stamp[16];
    local_stamp(stamp, sizeof(stamp));

    char title[NETCFG_NAME_MAX * 6];
    html_escape(netcfg_device_name(), title, sizeof(title));

    char page[1200];
    snprintf(page, sizeof(page),
             "%s"
             "<h1>%s</h1>"
             "<p class=\"value\">%s &deg;C</p>"
             "<p class=\"small\">%s %%rF</p>"
             "<p class=\"small\">%s</p>"
             "<p class=\"hint\">%s, %d dBm, %s</p>"
             "<p class=\"hint\">JSON: <code>/api/values</code></p>"
             "</body></html>",
             PAGE_HEAD, title, temperature, humidity, stamp, wifi_ip_address(), wifi_rssi(),
             valid ? "Sensor bereit" : "noch kein Messwert");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t webserver_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;
    /* a thermometer does not serve a crowd; every socket costs lwIP buffers */
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_index, .user_ctx = NULL},
        {.uri = "/api", .method = HTTP_GET, .handler = handler_values, .user_ctx = NULL},
        {.uri = "/api/values", .method = HTTP_GET, .handler = handler_values, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "readings at http://%s/api/values (stack %d bytes, %d sockets)",
             wifi_ip_address(), config.stack_size, config.max_open_sockets);
    ESP_LOGI(TAG, "heap: %d bytes free, largest block %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return ESP_OK;
}

void webserver_stop(void)
{
    if (s_server == NULL) {
        return;
    }
    /* httpd_stop() waits for the task to end, so the port is really free when it
     * returns - the configuration server binds it right afterwards */
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "readings endpoint stopped");
}