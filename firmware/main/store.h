/*
 * store.h -- the one place every detection lands.
 *
 * Scanners upsert here; the UI and the SD logger read from here. All access is
 * mutex-guarded and the backing table lives in PSRAM.
 *
 * Dedup key is (kind, mac). A repeat sighting bumps hits/rssi/last_seen rather
 * than adding a row, so "unique" means unique devices, not packets.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DET_WIFI = 0,
    DET_BLE,
    DET_MATTER,
    DET_ZIGBEE,
    DET_KIND_COUNT
} det_kind_t;

/* How a Matter device was spotted. */
typedef enum {
    MATTER_VIA_NONE = 0,
    MATTER_VIA_BLE,          /* BLE advert carrying service UUID 0xFFF6 */
    MATTER_VIA_MDNS_COMM,    /* _matterc._udp -- commissionable          */
    MATTER_VIA_MDNS_OPER,    /* _matter._tcp  -- already on a fabric     */
} matter_via_t;

#define DET_NAME_LEN 33

/* No real radio reports +127 dBm, so it stands in for "not measured" --
 * mDNS-discovered Matter nodes carry no signal strength. */
#define DET_RSSI_NA 127

typedef struct {
    uint8_t  kind;                 /* det_kind_t                         */
    uint8_t  mac[6];
    int8_t   rssi;                 /* most recent                        */
    int8_t   rssi_best;            /* strongest ever seen                */
    uint8_t  channel;
    uint16_t hits;                 /* saturates, see STORE_HITS_MAX      */
    int64_t  first_us;
    int64_t  last_us;
    char     name[DET_NAME_LEN];   /* SSID / BLE local name / instance   */

    union {
        struct {
            uint8_t authmode;      /* wifi_auth_mode_t                   */
            bool    hidden;
        } wifi;
        struct {
            uint8_t  addr_type;
            uint16_t company;      /* manufacturer-data company ID       */
            bool     connectable;
        } ble;
        struct {
            uint16_t vendor_id;
            uint16_t product_id;
            uint16_t discriminator;
            uint8_t  via;          /* matter_via_t                       */
        } matter;
        struct {
            uint16_t panid;
            uint8_t  lqi;
        } zigbee;
    } x;
} detection_t;

#define STORE_HITS_MAX 0xFFFF

typedef struct {
    uint32_t unique;        /* distinct devices of this kind             */
    uint32_t hits;          /* total sightings                           */
    uint32_t new_last_min;  /* first-seen in the trailing 60 s           */
    int8_t   best_rssi;
    int64_t  last_us;       /* most recent sighting of any device        */
} store_stats_t;

/* Capacity is clamped internally. 4096 covers a long drive comfortably. */
esp_err_t store_init(size_t capacity);

/* Insert or merge. Returns true when this device had not been seen before.
 * On a new device the record is also handed to the SD logger. */
bool store_upsert(const detection_t *det);

/* Per-kind counters. Safe to call from the UI task. */
void store_stats(det_kind_t kind, store_stats_t *out);

/* Totals across every kind. */
void store_stats_total(store_stats_t *out);

/* Copy up to max_out records of one kind into out.
 * sort_by_rssi picks the strongest; otherwise the most recently seen.
 * Returns how many were written. */
size_t store_snapshot(det_kind_t kind, detection_t *out, size_t max_out,
                      bool sort_by_rssi);

/* Wi-Fi channel occupancy, index 0..13 == channels 1..14. */
void store_wifi_channel_hist(uint16_t hist[14]);

/* How full the table is, 0..100. */
uint8_t store_fill_pct(void);

void store_clear(void);

#ifdef __cplusplus
}
#endif
