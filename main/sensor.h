/* SHT35 temperature / humidity sensor on the I2C bus (see board.h). */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float temperature_c; /* °C */
    float humidity_rh;   /* % relative humidity */
} sensor_reading_t;

/** Bring up the I2C bus and the sensor. */
esp_err_t sensor_init(void);

/** True once sensor_init() succeeded. */
bool sensor_is_available(void);

/**
 * Take one measurement.  Returns ESP_ERR_INVALID_STATE if not initialised.
 *
 * The reading is also kept as "the last one", so other tasks (the HTTP
 * endpoint) can pick it up without touching the I2C bus: see sensor_last().
 */
esp_err_t sensor_read(sensor_reading_t *reading);

/**
 * Copy the last successful reading and its age.  Safe to call from any task;
 * no bus access.
 *
 * @param age_ms receives the milliseconds since that reading
 * @return false when no reading was taken yet
 */
bool sensor_last(sensor_reading_t *reading, int64_t *age_ms);