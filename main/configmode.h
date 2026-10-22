/*
 * Configuration mode: the unit opens its own access point and serves a small
 * form for network name, password and device name.
 *
 * It starts when the stored network cannot be reached, or when the BOOT button
 * is held for a few seconds while the unit runs (see main.c).  Holding BOOT
 * during reset does not work: GPIO0 is the download strap and the chip would
 * stay in the ROM download mode.  Saving the form stores the values in NVS and
 * restarts the unit, which then logs into the new network.
 *
 * The access point name is CONNECT_AP_SSID plus the last two MAC bytes
 * ("thermometer-1a2b"), the password is CONNECT_AP_PASSWORD (empty = open).
 */

#pragma once

#include "esp_err.h"

/**
 * Start the access point and the configuration page on port 80.
 *
 * Stops the readings endpoint first, because both listeners use port 80.
 */
esp_err_t configmode_start(void);
