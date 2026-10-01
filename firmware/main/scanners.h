/*
 * scanners.h -- the four detection sources.
 *
 * Each scanner owns a task, upserts into the store, and hands genuinely new
 * devices to the SD logger. They share the C6's single 2.4 GHz front end, so
 * they are deliberately duty-cycled rather than run flat out.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCAN_STOPPED = 0,
    SCAN_RUNNING,
    SCAN_RESTARTING,
    SCAN_UNAVAILABLE,   /* hardware or firmware cannot do this at all */
} scan_state_t;

typedef struct {
    scan_state_t state;
    uint32_t     cycles;      /* completed sweeps / restarts          */
    uint32_t     reports;     /* raw sightings handed to the store    */
    char         detail[64];  /* human-readable status for the UI     */
} scanner_status_t;

/* Wi-Fi: passive all-channel sweep via esp_wifi_remote on the C6. */
esp_err_t scan_wifi_start(void);
void      scan_wifi_status(scanner_status_t *out);

/* BLE: passive observer via NimBLE against the C6's controller.
 * Also feeds the Matter screen when an advert carries UUID 0xFFF6. */
esp_err_t scan_ble_start(void);
void      scan_ble_status(scanner_status_t *out);

/* Matter: DNS-SD browse for _matterc._udp and _matter._tcp over Wi-Fi. */
esp_err_t scan_matter_start(void);
void      scan_matter_status(scanner_status_t *out);

/* Called by scan_ble when it decodes a Matter commissioning advert. */
void scan_matter_note_ble(const uint8_t mac[6], int8_t rssi,
                          uint16_t discriminator, uint16_t vendor_id,
                          uint16_t product_id);

/* Zigbee: requires raw 802.15.4, which the stock ESP-Hosted slave does not
 * expose. Reports SCAN_UNAVAILABLE and explains why. See docs/C6-OTA.md. */
esp_err_t scan_zigbee_start(void);
void      scan_zigbee_status(scanner_status_t *out);

#ifdef __cplusplus
}
#endif
