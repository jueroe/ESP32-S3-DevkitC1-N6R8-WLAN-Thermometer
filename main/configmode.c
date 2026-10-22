#include "configmode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "connect.h"
#include "netcfg.h"
#include "textutil.h"
#include "webserver.h"
#include "wifi.h"

static const char *TAG = "configmode";

/* Largest scan list shown in the form */
#define CONFIG_SCAN_MAX 12

/* Big enough for the page incl. the scan list */
#define CONFIG_PAGE_MAX 4096

/* Longest accepted POST body (ssid + password + name, percent-encoded) */
#define CONFIG_BODY_MAX 512

/* The scan runs in the background: it takes a few seconds while the access
 * point is up, and the page must answer immediately (phone browsers give up
 * quickly and fall back to mobile data when the Wi-Fi has no internet). */
#define CONFIG_SCAN_INTERVAL_MS 20000

static httpd_handle_t s_server;

static SemaphoreHandle_t s_scan_lock;
static wifi_ap_info_t s_networks[CONFIG_SCAN_MAX];
static int s_network_count;

static const char *CONFIG_PAGE_HEAD =
    "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Thermometer einrichten</title><style>"
    "body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;margin:0;padding:20px;"
    "background:#101418;color:#e8eef5}"
    "h1{font-size:20px;font-weight:600;margin:0 0 20px;color:#e8eef5}"
    "label{display:block;font-size:14px;color:#9fb0c0;margin:18px 0 6px}"
    "input{width:100%;box-sizing:border-box;padding:11px 12px;font-size:16px;border-radius:8px;"
    "border:1px solid #2a3541;background:#1b232b;color:#e8eef5}"
    "button{width:100%;margin-top:24px;padding:13px;font-size:16px;font-weight:600;"
    "border:0;border-radius:8px;background:#2f7d4f;color:#fff}"
    ".hint{font-size:13px;color:#9fb0c0;line-height:1.5;margin:14px 0 0}"
    "code{background:#1b232b;padding:2px 6px;border-radius:5px}"
    "</style></head><body>";

/* Append to a buffer, never writing past `size`.  Returns the new length. */
static size_t append(char *buffer, size_t size, size_t used, const char *format, ...)
{
    if (used + 1 >= size) {
        return used;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer + used, size - used, format, args);
    va_end(args);
    if (written <= 0) {
        return used;
    }
    const size_t added = (size_t)written;
    return used + (added < size - used ? added : size - used - 1);
}

static int hex_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

/* Decode one value of an application/x-www-form-urlencoded body */
static void url_decode(const char *source, size_t length, char *target, size_t size)
{
    size_t out = 0;
    for (size_t i = 0; i < length && out + 1 < size; i++) {
        char byte = source[i];
        if (byte == '+') {
            byte = ' ';
        } else if (byte == '%' && i + 2 < length) {
            const int high = hex_value(source[i + 1]);
            const int low = hex_value(source[i + 2]);
            if (high >= 0 && low >= 0) {
                byte = (char)((high << 4) | low);
                i += 2;
            }
        }
        target[out++] = byte;
    }
    target[out] = '\0';
}

/* Value of `key` in an application/x-www-form-urlencoded body. */
static bool form_value(const char *body, const char *key, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';

    const size_t key_length = strlen(key);
    for (const char *cursor = body; cursor != NULL && *cursor != '\0';) {
        const char *separator = strchr(cursor, '&');
        const char *pair_end = separator != NULL ? separator : cursor + strlen(cursor);
        const size_t pair_length = (size_t)(pair_end - cursor);

        if (pair_length > key_length && cursor[key_length] == '=' &&
            strncmp(cursor, key, key_length) == 0) {
            url_decode(cursor + key_length + 1, pair_length - key_length - 1, out, size);
            return true;
        }
        cursor = separator != NULL ? separator + 1 : NULL;
    }
    return false;
}

static void build_ap_ssid(char *out, size_t size)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, size, "%s-%02x%02x", CONNECT_AP_SSID, mac[4], mac[5]);
}

/* Copy the scan list, so the page can be built without waiting for a scan. */
static int scan_snapshot(wifi_ap_info_t *out, int max)
{
    if (s_scan_lock == NULL || out == NULL || max <= 0) {
        return 0;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    int count = s_network_count < max ? s_network_count : max;
    memcpy(out, s_networks, (size_t)count * sizeof(out[0]));
    xSemaphoreGive(s_scan_lock);
    return count;
}

static void scan_task(void *argument)
{
    (void)argument;

    for (;;) {
        wifi_ap_info_t list[CONFIG_SCAN_MAX];
        const int count = wifi_scan_networks(list, CONFIG_SCAN_MAX);

        xSemaphoreTake(s_scan_lock, portMAX_DELAY);
        memcpy(s_networks, list, sizeof(s_networks));
        s_network_count = count;
        xSemaphoreGive(s_scan_lock);

        vTaskDelay(pdMS_TO_TICKS(CONFIG_SCAN_INTERVAL_MS));
    }
}

/*
 * Minimal DNS server: every name resolves to the access point.
 *
 * Phones look up the address of their connectivity check host (for Android
 * "connectivitycheck.gstatic.com") before they send the HTTP request.  Without
 * DNS that lookup fails, no request arrives, and the phone silently marks the
 * network as "no internet" instead of showing the sign-in page.  Answering all
 * names with 192.168.4.1 makes the check land on our server, whose redirect
 * (see handler_captive) then opens the form.
 */
/* Decode the question name (labels joined by dots) for the log. */
static void dns_log_question(const uint8_t *query, int length)
{
    char name[128];
    size_t used = 0;
    size_t pos = 12;

    while (pos < (size_t)length) {
        const uint8_t label = query[pos];
        if (label == 0 || (label & 0xC0) == 0xC0) {
            break;
        }
        if (used + label + 2 >= sizeof(name)) {
            break;
        }
        if (used > 0) {
            name[used++] = '.';
        }
        memcpy(&name[used], &query[pos + 1], label);
        used += label;
        pos += (size_t)label + 1;
    }
    name[used] = '\0';

    const uint16_t type =
        (pos + 2 < (size_t)length) ? (uint16_t)((query[pos + 1] << 8) | query[pos + 2]) : 0;
    ESP_LOGI(TAG, "DNS query: %s (type %u)", name[0] != '\0' ? name : "?", type);
}

static size_t dns_build_answer(const uint8_t *query, int length, uint8_t *answer, size_t size)
{
    if (length < 12 + 5 || size < 12 + 16) {
        return 0;
    }

    /* walk over the question name: labels, terminated by a zero byte or a
     * compression pointer */
    size_t pos = 12;
    while (pos < (size_t)length) {
        const uint8_t label = query[pos];
        if (label == 0) {
            pos++;
            break;
        }
        if ((label & 0xC0) == 0xC0) {
            pos += 2;
            break;
        }
        pos += (size_t)label + 1;
    }
    if (pos + 4 > (size_t)length) {
        return 0;
    }

    const uint16_t type = (uint16_t)((query[pos] << 8) | query[pos + 1]);
    const size_t question_end = pos + 4;
    if (question_end + 16 > size) {
        return 0;
    }

    memcpy(answer, query, question_end);
    answer[2] = (uint8_t)(0x80 | (query[2] & 0x01)); /* response, RD copied */
    answer[3] = 0x80;                                /* recursion available, no error */
    const bool with_address = type == 1;             /* A, not AAAA */
    answer[6] = 0;
    answer[7] = with_address ? 1 : 0;
    answer[8] = 0;
    answer[9] = 0;
    answer[10] = 0;
    answer[11] = 0;

    if (!with_address) {
        return question_end;
    }

    size_t out = question_end;
    answer[out++] = 0xC0; /* name: pointer to the question */
    answer[out++] = 0x0C;
    answer[out++] = 0x00; /* type A */
    answer[out++] = 0x01;
    answer[out++] = 0x00; /* class IN */
    answer[out++] = 0x01;
    answer[out++] = 0x00; /* TTL 60 s */
    answer[out++] = 0x00;
    answer[out++] = 0x00;
    answer[out++] = 60;
    answer[out++] = 0x00; /* 4 bytes of address */
    answer[out++] = 0x04;
    answer[out++] = 192;
    answer[out++] = 168;
    answer[out++] = 4;
    answer[out++] = 1;
    return out;
}

static void dns_task(void *argument)
{
    (void)argument;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "no socket for the DNS answerer");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_address = { 0 };
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(53);
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_address, sizeof(bind_address)) < 0) {
        ESP_LOGE(TAG, "cannot bind the DNS port");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS: every name answers with 192.168.4.1");

    uint8_t query[512];
    uint8_t answer[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        const int length = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from,
                                   &from_length);
        if (length < 12) {
            continue;
        }
        const size_t answer_length = dns_build_answer(query, length, answer, sizeof(answer));
        if (answer_length == 0) {
            continue;
        }
        dns_log_question(query, length);
        sendto(sock, answer, answer_length, 0, (struct sockaddr *)&from, from_length);
    }
}

static void ap_event_handler(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    (void)base;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x connected",
                 event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4],
                 event->mac[5]);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = (const wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x disconnected",
                 event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4],
                 event->mac[5]);
    }
}

/* Log method, host and agent, so a phone's connectivity check can be told
 * apart from a real browser. */
static void log_request(const char *kind, httpd_req_t *req)
{
    char host[64] = "-";
    char agent[96] = "-";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    httpd_req_get_hdr_value_str(req, "User-Agent", agent, sizeof(agent));
    ESP_LOGI(TAG, "%s %s (host %s, agent %s)", kind, req->uri, host, agent);
}

/* Small answer page, used for the result of the form */
static esp_err_t send_result(httpd_req_t *req, const char *status, const char *heading,
                             const char *text)
{
    char page[1400];
    snprintf(page, sizeof(page), "%s<h1>%s</h1><p class=\"hint\">%s</p></body></html>",
             CONFIG_PAGE_HEAD, heading, text);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_form(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET %s (browser)", req->uri);
    log_request("form", req);
    netcfg_t cfg;
    netcfg_load(&cfg);

    char *page = malloc(CONFIG_PAGE_MAX);
    if (page == NULL) {
        return send_result(req, "500 Internal Server Error", "Kein Speicher",
                           "Bitte die Seite neu laden.");
    }

    char name[NETCFG_NAME_MAX * 6];
    char ssid[NETCFG_SSID_MAX * 6];
    html_escape(cfg.name, name, sizeof(name));
    html_escape(cfg.ssid, ssid, sizeof(ssid));

    size_t used = 0;
    used = append(page, CONFIG_PAGE_MAX, used,
                  "%s"
                  "<h1>%s einrichten</h1>"
                  "<form method=\"post\" action=\"/save\">"
                  "<label for=\"ssid\">WLAN-Name (SSID)</label>"
                  "<input id=\"ssid\" name=\"ssid\" list=\"networks\" value=\"%s\" "
                  "maxlength=\"%d\" autocapitalize=\"none\" autocomplete=\"off\" required>"
                  "<datalist id=\"networks\">",
                  CONFIG_PAGE_HEAD, name, ssid, NETCFG_SSID_MAX);

    wifi_ap_info_t networks[CONFIG_SCAN_MAX];
    const int count = scan_snapshot(networks, CONFIG_SCAN_MAX);
    for (int i = 0; i < count; i++) {
        char option[33 * 6];
        html_escape(networks[i].ssid, option, sizeof(option));
        used = append(page, CONFIG_PAGE_MAX, used, "<option value=\"%s\">%d dBm%s</option>",
                      option, networks[i].rssi, networks[i].secured ? ", gesichert" : ", offen");
    }

    used = append(page, CONFIG_PAGE_MAX, used,
                  "</datalist>"
                  "<label for=\"password\">WLAN-Passwort</label>"
                  "<input id=\"password\" name=\"password\" type=\"password\" maxlength=\"%d\" "
                  "placeholder=\"leer lassen = unveraendert\">"
                  "<label for=\"name\">Name des Ger&auml;ts</label>"
                  "<input id=\"name\" name=\"name\" value=\"%s\" maxlength=\"%d\">"
                  "<button type=\"submit\">Speichern und neu starten</button>"
                  "</form>"
                  "<p class=\"hint\">Das Ger&auml;t meldet sich nach dem Neustart im Heimnetz an "
                  "und ist dann unter <code>http://&lt;IP&gt;/</code> erreichbar.<br>"
                  "Diese Seite jederzeit wieder: <code>http://192.168.4.1/</code> im Browser "
                  "&ouml;ffnen (das WLAN zeigt dabei kein Internet an - das ist normal).<br>"
                  "Zum erneuten &Ouml;ffnen dieser Seite den <b>BOOT</b>-Taster im laufenden "
                  "Betrieb 3 Sekunden gedr&uuml;ckt halten.</p>"
                  "</body></html>",
                  NETCFG_PASSWORD_MAX, name, NETCFG_NAME_MAX);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, page, (ssize_t)used);
    free(page);
    return err;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    if (req->content_len >= CONFIG_BODY_MAX) {
        return send_result(req, "413 Payload Too Large", "Zu lang",
                           "Die Eingaben sind zu lang. Bitte die Seite neu laden.");
    }

    char body[CONFIG_BODY_MAX];
    size_t total = 0;
    while (total < req->content_len) {
        const int received = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return send_result(req, "400 Bad Request", "Ungueltige Anfrage",
                               "Bitte die Seite neu laden.");
        }
        total += (size_t)received;
    }
    body[total] = '\0';

    netcfg_t cfg;
    netcfg_load(&cfg);

    char ssid[NETCFG_SSID_MAX + 1];
    char password[NETCFG_PASSWORD_MAX + 1];
    char name[NETCFG_NAME_MAX + 1];

    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return send_result(req, "400 Bad Request", "WLAN-Name fehlt",
                           "Bitte einen WLAN-Namen eintragen.");
    }

    /* An empty password field with an unchanged network means "keep the stored
     * password"; with a new network it means "open network". */
    if (!form_value(body, "password", password, sizeof(password)) || password[0] == '\0') {
        snprintf(password, sizeof(password), "%s",
                 strcmp(ssid, cfg.ssid) == 0 ? cfg.password : "");
    }

    if (!form_value(body, "name", name, sizeof(name)) || name[0] == '\0') {
        snprintf(name, sizeof(name), "%s", cfg.name);
    }

    netcfg_t next = { 0 };
    snprintf(next.ssid, sizeof(next.ssid), "%s", ssid);
    snprintf(next.password, sizeof(next.password), "%s", password);
    snprintf(next.name, sizeof(next.name), "%s", name);

    const esp_err_t err = netcfg_save(&next);
    if (err != ESP_OK) {
        return send_result(req, "500 Internal Server Error", "Speichern fehlgeschlagen",
                           "Die Einstellungen konnten nicht gespeichert werden.");
    }

    ESP_LOGI(TAG, "settings stored, announcing the restart");
    const esp_err_t sent = send_result(
        req, "200 OK", "Gespeichert",
        "Das Ger&auml;t startet jetzt neu und meldet sich im WLAN an. "
        "Diese Seite kann geschlossen werden.");
    ESP_LOGI(TAG, "answer queued: %s", esp_err_to_name(sent));

    /* give the browser a moment to receive the answer */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return sent;
}

/* Answer the connectivity checks of the operating systems as "everything is
 * fine".  Otherwise the phone marks the network as "no internet", shows the
 * "!" and moves its traffic to mobile data, which makes the configuration page
 * unreachable.  With these answers the unit stays the active network and
 * http://192.168.4.1/ opens reliably. */
static esp_err_t handler_no_content(httpd_req_t *req)
{
    ESP_LOGI(TAG, "connectivity check %s -> 204", req->uri);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handler_apple_success(httpd_req_t *req)
{
    ESP_LOGI(TAG, "connectivity check %s -> Success", req->uri);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_ncsi(httpd_req_t *req)
{
    const bool ncsi = strcmp(req->uri, "/ncsi.txt") == 0;
    ESP_LOGI(TAG, "connectivity check %s -> text", req->uri);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, ncsi ? "Microsoft NCSI" : "Microsoft Connect Test",
                           HTTPD_RESP_USE_STRLEN);
}

/* Every other path leads to the form, so any address the user types ends up in
 * the configuration. */
static esp_err_t handler_captive(httpd_req_t *req)
{
    ESP_LOGD(TAG, "%s %s -> redirect", req->method == HTTP_POST ? "POST" : "GET", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "Konfiguration: http://192.168.4.1/", HTTPD_RESP_USE_STRLEN);
}

esp_err_t configmode_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    /* both servers listen on port 80 */
    webserver_stop();

    if (wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "the WiFi driver did not come up");
        return ESP_FAIL;
    }

    char ap_ssid[33];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));

    char ip[16];
    esp_err_t err = wifi_start_ap(ap_ssid, CONNECT_AP_PASSWORD, ip, sizeof(ip));
    if (err != ESP_OK) {
        return err;
    }

    /* Hand the access point itself out as DNS server, so phones resolve their
     * connectivity check host to us (see dns_task). */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL) {
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(ip);
        const esp_err_t dns_err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (dns_err != ESP_OK) {
            ESP_LOGW(TAG, "cannot offer %s as DNS: %s", ip, esp_err_to_name(dns_err));
        }
    }
    xTaskCreate(dns_task, "dns", 4096, NULL, 3, NULL);

    if (s_scan_lock == NULL) {
        s_scan_lock = xSemaphoreCreateMutex();
        if (s_scan_lock == NULL) {
            ESP_LOGE(TAG, "no memory for the scan lock");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event_handler,
                                        NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event_handler,
                                        NULL, NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    /* a phone opens several connections at once while probing for a portal */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    /* form, save, six connectivity checks and the wildcard redirect */
    config.max_uri_handlers = 12;
    /* required for the wildcard handlers below: without it the server only
     * compares strings and answers 404 to every unknown path */
    config.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_form, .user_ctx = NULL},
        {.uri = "/save", .method = HTTP_POST, .handler = handler_save, .user_ctx = NULL},
        /* connectivity checks: answer as if the internet worked */
        {.uri = "/generate_204", .method = HTTP_GET, .handler = handler_no_content, .user_ctx = NULL},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = handler_no_content, .user_ctx = NULL},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_apple_success,
         .user_ctx = NULL},
        {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = handler_apple_success,
         .user_ctx = NULL},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = handler_ncsi, .user_ctx = NULL},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = handler_ncsi, .user_ctx = NULL},
        /* everything else: redirect to the form */
        {.uri = "/*", .method = HTTP_GET, .handler = handler_captive, .user_ctx = NULL},
        {.uri = "/*", .method = HTTP_POST, .handler = handler_captive, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    /* the list is filled in the background; the first page shows what is there */
    xTaskCreate(scan_task, "scan", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "configuration access point \"%s\" (%s) - open http://%s/", ap_ssid,
             strlen(CONNECT_AP_PASSWORD) >= 8 ? "WPA2" : "open", ip);
    return ESP_OK;
}