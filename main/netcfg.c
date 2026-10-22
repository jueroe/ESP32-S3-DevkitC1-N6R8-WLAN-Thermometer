#include "netcfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "connect.h"

static const char *TAG = "netcfg";

#define NETCFG_NAMESPACE "net"
#define NETCFG_KEY_SSID  "ssid"
#define NETCFG_KEY_PASS  "pass"
#define NETCFG_KEY_NAME  "name"

/* Long enough for the longest value (password plus NUL) */
#define NETCFG_VALUE_MAX 80

static bool s_ready;
static bool s_loaded;
static bool s_from_nvs;
static netcfg_t s_cfg;

esp_err_t netcfg_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS had to be erased (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    return ESP_OK;
}

static void set_defaults(netcfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", CONNECT_WIFI_SSID);
    snprintf(cfg->password, sizeof(cfg->password), "%s", CONNECT_WIFI_PASSWORD);
    snprintf(cfg->name, sizeof(cfg->name), "%s", CONNECT_DEVICE_NAME);
}

/* Copy `source` into `target`, cutting it when the target is smaller. */
static void copy_string(char *target, size_t size, const char *source)
{
    if (size == 0) {
        return;
    }
    size_t length = strlen(source);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(target, source, length);
    target[length] = '\0';
}

/* Read one string key into `out`.  A missing or empty key keeps the default. */
static void read_string(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    char value[NETCFG_VALUE_MAX];
    size_t length = sizeof(value);
    if (nvs_get_str(handle, key, value, &length) == ESP_OK && value[0] != '\0') {
        copy_string(out, size, value);
    }
}

static void load_once(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    set_defaults(&s_cfg);

    nvs_handle_t handle;
    if (netcfg_init() != ESP_OK || nvs_open(NETCFG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    char ssid[NETCFG_VALUE_MAX];
    size_t length = sizeof(ssid);
    if (nvs_get_str(handle, NETCFG_KEY_SSID, ssid, &length) == ESP_OK && ssid[0] != '\0') {
        copy_string(s_cfg.ssid, sizeof(s_cfg.ssid), ssid);
        read_string(handle, NETCFG_KEY_PASS, s_cfg.password, sizeof(s_cfg.password));
        s_from_nvs = true;
    }

    /* the name can be set on its own, without touching the network data */
    read_string(handle, NETCFG_KEY_NAME, s_cfg.name, sizeof(s_cfg.name));
    nvs_close(handle);
}

bool netcfg_load(netcfg_t *out)
{
    load_once();
    if (out != NULL) {
        *out = s_cfg;
    }
    return s_from_nvs;
}

esp_err_t netcfg_save(const netcfg_t *cfg)
{
    if (cfg == NULL || cfg->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (netcfg_init() != ESP_OK) {
        return ESP_FAIL;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NETCFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NETCFG_KEY_SSID, cfg->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NETCFG_KEY_PASS, cfg->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NETCFG_KEY_NAME, cfg->name);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing the settings failed: %s", esp_err_to_name(err));
        return err;
    }

    s_cfg = *cfg;
    s_from_nvs = true;
    s_loaded = true;
    ESP_LOGI(TAG, "stored \"%s\": WLAN \"%s\"", s_cfg.name, s_cfg.ssid);
    return ESP_OK;
}

const char *netcfg_device_name(void)
{
    load_once();
    return s_cfg.name;
}

/* One byte of a host name: lower case letters and digits stay, everything else
 * becomes a separating "-" (never two in a row, never at the ends). */
static void add_host_byte(char *out, size_t size, size_t *used, char byte)
{
    char mapped;
    if (byte >= 'A' && byte <= 'Z') {
        mapped = (char)(byte - 'A' + 'a');
    } else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
        mapped = byte;
    } else {
        mapped = '-';
    }

    if (mapped == '-' && (*used == 0 || out[*used - 1] == '-')) {
        return;
    }
    if (*used + 1 < size) {
        out[(*used)++] = mapped;
        out[*used] = '\0';
    }
}

void netcfg_hostname(char *out, size_t size)
{
    load_once();
    if (out == NULL || size == 0) {
        return;
    }
    out[0] = '\0';

    size_t used = 0;
    const unsigned char *cursor = (const unsigned char *)s_cfg.name;
    for (size_t i = 0; cursor[i] != '\0'; i++) {
        const unsigned char byte = cursor[i];

        /* German umlauts are two bytes in UTF-8; spell them out */
        if (byte == 0xC3 && cursor[i + 1] != '\0') {
            const char *replacement = NULL;
            switch (cursor[i + 1]) {
            case 0xA4:
            case 0x84: replacement = "ae"; break; /* ae / Ae */
            case 0xB6:
            case 0x96: replacement = "oe"; break; /* oe / Oe */
            case 0xBC:
            case 0x9C: replacement = "ue"; break; /* ue / Ue */
            case 0x9F: replacement = "ss"; break; /* ss */
            default: break;
            }
            i++;
            if (replacement != NULL) {
                for (const char *r = replacement; *r != '\0'; r++) {
                    add_host_byte(out, size, &used, *r);
                }
            } else {
                add_host_byte(out, size, &used, '-');
            }
            continue;
        }
        if (byte >= 0x80) {
            add_host_byte(out, size, &used, '-');
            continue;
        }
        add_host_byte(out, size, &used, (char)byte);
    }

    while (used > 0 && out[used - 1] == '-') {
        out[--used] = '\0';
    }
    if (used == 0) {
        snprintf(out, size, "%s", "thermometer");
    }
}