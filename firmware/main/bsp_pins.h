/*
 * bsp_pins.h -- every board-dependent number in this project.
 *
 * Target: Waveshare ESP32-P4-WIFI6-DEV-KIT (Basic Kit) + lcdwiki MSP4031
 *         4.0" 320x480 ST7796S SPI module with FT6336U capacitive touch.
 *
 * If your wiring differs from docs/WIRING.md, change it HERE and only here.
 *
 * ============================ DO NOT USE =================================
 *  40-pin header pin 32 == GPIO54 == ESP32-C6 ENABLE.
 *  Driving it resets the radio and Wi-Fi/BLE die with no obvious symptom.
 * =========================================================================
 */
#pragma once

#include "driver/gpio.h"

/* ------------------------------------------------------------------ *
 *  Display -- ST7796S over SPI2                                      *
 * ------------------------------------------------------------------ */

#define BSP_LCD_SPI_HOST        SPI2_HOST

#define BSP_LCD_PIN_SCLK        GPIO_NUM_5    /* hdr 16 -> disp 7  SCK      */
#define BSP_LCD_PIN_MOSI        GPIO_NUM_6    /* hdr 15 -> disp 6  SDI      */
#define BSP_LCD_PIN_MISO        GPIO_NUM_4    /* hdr 18 -> disp 9  SDO      */
#define BSP_LCD_PIN_CS          GPIO_NUM_21   /* hdr 11 -> disp 3  LCD_CS   */
#define BSP_LCD_PIN_DC          GPIO_NUM_22   /* hdr 12 -> disp 5  LCD_RS   */
#define BSP_LCD_PIN_RST         GPIO_NUM_20   /* hdr 13 -> disp 4  LCD_RST  */
#define BSP_LCD_PIN_BL          GPIO_NUM_3    /* hdr 19 -> disp 8  LED      */

/* The panel is 320(W) x 480(H) native, portrait. The dashboard runs
 * landscape, so the driver swaps XY and the UI works in 480x320. */
#define BSP_LCD_H_RES_NATIVE    320
#define BSP_LCD_V_RES_NATIVE    480
#define BSP_LCD_H_RES           480
#define BSP_LCD_V_RES           320

#define BSP_LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define BSP_LCD_CMD_BITS        8
#define BSP_LCD_PARAM_BITS      8
#define BSP_LCD_BITS_PER_PIXEL  16          /* RGB565 */
#define BSP_LCD_BL_ON_LEVEL     1           /* LED pin is active high */

/* Backlight PWM */
#define BSP_LCD_BL_LEDC_TIMER   LEDC_TIMER_0
#define BSP_LCD_BL_LEDC_CHAN    LEDC_CHANNEL_0
#define BSP_LCD_BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BSP_LCD_BL_DUTY_RES     LEDC_TIMER_10_BIT
#define BSP_LCD_BL_FREQ_HZ      5000
#define BSP_LCD_BL_DEFAULT_PCT  85

/* ------------------------------------------------------------------ *
 *  Touch -- FT6336U on the board's existing I2C bus                   *
 *  Shares the bus with the ES8311 codec (0x18); FT6336U is at 0x38.   *
 * ------------------------------------------------------------------ */

#define BSP_I2C_PORT            I2C_NUM_0
#define BSP_I2C_PIN_SDA         GPIO_NUM_7    /* hdr 3 -> disp 12 CTP_SDA */
#define BSP_I2C_PIN_SCL         GPIO_NUM_8    /* hdr 5 -> disp 10 CTP_SCL */
#define BSP_I2C_FREQ_HZ         400000

#define BSP_TOUCH_PIN_RST       GPIO_NUM_2    /* hdr 21 -> disp 11 CTP_RST */
#define BSP_TOUCH_PIN_INT       GPIO_NUM_1    /* hdr 22 -> disp 13 CTP_INT */
#define BSP_TOUCH_I2C_ADDR      0x38

/* ------------------------------------------------------------------ *
 *  microSD -- the P4's own TF slot, 4-bit SDIO 3.0.                   *
 *                                                                     *
 *  These are fixed by the board, NOT by your wiring. The display also *
 *  has an SD slot; we deliberately do not use it because it shares    *
 *  the LCD SPI bus. See docs/WIRING.md.                               *
 * ------------------------------------------------------------------ */

/* The ESP32-P4 has two SDMMC slots and BOTH this card and the C6 radio hang
 * off that one peripheral, so they must be on different slots.
 *
 * The split is dictated by the silicon, not by preference: slot 0's IOMUX
 * pins are 43/44/39-42, which is exactly where this board wired its TF card,
 * and the C6 sits on 18/19/14-17, which belong to slot 1. So:
 *
 *      card  -> slot 0   (here)
 *      C6    -> slot 1   (CONFIG_ESP_HOSTED_SDIO_SLOT_1)
 *
 * Getting this backwards makes esp_hosted drive the SD card's pins looking
 * for a radio, and the C6 never answers. */
#define BSP_SD_SLOT             SDMMC_HOST_SLOT_0

/* Slot 0's card IO rail is fed from an on-chip LDO on the P4
 * (SOC_SDMMC_IO_POWER_EXTERNAL). Without configuring it the card never
 * powers up and mounting fails with ESP_ERR_INVALID_RESPONSE. */
#define BSP_SD_LDO_CHAN         4

#define BSP_SD_PIN_CLK          GPIO_NUM_43
#define BSP_SD_PIN_CMD          GPIO_NUM_44
#define BSP_SD_PIN_D0           GPIO_NUM_39
#define BSP_SD_PIN_D1           GPIO_NUM_40
#define BSP_SD_PIN_D2           GPIO_NUM_41
#define BSP_SD_PIN_D3           GPIO_NUM_42
#define BSP_SD_BUS_WIDTH        4
#define BSP_SD_MOUNT_POINT      "/sdcard"
#define BSP_SD_MAX_FREQ_KHZ     40000

/* ------------------------------------------------------------------ *
 *  ESP32-C6 radio co-processor (ESP-Hosted over SDIO).                *
 *                                                                     *
 *  Listed for documentation only -- these are the ESP-Hosted defaults *
 *  for the ESP32-P4 and are configured through Kconfig, not here.     *
 *                                                                     *
 *      CLK GPIO18   CMD GPIO19   D0 GPIO14                            *
 *      D1  GPIO15   D2  GPIO16   D3 GPIO17                            *
 *      C6 reset/EN  GPIO54  <-- also on header pin 32. DO NOT USE.    *
 * ------------------------------------------------------------------ */

#define BSP_C6_RESET_GPIO       54    /* informational; owned by esp_hosted */

/* The C6 slave image is read from the SD card and pushed over SDIO. */
#define BSP_C6_FIRMWARE_PATH    BSP_SD_MOUNT_POINT "/c6_slave.bin"
