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
/* 50 kHz instead of the 100 kHz the SHT35 could do: the sensor sits outside on
 * a 1.5 m cable, which raises the bus capacitance.  With 4k7 pull-ups that
 * leaves the rise time at about 0.7 us - safely below the 1 us limit.  A
 * transfer of six bytes takes well under a millisecond either way. */
#define BOARD_I2C_FREQ_HZ       50000

/* 0x44 with ADDR tied to GND (default on most breakouts), 0x45 with ADDR to VDD */
#define BOARD_SHT35_ADDRESS     0x44

/* The on-board BOOT button (GPIO0, download strap).  Holding it for 3 s while
 * the unit runs opens the configuration access point; holding it during reset
 * enters the ROM download mode instead, so that path must not be used. */
#define BOARD_BOOT_BUTTON_GPIO  0

/* On-board addressable RGB LED (WS2812).  The DevKitC-1 exists in two
 * revisions: v1.1 drives it on GPIO38, the initial version on GPIO48.  Both
 * pins are driven, so the firmware fits either board; the pin that has no LED
 * simply does nothing.  Set one of them to 0 to drive a single pin. */
#define BOARD_RGB_LED_GPIO      38
#define BOARD_RGB_LED_GPIO_ALT  48

/* Duty cycle per colour channel (0..255).  Keep it low: a single WS2812 at
 * full brightness is unpleasant in a dark room.  Green is set lower because
 * the eye is much more sensitive to it than to red or blue. */
#define BOARD_RGB_LED_BRIGHTNESS       64
#define BOARD_RGB_LED_BRIGHTNESS_GREEN 24
