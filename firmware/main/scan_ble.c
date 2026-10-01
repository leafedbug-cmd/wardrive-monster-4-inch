#include "scanners.h"

#include <string.h>

#include "c6_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_link.h"
#include "sdlog.h"
#include "store.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "scan_ble";

/* ------------------------------------------------------------------ *
 *  Known issue: esp-hosted-mcu#180                                    *
 *                                                                     *
 *  On ESP32-P4 + C6 over SDIO the controller stops delivering         *
 *  advertising reports after roughly 90 seconds. Passive scanning is  *
 *  markedly more reliable than active -- the fault appears to be in   *
 *  the C6's TX path -- so we scan passively and tear the scan down    *
 *  and restart it well inside the failure window.                     *
 * ------------------------------------------------------------------ */
#define RESCAN_PERIOD_MS   60000
#define STALL_TIMEOUT_MS   15000   /* no reports at all for this long  */

/* Matter commissioning adverts carry service data under this UUID. */
#define MATTER_SVC_UUID16  0xFFF6

static scanner_status_t s_st;
static uint8_t          s_own_addr_type;
static volatile int64_t s_last_report_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ------------------------------------------------------------------ *
 *  Matter service data                                                *
 *                                                                     *
 *  Layout (Matter spec, commissionable payload):                      *
 *    [0]    opcode, 0x00 = commissionable                             *
 *    [1..2] uint16 LE: bits 0..11 discriminator, bits 12..15 version  *
 *    [3..4] uint16 LE: vendor id                                      *
 *    [5..6] uint16 LE: product id                                     *
 * ------------------------------------------------------------------ */
static bool parse_matter_svc(const uint8_t *sd, uint8_t len,
                             uint16_t *disc, uint16_t *vid, uint16_t *pid)
{
    if (len < 7 || sd[0] != 0x00) {
        return false;
    }
    uint16_t w = (uint16_t)(sd[1] | (sd[2] << 8));
    *disc = w & 0x0FFF;
    *vid  = (uint16_t)(sd[3] | (sd[4] << 8));
    *pid  = (uint16_t)(sd[5] | (sd[6] << 8));
    return true;
}

static void handle_report(const struct ble_gap_disc_desc *desc)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, desc->data, desc->length_data) != 0) {
        return;
    }

    s_last_report_ms = now_ms();
    s_st.reports++;

    detection_t d = {0};
    d.kind = DET_BLE;
    d.rssi = desc->rssi;
    memcpy(d.mac, desc->addr.val, 6);
    d.x.ble.addr_type   = desc->addr.type;
    d.x.ble.connectable = (desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);

    if (f.name && f.name_len) {
        size_t n = f.name_len < DET_NAME_LEN - 1 ? f.name_len : DET_NAME_LEN - 1;
        memcpy(d.name, f.name, n);
        d.name[n] = '\0';
    }

    /* Manufacturer data starts with a little-endian company identifier. */
    if (f.mfg_data && f.mfg_data_len >= 2) {
        d.x.ble.company = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));
    }

    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }

    /* Matter devices in commissioning mode beacon over BLE before they are
     * on any network, so this catches them earlier than mDNS can. */
    if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 9) {
        uint16_t uuid = (uint16_t)(f.svc_data_uuid16[0] |
                                   (f.svc_data_uuid16[1] << 8));
        if (uuid == MATTER_SVC_UUID16) {
            uint16_t disc = 0, vid = 0, pid = 0;
            if (parse_matter_svc(f.svc_data_uuid16 + 2,
                                 f.svc_data_uuid16_len - 2, &disc, &vid, &pid)) {
                scan_matter_note_ble(desc->addr.val, desc->rssi, disc, vid, pid);
            }
        }
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_report(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGD(TAG, "discovery complete (reason %d)", event->disc_complete.reason);
        break;
    default:
        break;
    }
    return 0;
}

static int start_discovery(void)
{
    struct ble_gap_disc_params p = {
        .itvl              = BLE_GAP_SCAN_ITVL_MS(60),
        .window            = BLE_GAP_SCAN_WIN_MS(60),
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
        .passive           = 1,   /* never transmit SCAN_REQ -- see note above */
        .filter_duplicates = 0,   /* we want repeat sightings for hit counts   */
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
        return rc;
    }
    s_last_report_ms = now_ms();
    return 0;
}

/* Periodically bounce the scan so the ~90 s stall never bites. Also acts as
 * a recovery path if reports dry up early. */
static void watchdog_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (c6_ota_busy()) {
            s_st.state = SCAN_RESTARTING;
            strlcpy(s_st.detail, "paused: C6 update", sizeof(s_st.detail));
            continue;
        }
        if (s_st.state == SCAN_STOPPED) {
            continue;
        }

        int64_t idle = now_ms() - s_last_report_ms;
        bool periodic = idle >= RESCAN_PERIOD_MS;
        bool stalled  = idle >= STALL_TIMEOUT_MS;

        if (periodic || stalled) {
            s_st.state = SCAN_RESTARTING;
            ble_gap_disc_cancel();
            vTaskDelay(pdMS_TO_TICKS(50));
            if (start_discovery() == 0) {
                s_st.cycles++;
                s_st.state = SCAN_RUNNING;
                snprintf(s_st.detail, sizeof(s_st.detail),
                         "%s restart #%lu",
                         stalled ? "stall" : "routine",
                         (unsigned long)s_st.cycles);
            } else {
                snprintf(s_st.detail, sizeof(s_st.detail), "restart failed");
            }
        } else {
            s_st.state = SCAN_RUNNING;
            snprintf(s_st.detail, sizeof(s_st.detail),
                     "passive, %lu reports", (unsigned long)s_st.reports);
        }
    }
}

/* ------------------------------------------------------------------ */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no BLE identity address");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "address type inference failed");
        return;
    }

    net_link_set_ble_up(true);
    s_st.state = SCAN_RUNNING;
    if (start_discovery() == 0) {
        ESP_LOGI(TAG, "passive scan running");
        strlcpy(s_st.detail, "passive scan running", sizeof(s_st.detail));
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "controller reset, reason %d", reason);
    net_link_set_ble_up(false);
    s_st.state = SCAN_RESTARTING;
    snprintf(s_st.detail, sizeof(s_st.detail), "controller reset (%d)", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t scan_ble_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        s_st.state = SCAN_UNAVAILABLE;
        strlcpy(s_st.detail, "NimBLE init failed", sizeof(s_st.detail));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);

    if (xTaskCreate(watchdog_task, "ble_wd", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void scan_ble_status(scanner_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}
