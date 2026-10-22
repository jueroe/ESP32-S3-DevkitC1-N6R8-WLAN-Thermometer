/*
 * SHT35 (Sensirion) temperature / humidity sensor.
 *
 * The sensor is driven directly through the I2C master driver, so no external
 * component is needed:
 *
 *   command 0x2C06  single shot, high repeatability, clock stretching disabled
 *   answer  6 bytes: T MSB, T LSB, T CRC, RH MSB, RH LSB, RH CRC
 *
 *   T  [degC] = -45 + 175 * raw / 65535
 *   RH [%]    = 100 * raw / 65535
 *
 * Each pair of data bytes carries a CRC-8 (polynomial 0x31, init 0xFF), so a
 * disturbed transfer is detected instead of being turned into a wrong value.
 */

#include "sensor.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board.h"

static const char *TAG = "sensor";

#define SHT35_CMD_SINGLE_SHOT_HIGH 0x2C06
#define SHT35_MEASURE_MS           20 /* datasheet: 15 ms max at high repeatability */
#define SHT35_TIMEOUT_MS           100

#define SHT35_CRC_POLYNOMIAL       0x31
#define SHT35_CRC_INIT             0xFF

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_sensor;
static bool s_available;

/* Last successful reading, so the HTTP endpoint can answer without touching
 * the I2C bus (the sensor task reads once every few seconds). */
static sensor_reading_t s_last;
static int64_t s_last_us;
static bool s_last_valid;
static SemaphoreHandle_t s_last_lock;

static uint8_t sht35_crc(const uint8_t *data, size_t length)
{
    uint8_t crc = SHT35_CRC_INIT;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ SHT35_CRC_POLYNOMIAL) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* List the addresses that answer on the bus, to help with wiring problems. */
static void log_bus_scan(void)
{
    char found[96] = { 0 };
    size_t used = 0;
    int count = 0;
    bool sensor_seen = false;

    for (uint8_t address = 0x08; address <= 0x77; address++) {
        if (i2c_master_probe(s_bus, address, 50) != ESP_OK) {
            continue;
        }
        count++;
        if (address == BOARD_SHT35_ADDRESS) {
            sensor_seen = true;
        }
        if (used + 7 < sizeof(found)) {
            const int written = snprintf(found + used, sizeof(found) - used, "%s0x%02X",
                                         used > 0 ? " " : "", address);
            if (written > 0) {
                used += (size_t)written;
            }
        }
    }

    if (count == 0) {
        ESP_LOGE(TAG, "no I2C device answers - check 3.3 V, GND, SDA=GPIO%d, SCL=GPIO%d",
                 BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    } else if (!sensor_seen) {
        ESP_LOGW(TAG, "I2C devices found: %s, but nothing at 0x%02X - check the ADDR pin "
                      "and BOARD_SHT35_ADDRESS in board.h", found, BOARD_SHT35_ADDRESS);
    } else {
        ESP_LOGW(TAG, "I2C devices found: %s", found);
    }
}

esp_err_t sensor_init(void)
{
    if (s_last_lock == NULL) {
        s_last_lock = xSemaphoreCreateMutex();
        if (s_last_lock == NULL) {
            ESP_LOGE(TAG, "no memory for the reading lock");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_sensor != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* the breakout has 10k pull-ups, this is a fallback */
    };

    ESP_LOGI(TAG, "I2C%d for SHT35: SDA=GPIO%d SCL=GPIO%d, %d kHz, address 0x%02X", BOARD_I2C_PORT,
             BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_I2C_FREQ_HZ / 1000,
             BOARD_SHT35_ADDRESS);

    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_SHT35_ADDRESS,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    s_available = true;

    /* A measurement also proves that the sensor really answers; retry a few
     * times because the first one after power-up can be disturbed. */
    sensor_reading_t probe;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (sensor_read(&probe) == ESP_OK) {
            ESP_LOGI(TAG, "SHT35 ready, first reading: %.2f C, %.1f %%rH", probe.temperature_c,
                     probe.humidity_rh);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGE(TAG, "no SHT35 at 0x%02X - check SDA/SCL, power and the ADDR pin",
             BOARD_SHT35_ADDRESS);
    log_bus_scan();
    s_available = false;
    return ESP_FAIL;
}

bool sensor_is_available(void)
{
    return s_available;
}

esp_err_t sensor_read(sensor_reading_t *reading)
{
    if (!s_available || reading == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t command[2] = {
        (uint8_t)(SHT35_CMD_SINGLE_SHOT_HIGH >> 8),
        (uint8_t)(SHT35_CMD_SINGLE_SHOT_HIGH & 0xFF),
    };
    esp_err_t err = i2c_master_transmit(s_sensor, command, sizeof(command), SHT35_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SHT35_MEASURE_MS));

    uint8_t raw[6];
    err = i2c_master_receive(s_sensor, raw, sizeof(raw), SHT35_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reading failed: %s", esp_err_to_name(err));
        return err;
    }

    if (sht35_crc(&raw[0], 2) != raw[2] || sht35_crc(&raw[3], 2) != raw[5]) {
        ESP_LOGW(TAG, "checksum mismatch, measurement discarded");
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature = (uint16_t)((raw[0] << 8) | raw[1]);
    const uint16_t raw_humidity = (uint16_t)((raw[3] << 8) | raw[4]);

    reading->temperature_c = -45.0f + 175.0f * (float)raw_temperature / 65535.0f;
    reading->humidity_rh = 100.0f * (float)raw_humidity / 65535.0f;
    if (reading->humidity_rh < 0.0f) {
        reading->humidity_rh = 0.0f;
    } else if (reading->humidity_rh > 100.0f) {
        reading->humidity_rh = 100.0f;
    }

    if (s_last_lock != NULL) {
        xSemaphoreTake(s_last_lock, portMAX_DELAY);
        s_last = *reading;
        s_last_us = esp_timer_get_time();
        s_last_valid = true;
        xSemaphoreGive(s_last_lock);
    }
    return ESP_OK;
}

bool sensor_last(sensor_reading_t *reading, int64_t *age_ms)
{
    if (s_last_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_last_lock, portMAX_DELAY);
    const bool valid = s_last_valid;
    if (valid) {
        if (reading != NULL) {
            *reading = s_last;
        }
        if (age_ms != NULL) {
            *age_ms = (esp_timer_get_time() - s_last_us) / 1000;
        }
    }
    xSemaphoreGive(s_last_lock);
    return valid;
}