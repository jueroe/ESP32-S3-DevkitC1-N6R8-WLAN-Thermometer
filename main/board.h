/*
 * Wiring of this unit: ESP32-S3-DevKitC-1 N16R8 with an SHT35 breakout.
 *
 * SDA/SCL are GPIO4 and GPIO5: both sit on the left header (J1) that also
 * carries 3V3 (top) and GND (bottom), are plain GPIOs without a strapping,
 * USB or JTAG function, and are not used by flash/octal PSRAM (26..37).
 *
 * Not suitable: GPIO0 (BOOT), 3 and 46 (strapping), 19/20 (native USB),
 * 43/44 (UART0 console), 26..32 (flash), 33..37 (PSRAM).
 *
 * Change the pin numbers here if the sensor is wired differently.
 */

#pragma once

/* I2C bus to the SHT35 */
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA_GPIO      4
#define BOARD_I2C_SCL_GPIO      5
#define BOARD_I2C_FREQ_HZ       100000 /* SHT35 allows up to 1 MHz */

/* 0x44 with ADDR tied to GND (default on most breakouts), 0x45 with ADDR to VDD */
#define BOARD_SHT35_ADDRESS     0x44

/* The on-board BOOT button (GPIO0, download strap).  Holding it for 3 s while
 * the unit runs opens the configuration access point; holding it during reset
 * enters the ROM download mode instead, so that path must not be used. */
#define BOARD_BOOT_BUTTON_GPIO  0
