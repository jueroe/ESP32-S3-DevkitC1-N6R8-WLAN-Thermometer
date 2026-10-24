/*
 * Template for main/connect.h - copy it and fill in your own values:
 *
 *     Copy-Item main\connect.example.h main\connect.h
 *
 * main/connect.h is listed in .gitignore, so the WiFi password never lands in
 * the repository.  Without the copy the build stops with
 * "connect.h: No such file or directory".
 */

#pragma once

#define CONNECT_WIFI_SSID       "your-network-name"
#define CONNECT_WIFI_PASSWORD   "your-password"
#define CONNECT_DEVICE_NAME     "Außenthermometer"
#define CONNECT_WIFI_TIMEOUT_MS 20000

/* Weakest signal accepted for the stored network.  Only used for a warning in
 * the log: the unit always tries to connect, so that a device mounted at the
 * edge of the range does not stay in the configuration mode. */
#define CONNECT_MIN_RSSI        (-85)

/* Access point of the unit for the WLAN configuration (password needs at
 * least 8 characters, "" makes it open) */
#define CONNECT_AP_SSID         "thermometer"
#define CONNECT_AP_PASSWORD     "thermometer"

#define CONNECT_SNTP_SERVER     "de.pool.ntp.org"
#define CONNECT_SNTP_WAIT_MS    15000
#define CONNECT_TZ              "CET-1CEST,M3.5.0,M10.5.0/3"
#define CONNECT_TZ_NAME         "Europe/Berlin"