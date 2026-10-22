/*
 * Date and time: network time (NTP) in the local time zone.
 *
 * The module is called "clock" and not "time", because a project header named
 * time.h would shadow the C library header <time.h> - the compiler then no
 * longer knows time_t, struct tm, time() or localtime_r().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/** Apply the time zone from connect.h.  Works without a network. */
void clock_init(void);

/** Start the NTP client.  Needs an IP address, so call it after wifi_connect(). */
void clock_start_sntp(void);

/** Wait until NTP delivered the time.  Returns false on timeout. */
bool clock_wait_for_sync(uint32_t timeout_ms);

/** True once the system clock holds a plausible date (year >= 2024). */
bool clock_is_valid(void);

/**
 * Current local time.
 *
 * @param out receives the broken down time (may be NULL)
 * @return true when the clock is synchronised, false when `out` holds the
 *         1970/2000 default instead
 */
bool clock_local_now(struct tm *out);

/** Format "dd.mm.yyyy hh:mm:ss" in `buffer` and return `buffer`. */
const char *clock_format(char *buffer, size_t size);
