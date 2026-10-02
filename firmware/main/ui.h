/*
 * ui.h -- Galaxy Hacker Touchscreen Interface & Phone-Style App Launcher.
 *
 * Apps:
 *   1  DASHBOARD        6-screen swiping telemetry (Combined, Wi-Fi, BLE, Matter, Zigbee, GPS)
 *   2  WARDRIVE         Survey / Session recorder with explicit Start/Stop & SD logging
 *   3  BLE TOOLKIT      Scanning, ChimeraBLE toolkit, Signal Finder, GATT Inspector
 *   4  WI-FI            2.4 GHz AP Scanner, Channel Histogram, Signal Monitor
 *   5  MATTER           Commissioning Survey (BLE Advertisements & mDNS)
 *   6  ZIGBEE           IEEE 802.15.4 / Zigbee Passive Sniffer & Channel Activity
 *
 * Navigation:
 *   Boot into stable Launcher (Home). Inside any app, a global [ < HOME ]
 *   button returns to the Launcher.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_VIEW_LAUNCHER = 0,    /* Galaxy Hacker 6-app home screen */
    UI_VIEW_DASHBOARD,       /* 6-screen swiping dashboard with scoped auto-cycle */
    UI_VIEW_WARDRIVE,        /* Survey / session interface with explicit start/stop */
    UI_VIEW_BLE,             /* BLE toolkit: scanning, Chimera, foxhunt, GATT inspect */
    UI_VIEW_WIFI,            /* Wi-Fi AP scan, channels histogram, signal monitor */
    UI_VIEW_MATTER,          /* Matter BLE commissioning & mDNS survey */
    UI_VIEW_ZIGBEE,          /* 802.15.4 / Zigbee sniffer observations */
    UI_VIEW_COUNT
} ui_view_t;

/* Sub-screens inside the Dashboard app */
typedef enum {
    UI_DASH_COMBINED = 0,
    UI_DASH_WIFI,
    UI_DASH_BLE,
    UI_DASH_MATTER,
    UI_DASH_ZIGBEE,
    UI_DASH_GPS,
    UI_DASH_COUNT
} ui_dash_screen_t;

/* Initialise UI and boot into the Launcher Home screen */
esp_err_t ui_init(void);

/* Switch current active view (e.g. from Launcher to App, or App back to Launcher) */
void ui_switch_view(ui_view_t view);

/* Navigate to specific dashboard screen when Dashboard app is open */
void ui_dashboard_show(ui_dash_screen_t screen);

/* Enable auto-advance within Dashboard app (seconds per screen; 0 disables) */
void ui_set_autocycle(uint16_t seconds);

#ifdef __cplusplus
}
#endif
