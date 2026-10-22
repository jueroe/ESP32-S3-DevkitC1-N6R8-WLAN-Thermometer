/*
 * Permanent HTTP endpoint with the current readings.
 *
 * Runs while the unit is logged into the home network - the configuration
 * mode (configmode.c) is a separate server on the unit's own access point and
 * never runs at the same time.
 *
 *   GET /api/values   JSON, for other devices and scripts
 *   GET /api          same as /api/values
 *   GET /              small page for a browser, refreshes every 10 s
 *
 * The JSON is built by hand (no cJSON in ESP-IDF 6.1) and uses a decimal point,
 * the HTML page a decimal comma:
 *
 * {
 *   "name": "Außenthermometer",
 *   "valid": true,                 false = no sensor reading yet
 *   "temperature_c": 23.6,
 *   "humidity_rh": 47.0,
 *   "age_s": 3,                    seconds since the reading was taken
 *   "time": "21.10. 16:52",        local time, fallback "01.01. 00:00"
 *   "ip": "192.168.123.122",
 *   "rssi_dbm": -42,
 *   "uptime_s": 1234
 * }
 *
 * The values come from the cache in sensor.c, so a request never blocks on the
 * I2C bus and never delays the next measurement.
 */

#pragma once

#include "esp_err.h"

/** Start the server (port 80).  Needs an IP address, so call it after wifi_connect(). */
esp_err_t webserver_start(void);

/**
 * Stop the server again.  The configuration mode runs its own server on port 80
 * (through the device's access point), and two listeners on the same port would
 * answer each other's requests.
 */
void webserver_stop(void);
