/*
 * Status display on the on-board RGB LED (WS2812 on the DevKitC-1).
 *
 *   green  WLAN:      connecting / no network -> 125 ms on, 125 ms off (4 Hz)
 *                     configuration access point -> 2 blinks, then 1000 ms off
 *                     logged in -> steady on
 *   red    sensor:    on when the SHT35 did not come up at start, else off
 *   blue   signal:    3 blinks >= -50 dBm, 2 blinks >= -65, 1 blink >= -80,
 *                     off below -80 dBm or while not connected
 *
 * The blink times are the same for all patterns: 125 ms on, 125 ms off.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    STATUSLED_WIFI_CONNECTING = 0, /* not connected yet, or no network reachable */
    STATUSLED_WIFI_AP,             /* configuration access point is running */
    STATUSLED_WIFI_CONNECTED,      /* logged into the home network */
} statusled_wifi_t;

/** Create the LED driver and start the pattern task. */
esp_err_t statusled_init(void);

/** Which WLAN state the green LED shows. */
void statusled_set_wifi(statusled_wifi_t state);

/** false turns the red LED on (sensor missing or not answering). */
void statusled_set_sensor_ok(bool ok);
