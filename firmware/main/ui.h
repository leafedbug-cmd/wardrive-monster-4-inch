/*
 * ui.h -- the five-screen dashboard.
 *
 *   0  COMBINED  totals across every protocol, plus link and card health
 *   1  WIFI      count, rate, channel occupancy, strongest APs
 *   2  BLE       count, rate, strongest advertisers
 *   3  MATTER    commissionable vs operational nodes
 *   4  ZIGBEE    why there is no data, and what would change that
 *
 * Swipe left/right to move between screens, or let it auto-cycle.
 * Call ui_init() only after display_init() has returned.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_COMBINED = 0,
    UI_SCREEN_WIFI,
    UI_SCREEN_BLE,
    UI_SCREEN_MATTER,
    UI_SCREEN_ZIGBEE,
    UI_SCREEN_COUNT
} ui_screen_t;

esp_err_t ui_init(void);

/* Jump to a screen programmatically. */
void ui_show(ui_screen_t screen);

/* Auto-advance through the screens every `seconds`. 0 disables.
 * Any touch postpones the next advance. */
void ui_set_autocycle(uint16_t seconds);

#ifdef __cplusplus
}
#endif
