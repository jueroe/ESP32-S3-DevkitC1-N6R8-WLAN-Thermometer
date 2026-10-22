/*
 * mDNS announcement: the unit answers as <hostname>.local.
 *
 * The DHCP host name (netcfg_hostname()) is only resolved when the router runs
 * a local DNS and the client uses its domain - "thermometer-kueche.fritz.box"
 * at a FRITZ!Box, something else (or nothing at all) on another router.  mDNS
 * makes the name work in any network:
 *
 *   http://aussenthermometer-garten.local/
 *   http://aussenthermometer-garten.local/api/values
 *
 * The host name follows the device name, the instance name is the device name
 * itself (so it may contain spaces and umlauts).  The HTTP service is announced
 * as "_http._tcp", which also lists the unit in network scanners and in the
 * "Network" view of Windows and macOS.
 *
 * The module is called "discovery" and not "mdns", because a main/mdns.h would
 * shadow the header of the mDNS component - the same trap as a main/time.h
 * (see clock.h).
 */

#pragma once

#include "esp_err.h"

/**
 * Start the responder and announce port 80.
 *
 * Needs a network connection, so call it after wifi_connect().
 */
esp_err_t discovery_start(void);

/** Stop the responder and free its memory (used before the access point starts). */
void discovery_stop(void);
