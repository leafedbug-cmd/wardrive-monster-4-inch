/*
 * c6ext_link.h -- the external ESP32-C6 (XIAO) second radio.
 *
 * The board's on-board C6 has one 2.4 GHz front end shared by Wi-Fi, BLE
 * and 802.15.4, so those protocols take airtime from each other. A second
 * C6 on the 40-pin header -- with its own antenna -- is a second front end.
 *
 * It does NOT run ESP-Hosted. It runs a standalone scanner that decodes
 * locally and reports finished sightings over a UART (see
 * shared/c6ext_proto.h for why that slow link is fine). Those sightings go
 * into the same store as the on-board radio's.
 *
 * Merging needs no special case: store_upsert() already dedups on
 * (kind, mac), so an AP both radios hear becomes one row that keeps the
 * better RSSI -- which, given the external antenna, is usually the XIAO's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "scanners.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the UART and start the reader task. Returns ESP_OK once the port is
 * up -- NOT once the XIAO answers, which may never happen if none is
 * fitted. Absence is a normal state, not an error: check c6ext_link_up(). */
esp_err_t c6ext_link_start(void);

/* True while a HELLO has arrived within C6EXT_LINK_TIMEOUT_MS. */
bool c6ext_link_up(void);

/* Link health for the UI. `detail` is a one-line human summary. */
void c6ext_link_status(scanner_status_t *out);

/* Per-protocol sighting counts contributed by the external radio, so the
 * UI can show what the second antenna is actually adding. */
void c6ext_link_counts(uint32_t *wifi, uint32_t *ble, uint32_t *zigbee);

/* Reassign what the external radio spends its airtime on (C6EXT_ROLE_*).
 * One front end: enabling everything thirds the airtime of each. */
esp_err_t c6ext_link_set_roles(uint8_t roles);

#ifdef __cplusplus
}
#endif
