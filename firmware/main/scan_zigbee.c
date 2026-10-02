#include "scanners.h"

#include <stdio.h>
#include <string.h>

#include "c6ext_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdlog.h"
#include "store.h"

static const char *TAG = "scan_zigbee";

/* ------------------------------------------------------------------ *
 *  Where the data comes from                                          *
 *                                                                     *
 *  Not from the on-board C6. That chip has the 802.15.4 radio, but:   *
 *                                                                     *
 *  1. The stock ESP-Hosted slave exposes Wi-Fi, BLE HCI and           *
 *     OpenThread to the host -- not raw 802.15.4, and not Zigbee.     *
 *  2. Zigbee-over-hosted reaches the C6 as an RCP over a DEDICATED    *
 *     spinel UART, and on this board that UART is on unrouted pads.   *
 *  3. The RCP role is built with Wi-Fi OFF, so even soldered, it      *
 *     would cost the Wi-Fi screen entirely.                           *
 *                                                                     *
 *  So 802.15.4 comes from the EXTERNAL C6 on the 40-pin header -- a   *
 *  second chip with its own front end and its own antenna, reporting  *
 *  decoded sightings over a UART. See c6ext_link.h and docs/WIRING.md.*
 *                                                                     *
 *  With no external C6 fitted this module reports SCAN_UNAVAILABLE    *
 *  and the screen says so, rather than inventing rows.                *
 * ------------------------------------------------------------------ */

static scanner_status_t s_st;

/* The hook the 802.15.4 source calls. c6ext_link.c is that source today;
 * it stays a plain function so any other one can be dropped in. */
void zigbee_report(const uint8_t ext_addr[8], uint16_t panid,
                   uint8_t channel, int8_t rssi, uint8_t lqi);

void zigbee_report(const uint8_t ext_addr[8], uint16_t panid,
                   uint8_t channel, int8_t rssi, uint8_t lqi)
{
    detection_t d = {0};
    d.kind    = DET_ZIGBEE;
    d.rssi    = rssi;
    d.channel = channel;
    memcpy(d.mac, ext_addr + 2, 6);   /* low 6 of the EUI-64 */
    d.x.zigbee.panid = panid;
    d.x.zigbee.lqi   = lqi;
    snprintf(d.name, sizeof(d.name), "PAN %04X", panid);

    s_st.reports++;
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }
}

esp_err_t scan_zigbee_start(void)
{
    /* Nothing to start: the link task owns the UART and calls
     * zigbee_report() as frames arrive. All this does is decide what the
     * screen should say until the first heartbeat lands. */
    s_st.state = SCAN_UNAVAILABLE;
    strlcpy(s_st.detail, "waiting for external C6", sizeof(s_st.detail));

    ESP_LOGI(TAG, "802.15.4 comes from the external C6 over UART; "
                  "idle until it reports in");
    return ESP_OK;
}

void scan_zigbee_status(scanner_status_t *out)
{
    if (!out) {
        return;
    }

    scanner_status_t link;
    c6ext_link_status(&link);

    if (link.state != SCAN_RUNNING) {
        s_st.state = SCAN_UNAVAILABLE;
        strlcpy(s_st.detail, link.detail, sizeof(s_st.detail));
    } else {
        uint32_t zb = 0;
        c6ext_link_counts(NULL, NULL, &zb);
        s_st.state = SCAN_RUNNING;
        snprintf(s_st.detail, sizeof(s_st.detail), "%s", link.detail);
        s_st.cycles = zb;
    }

    *out = s_st;
}
