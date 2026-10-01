#include "scanners.h"

#include <string.h>

#include "c6_ota.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdlog.h"
#include "store.h"

static const char *TAG = "scan_wifi";

/* Passive scanning only listens for beacons -- no probe requests go out.
 * Slower than active, but it is the right default for a survey tool and it
 * keeps the shared radio quieter for the BLE scanner. */
#define PASSIVE_DWELL_MS   120
#define SWEEP_GAP_MS       400
#define MAX_RECORDS        64

static scanner_status_t s_st;

static void record_ap(const wifi_ap_record_t *ap)
{
    detection_t d = {0};
    d.kind    = DET_WIFI;
    d.rssi    = ap->rssi;
    d.channel = ap->primary;
    memcpy(d.mac, ap->bssid, 6);

    /* A zero-length SSID is a hidden network, not a missing one. */
    if (ap->ssid[0]) {
        strlcpy(d.name, (const char *)ap->ssid, sizeof(d.name));
    } else {
        d.x.wifi.hidden = true;
    }
    d.x.wifi.authmode = ap->authmode;

    s_st.reports++;
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }
}

static void sweep(void)
{
    const wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,               /* every channel */
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time   = {
            .passive = PASSIVE_DWELL_MS,
        },
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        snprintf(s_st.detail, sizeof(s_st.detail), "scan error: %s",
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK || n == 0) {
        return;
    }
    if (n > MAX_RECORDS) {
        n = MAX_RECORDS;
    }

    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return;
    }

    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            record_ap(&recs[i]);
        }
        snprintf(s_st.detail, sizeof(s_st.detail), "%u AP%s last sweep",
                 n, n == 1 ? "" : "s");
    }

    free(recs);
    s_st.cycles++;
}

static void wifi_task(void *arg)
{
    s_st.state = SCAN_RUNNING;
    strlcpy(s_st.detail, "starting", sizeof(s_st.detail));

    for (;;) {
        /* Stand down while the C6 is being reflashed -- the radio is gone. */
        if (c6_ota_busy()) {
            s_st.state = SCAN_RESTARTING;
            strlcpy(s_st.detail, "paused: C6 update", sizeof(s_st.detail));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_st.state = SCAN_RUNNING;
        sweep();
        vTaskDelay(pdMS_TO_TICKS(SWEEP_GAP_MS));
    }
}

esp_err_t scan_wifi_start(void)
{
    if (xTaskCreate(wifi_task, "scan_wifi", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void scan_wifi_status(scanner_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}
