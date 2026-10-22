/*
 * Task watchdog (TWDT).
 *
 * The tasks that must keep running subscribe themselves and call
 * watchdog_feed() in their loop.  When one of them stops feeding - blocked in
 * a driver, a deadlock on a lock - the watchdog triggers a panic and the unit
 * restarts itself instead of staying silent with a stale value on the web page.
 *
 * The 60 s timeout is generous on purpose: the sensor task sleeps 10 s between
 * readings and the network scan can take a few seconds.
 */

#pragma once

/** Configure the watchdog and log the result.  Call once from app_main(). */
void watchdog_init(void);

/** Subscribe the calling task; it must call watchdog_feed() regularly. */
void watchdog_subscribe(void);

/** Unsubscribe the calling task (before it ends itself). */
void watchdog_unsubscribe(void);

/** Report that the calling task is still alive. */
void watchdog_feed(void);
