/*
 * WLAN station and the configuration access point.
 *
 * The device logs into the network from NVS (see netcfg.h); connect.h only
 * holds the defaults for the first start.  wifi_init() brings the driver up in
 * station mode and is safe to call more than once.
 *
 * When the network cannot be reached, configmode.c opens an access point of its
 * own and uses wifi_scan_networks() to show what is around.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* One entry of a scan result */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured; /* needs a password */
} wifi_ap_info_t;

/* Signal strength value used when an SSID is not in the scan result */
#define WIFI_RSSI_NOT_FOUND (-127)

/* One access point from a scan, without merging equal names: a mesh shows up
 * as several entries with the same SSID and a different BSSID. */
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
} wifi_bss_t;

/** Bring up NVS-backed WiFi in station mode, register the handlers.  Idempotent. */
esp_err_t wifi_init(void);

/**
 * Log into `ssid`.
 *
 * Blocks until an IP address was obtained or the retries are used up.
 *
 * @param timeout_ms total time to wait for an address
 * @return true when the device has an IP address
 */
bool wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * Like wifi_connect(), but tied to one access point of a mesh.
 *
 * With `bssid` the driver is told which of several access points that share the
 * name to use - it does not always pick the strongest by itself.  When that
 * access point stops answering, the pin is dropped and the driver chooses
 * again.  `bssid` NULL behaves like wifi_connect().
 */
bool wifi_connect_bssid(const char *ssid, const char *password, const uint8_t *bssid,
                        uint32_t timeout_ms);

/**
 * Scan and copy all access points into `out`, keeping several entries with the
 * same name (one per BSSID).  Returns the number of entries written.
 */
int wifi_scan_all(wifi_bss_t *out, int max);

/** Stop the automatic reconnect logic (used before the access point starts). */
void wifi_stop_sta(void);

/**
 * Start the configuration access point, in addition to the (idle) station
 * interface so that scanning keeps working.
 *
 * @param password NULL or shorter than 8 characters opens the access point
 * @param ip       receives the address of the access point, e.g. "192.168.4.1"
 */
esp_err_t wifi_start_ap(const char *ssid, const char *password, char *ip, size_t ip_size);

/**
 * Scan for networks and copy the strongest `max` into `out` (strongest first,
 * hidden and nameless entries skipped).  Returns the number of entries written.
 */
int wifi_scan_networks(wifi_ap_info_t *out, int max);

/** Signal strength of `ssid` in dBm, or WIFI_RSSI_NOT_FOUND. */
int wifi_scan_rssi(const char *ssid);

/** True while an IP address is assigned. */
bool wifi_is_connected(void);

/** Current IPv4 address as text, or "-" while not connected. */
const char *wifi_ip_address(void);

/** Signal strength of the current link in dBm, 0 while not connected. */
int wifi_rssi(void);

/**
 * Details of the current link, for the JSON endpoint and the log: name and
 * address of the access point the unit is associated with.  In a mesh several
 * access points share one name, so the BSSID tells which one is in use.
 *
 * @param ssid    receives the network name, may be NULL
 * @param bssid   receives "aa:bb:cc:dd:ee:ff", may be NULL
 * @param channel receives the channel, may be NULL
 * @return false while not connected
 */
bool wifi_link_info(char *ssid, size_t ssid_size, char *bssid, size_t bssid_size, int *channel);

/** Log access point name, channel, signal strength and IP address. */
void wifi_log_link(void);
