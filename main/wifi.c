#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "connect.h"
#include "netcfg.h"

static const char *TAG = "wifi";

#define WIFI_GOT_IP_BIT  BIT0
#define WIFI_FAILED_BIT  BIT1

/* Tries before startup gives up; once the link was up, reconnects never stop,
 * so a router reboot does not need a reboot of the thermometer */
#define WIFI_MAX_RETRIES 5

/* Networks copied out of a scan (strongest kept) */
#define WIFI_SCAN_MAX_APS 16

static EventGroupHandle_t s_events;
static bool s_initialized;
static bool s_auto_connect; /* only reconnect once wifi_connect() asked for it */
static bool s_connected;
static bool s_was_connected;
static int s_retries;
static char s_ip[16];

static void clear_ip(void)
{
    snprintf(s_ip, sizeof(s_ip), "%s", "-");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_auto_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        clear_ip();
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)data;

        if (!s_auto_connect) {
            return;
        }
        if (s_retries < WIFI_MAX_RETRIES || s_was_connected) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", event->reason, s_retries);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "cannot reach the network (reason %d, e.g. 201 = network not found, "
                          "15 = wrong password), continuing without network",
                     event->reason);
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_was_connected = true;
        s_retries = 0;
        ESP_LOGI(TAG, "got IP %s", s_ip);
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, WIFI_GOT_IP_BIT);
        }
    }
}

esp_err_t wifi_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    clear_ip();
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "no memory for the event group");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (sta == NULL) {
        ESP_LOGE(TAG, "cannot create the station interface");
        return ESP_FAIL;
    }

    /* the router lists the unit under this name, derived from the configured
     * device name ("Raumthermometer Küche" -> "raumthermometer-kueche") */
    char hostname[33];
    netcfg_hostname(hostname, sizeof(hostname));
    esp_netif_set_hostname(sta, hostname);

    const wifi_init_config_t init_configuration = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_configuration);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL,
                                             NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL,
                                                  NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot register the event handlers: %s", esp_err_to_name(err));
        return err;
    }

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "starting the driver failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

bool wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "no network name configured");
        return false;
    }
    if (wifi_init() != ESP_OK) {
        return false;
    }

    wifi_config_t wifi_configuration = {0};
    snprintf((char *)wifi_configuration.sta.ssid, sizeof(wifi_configuration.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_configuration.sta.password, sizeof(wifi_configuration.sta.password), "%s",
             password != NULL ? password : "");
    wifi_configuration.sta.threshold.authmode =
        wifi_configuration.sta.password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* the interface is already running: stop the old attempt, take the new
     * credentials and reconnect deliberately */
    s_auto_connect = false;
    esp_wifi_disconnect();
    s_connected = false;
    s_was_connected = false;
    s_retries = 0;
    clear_ip();
    xEventGroupClearBits(s_events, WIFI_GOT_IP_BIT | WIFI_FAILED_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    s_auto_connect = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_GOT_IP_BIT | WIFI_FAILED_BIT,
                                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_GOT_IP_BIT) != 0) {
        ESP_LOGI(TAG, "connected to \"%s\", IP %s", ssid, s_ip);
        return true;
    }
    if ((bits & WIFI_FAILED_BIT) == 0) {
        ESP_LOGW(TAG, "no IP address within %u ms", (unsigned)timeout_ms);
    }
    s_auto_connect = false;
    return false;
}

void wifi_stop_sta(void)
{
    s_auto_connect = false;
    if (s_initialized) {
        esp_wifi_disconnect();
    }
}

esp_err_t wifi_start_ap(const char *ssid, const char *password, char *ip, size_t ip_size)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > 32) {
        ESP_LOGE(TAG, "access point name \"%s\" is longer than 32 bytes", ssid);
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_init() != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_stop_sta();

    /* Deterministic restart: without esp_wifi_stop() a driver that already runs
     * in station mode may keep the access point down. */
    if (esp_wifi_stop() != ESP_OK) {
        ESP_LOGD(TAG, "the driver was not running");
    }

    /* The station interface stays enabled (mode APSTA) so that the scan works
     * while the access point is running; it has no credentials and therefore
     * never connects. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        return err;
    }

    static esp_netif_t *s_ap_netif;
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    esp_netif_t *ap = s_ap_netif;
    if (ap == NULL) {
        ESP_LOGE(TAG, "cannot create the access point interface");
        return ESP_FAIL;
    }

    wifi_config_t ap_configuration = {0};
    memcpy(ap_configuration.ap.ssid, ssid, strlen(ssid));
    ap_configuration.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_configuration.ap.channel = 1;
    ap_configuration.ap.max_connection = 2;
    if (password != NULL && strlen(password) >= 8) {
        snprintf((char *)ap_configuration.ap.password, sizeof(ap_configuration.ap.password), "%s",
                 password);
        ap_configuration.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_configuration.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_configuration);
    if (err == ESP_OK) {
        err = esp_wifi_start();
        if (err == ESP_ERR_WIFI_STATE) {
            err = ESP_OK; /* already running, the new AP configuration applies anyway */
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting the access point failed: %s", esp_err_to_name(err));
        return err;
    }

    /* the address is assigned right after the start, give it a moment */
    esp_netif_ip_info_t ip_info;
    esp_err_t ip_err = ESP_FAIL;
    for (int attempt = 0; attempt < 25; attempt++) {
        ip_err = esp_netif_get_ip_info(ap, &ip_info);
        if (ip_err == ESP_OK && ip_info.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (ip_err == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(ip, ip_size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(ip, ip_size, "%s", "192.168.4.1");
    }

    ESP_LOGI(TAG, "access point \"%s\" (%s) at http://%s", ssid,
             ap_configuration.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", ip);
    return ESP_OK;
}

int wifi_scan_networks(wifi_ap_info_t *out, int max)
{
    if (out == NULL || max <= 0 || !s_initialized) {
        return 0;
    }

    const wifi_scan_config_t scan_configuration = {
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_configuration, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK || found == 0) {
        esp_wifi_clear_ap_list();
        ESP_LOGW(TAG, "scan found no networks");
        return 0;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGE(TAG, "no memory for %u scan records (%u bytes)", (unsigned)found,
                 (unsigned)(found * sizeof(wifi_ap_record_t)));
        esp_wifi_clear_ap_list();
        return 0;
    }

    uint16_t count = found;
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        count = 0;
    }
    esp_wifi_clear_ap_list();

    int used = 0;
    for (uint16_t i = 0; i < count; i++) {
        const char *name = (const char *)records[i].ssid;
        if (name[0] == '\0') {
            continue;
        }

        /* keep the list sorted by signal strength, strongest first, so that a
         * crowded band still ends up with the strongest `max` entries */
        int slot = used < max ? used : max - 1;
        if (used == max && out[slot].rssi > records[i].rssi) {
            continue; /* weaker than everything already in the list */
        }
        while (slot > 0 && out[slot - 1].rssi < records[i].rssi) {
            if (slot < max) {
                out[slot] = out[slot - 1];
            }
            slot--;
        }
        snprintf(out[slot].ssid, sizeof(out[slot].ssid), "%s", name);
        out[slot].rssi = records[i].rssi;
        out[slot].secured = records[i].authmode != WIFI_AUTH_OPEN;
        if (used < max) {
            used++;
        }
    }

    free(records);

    /* One entry per name: a mesh, or the same router on 2.4 and 5 GHz, appears
     * twice in the scan result.  The list is sorted by strength, so the first
     * occurrence is the better one. */
    int unique = 0;
    for (int i = 0; i < used; i++) {
        bool duplicate = false;
        for (int j = 0; j < unique; j++) {
            if (strcmp(out[j].ssid, out[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            out[unique++] = out[i];
        }
    }
    used = unique;

    ESP_LOGI(TAG, "scan: %u networks, %d shown (record size %u bytes)", (unsigned)count, used,
             (unsigned)sizeof(wifi_ap_record_t));
    return used;
}

int wifi_scan_rssi(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return WIFI_RSSI_NOT_FOUND;
    }

    wifi_ap_info_t list[WIFI_SCAN_MAX_APS];
    const int count = wifi_scan_networks(list, WIFI_SCAN_MAX_APS);
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            return list[i].rssi;
        }
    }
    return WIFI_RSSI_NOT_FOUND;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

const char *wifi_ip_address(void)
{
    return s_ip;
}

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

void wifi_log_link(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGI(TAG, "not connected");
        return;
    }
    ESP_LOGI(TAG, "link to \"%s\": channel %d, RSSI %d dBm, IP %s", (const char *)ap.ssid,
             ap.primary, ap.rssi, s_ip);
}
