/*
 * display.h -- ST7796S panel + FT6336U touch + LVGL, on the 40-pin header.
 *
 * The Basic Kit has no DSI panel, so the 4.0" module hangs off SPI2 and the
 * board's existing I2C bus. All pin numbers come from bsp_pins.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up SPI, the panel, the touch controller and LVGL. After this
 * returns, take display_lock() before touching any lv_* object. */
esp_err_t display_init(void);

/* LVGL is not thread-safe. Wrap every UI mutation.
 * timeout_ms of 0 waits forever. */
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

/* Backlight, 0..100. Driven by LEDC on BSP_LCD_PIN_BL. */
void display_set_brightness(uint8_t percent);
uint8_t display_get_brightness(void);

#ifdef __cplusplus
}
#endif
