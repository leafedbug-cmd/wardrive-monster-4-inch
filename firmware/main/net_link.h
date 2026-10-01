/*
 * net_link.h -- bring the ESP32-C6 up as the P4's radio.
 *
 * The P4 has no radio of its own. esp_hosted tunnels Wi-Fi and BLE HCI over
 * SDIO to the C6, so esp_wifi_* and NimBLE behave as though they were local.
 *
 * Everything else in this firmware assumes net_link_start() has succeeded.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     hosted_up;      /* SDIO link to the C6 is alive        */
    bool     wifi_up;        /* esp_wifi started (STA, not joined)  */
    bool     ble_up;         /* NimBLE host synced                  */
    uint32_t cp_chip_id;
    char     cp_target[16];  /* e.g. "esp32c6"                      */
    char     cp_fw[32];      /* co-processor esp_hosted version     */
} net_link_status_t;

/* NVS, netif, the default event loop, esp_hosted, and Wi-Fi in STA mode.
 * Does not join any network -- this build only ever scans. */
esp_err_t net_link_start(void);

void net_link_status(net_link_status_t *out);

/* Scanners call this to mark BLE as ready once NimBLE reports sync. */
void net_link_set_ble_up(bool up);

#ifdef __cplusplus
}
#endif
