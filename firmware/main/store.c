#include "store.h"

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "store";

#define RATE_WINDOW_S 60

typedef struct {
    uint32_t bucket[RATE_WINDOW_S]; /* new devices per second, ring */
    int64_t  last_sec;
    uint32_t unique;
    uint32_t hits;
    int8_t   best_rssi;
    int64_t  last_us;
} kind_stat_t;

static detection_t   *s_tab;
static size_t         s_cap;        /* always a power of two */
static size_t         s_count;
static kind_stat_t    s_stat[DET_KIND_COUNT];
static uint16_t       s_chan_hist[14];
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */

static inline bool slot_used(const detection_t *d)
{
    return d->hits != 0;
}

static size_t hash_key(uint8_t kind, const uint8_t mac[6])
{
    /* FNV-1a over kind + mac */
    uint32_t h = 2166136261u;
    h = (h ^ kind) * 16777619u;
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    return h & (s_cap - 1);
}

static inline bool same_dev(const detection_t *a, uint8_t kind, const uint8_t mac[6])
{
    return a->kind == kind && memcmp(a->mac, mac, 6) == 0;
}

/* Roll the per-second ring forward to `now_sec`, zeroing skipped buckets. */
static void rate_advance(kind_stat_t *k, int64_t now_sec)
{
    if (k->last_sec == 0) {
        k->last_sec = now_sec;
        return;
    }
    int64_t gap = now_sec - k->last_sec;
    if (gap <= 0) {
        return;
    }
    if (gap >= RATE_WINDOW_S) {
        memset(k->bucket, 0, sizeof(k->bucket));
    } else {
        for (int64_t i = 1; i <= gap; i++) {
            k->bucket[(k->last_sec + i) % RATE_WINDOW_S] = 0;
        }
    }
    k->last_sec = now_sec;
}

static uint32_t rate_sum(const kind_stat_t *k)
{
    uint32_t n = 0;
    for (int i = 0; i < RATE_WINDOW_S; i++) {
        n += k->bucket[i];
    }
    return n;
}

/* ------------------------------------------------------------------ */

esp_err_t store_init(size_t capacity)
{
    if (s_tab) {
        return ESP_OK;
    }

    /* round up to a power of two, clamp to something sane */
    size_t cap = 256;
    while (cap < capacity && cap < 16384) {
        cap <<= 1;
    }

    s_tab = heap_caps_calloc(cap, sizeof(detection_t), MALLOC_CAP_SPIRAM);
    if (!s_tab) {
        ESP_LOGW(TAG, "no PSRAM for %u entries, falling back to internal", (unsigned)cap);
        cap = 512;
        s_tab = calloc(cap, sizeof(detection_t));
    }
    if (!s_tab) {
        return ESP_ERR_NO_MEM;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        free(s_tab);
        s_tab = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cap = cap;
    s_count = 0;
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        s_stat[i].best_rssi = -128;
    }

    ESP_LOGI(TAG, "table ready: %u entries, %u KiB",
             (unsigned)cap, (unsigned)(cap * sizeof(detection_t) / 1024));
    return ESP_OK;
}

bool store_upsert(const detection_t *det)
{
    if (!s_tab || !det || det->kind >= DET_KIND_COUNT) {
        return false;
    }

    const int64_t now = esp_timer_get_time();
    bool is_new = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t idx = hash_key(det->kind, det->mac);
    detection_t *slot = NULL;

    /* Linear probe for an existing match or the first free slot. */
    for (size_t probe = 0; probe < s_cap; probe++) {
        detection_t *cur = &s_tab[(idx + probe) & (s_cap - 1)];
        if (!slot_used(cur)) {
            slot = cur;
            is_new = true;
            break;
        }
        if (same_dev(cur, det->kind, det->mac)) {
            slot = cur;
            break;
        }
    }

    if (!slot) {
        /* Table full. Keep counting hits so the rate stays honest. */
        kind_stat_t *k = &s_stat[det->kind];
        k->hits++;
        k->last_us = now;
        xSemaphoreGive(s_lock);
        return false;
    }

    kind_stat_t *k = &s_stat[det->kind];
    rate_advance(k, now / 1000000);

    if (is_new) {
        memcpy(slot, det, sizeof(*slot));
        slot->first_us  = now;
        slot->last_us   = now;
        slot->hits      = 1;
        slot->rssi_best = det->rssi;
        s_count++;
        k->unique++;
        k->bucket[(now / 1000000) % RATE_WINDOW_S]++;

        if (det->kind == DET_WIFI && det->channel >= 1 && det->channel <= 14) {
            s_chan_hist[det->channel - 1]++;
        }
    } else {
        slot->last_us = now;
        if (slot->hits < STORE_HITS_MAX) {
            slot->hits++;
        }
        /* A sighting with no measured RSSI must not overwrite a real one. */
        if (det->rssi != DET_RSSI_NA) {
            slot->rssi = det->rssi;
            if (slot->rssi_best == DET_RSSI_NA || det->rssi > slot->rssi_best) {
                slot->rssi_best = det->rssi;
            }
        }
        if (det->channel) {
            slot->channel = det->channel;
        }
        /* A later sighting may carry a name the first one lacked. */
        if (slot->name[0] == '\0' && det->name[0] != '\0') {
            memcpy(slot->name, det->name, DET_NAME_LEN);
        }
        /* Matter: mDNS carries richer identity than a BLE beacon. */
        if (det->kind == DET_MATTER && det->x.matter.vendor_id) {
            slot->x.matter.vendor_id = det->x.matter.vendor_id;
            slot->x.matter.product_id = det->x.matter.product_id;
            if (det->x.matter.via) {
                slot->x.matter.via = det->x.matter.via;
            }
        }
    }

    k->hits++;
    k->last_us = now;
    if (det->rssi != DET_RSSI_NA && det->rssi > k->best_rssi) {
        k->best_rssi = det->rssi;
    }

    xSemaphoreGive(s_lock);
    return is_new;
}

void store_stats(det_kind_t kind, store_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->best_rssi = -128;
    if (!s_tab || kind >= DET_KIND_COUNT) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    kind_stat_t *k = &s_stat[kind];
    rate_advance(k, esp_timer_get_time() / 1000000);
    out->unique       = k->unique;
    out->hits         = k->hits;
    out->new_last_min = rate_sum(k);
    out->best_rssi    = k->best_rssi;
    out->last_us      = k->last_us;
    xSemaphoreGive(s_lock);
}

void store_stats_total(store_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->best_rssi = -128;
    if (!s_tab) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now_sec = esp_timer_get_time() / 1000000;
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        kind_stat_t *k = &s_stat[i];
        rate_advance(k, now_sec);
        out->unique       += k->unique;
        out->hits         += k->hits;
        out->new_last_min += rate_sum(k);
        if (k->best_rssi > out->best_rssi) {
            out->best_rssi = k->best_rssi;
        }
        if (k->last_us > out->last_us) {
            out->last_us = k->last_us;
        }
    }
    xSemaphoreGive(s_lock);
}

size_t store_snapshot(det_kind_t kind, detection_t *out, size_t max_out,
                      bool sort_by_rssi)
{
    if (!s_tab || !out || max_out == 0 || kind >= DET_KIND_COUNT) {
        return 0;
    }

    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (size_t i = 0; i < s_cap; i++) {
        detection_t *cur = &s_tab[i];
        if (!slot_used(cur) || cur->kind != kind) {
            continue;
        }

        /* Insertion sort into the top-N window. */
        size_t pos = n;
        while (pos > 0) {
            bool better = sort_by_rssi
                ? (cur->rssi_best > out[pos - 1].rssi_best)
                : (cur->last_us   > out[pos - 1].last_us);
            if (!better) {
                break;
            }
            pos--;
        }
        if (pos >= max_out) {
            continue;
        }

        size_t end = (n < max_out) ? n : max_out - 1;
        if (end > pos) {
            memmove(&out[pos + 1], &out[pos], (end - pos) * sizeof(detection_t));
        }
        out[pos] = *cur;
        if (n < max_out) {
            n++;
        }
    }

    xSemaphoreGive(s_lock);
    return n;
}

void store_wifi_channel_hist(uint16_t hist[14])
{
    if (!hist) {
        return;
    }
    if (!s_tab) {
        memset(hist, 0, sizeof(uint16_t) * 14);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(hist, s_chan_hist, sizeof(s_chan_hist));
    xSemaphoreGive(s_lock);
}

uint8_t store_fill_pct(void)
{
    if (!s_tab || s_cap == 0) {
        return 0;
    }
    return (uint8_t)((s_count * 100) / s_cap);
}

void store_clear(void)
{
    if (!s_tab) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_tab, 0, s_cap * sizeof(detection_t));
    memset(s_stat, 0, sizeof(s_stat));
    memset(s_chan_hist, 0, sizeof(s_chan_hist));
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        s_stat[i].best_rssi = -128;
    }
    s_count = 0;
    xSemaphoreGive(s_lock);
}
