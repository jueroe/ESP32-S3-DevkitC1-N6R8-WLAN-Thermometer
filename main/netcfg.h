/*
 * Network and device settings that survive a reboot.
 *
 * The compile time values in connect.h are only the defaults: as soon as the
 * configuration page stored something in NVS, that wins.  So a unit can be
 * moved to another network without a rebuild, and connect.h is only needed for
 * the very first start.
 *
 * NVS namespace "net", keys "ssid", "pass" and "name".
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define NETCFG_SSID_MAX      32
#define NETCFG_PASSWORD_MAX  64
#define NETCFG_NAME_MAX      32

typedef struct {
    char ssid[NETCFG_SSID_MAX + 1];
    char password[NETCFG_PASSWORD_MAX + 1];
    char name[NETCFG_NAME_MAX + 1]; /* shown in the browser, router and log */
} netcfg_t;

/**
 * Bring up the NVS partition.  Call once before anything else touches NVS;
 * repeated calls are free.
 */
esp_err_t netcfg_init(void);

/**
 * Fill `out` with the settings in use: the connect.h defaults first, then
 * whatever NVS holds.
 *
 * @return true when the values came from NVS (WLAN configured on the device)
 */
bool netcfg_load(netcfg_t *out);

/** Store the settings in NVS. */
esp_err_t netcfg_save(const netcfg_t *cfg);

/** Name of this unit for the browser, the router and the log ("Aussenthermometer"). */
const char *netcfg_device_name(void);

/**
 * Host name for DHCP/mDNS, derived from the device name: ASCII, lower case,
 * spaces and punctuation replaced by "-" ("Aussenthermometer Garten" ->
 * "aussenthermometer-garten").  Falls back to "thermometer".
 */
void netcfg_hostname(char *out, size_t size);