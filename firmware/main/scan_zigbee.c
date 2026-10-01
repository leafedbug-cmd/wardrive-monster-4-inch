#include "scanners.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdlog.h"
#include "store.h"

static const char *TAG = "scan_zigbee";

/* ------------------------------------------------------------------ *
 *  Why this scanner reports nothing                                   *
 *                                                                     *
 *  Zigbee needs raw IEEE 802.15.4. The C6 silicon has the radio, but: *
 *                                                                     *
 *  1. The stock ESP-Hosted slave firmware exposes Wi-Fi, BLE HCI and  *
 *     OpenThread to the host -- not raw 802.15.4, and not Zigbee.     *
 *                                                                     *
 *  2. Zigbee-over-hosted (esp_hosted 3.x) reaches the C6 as an        *
 *     802.15.4 RCP over a DEDICATED UART carrying spinel, in addition *
 *     to SDIO. On this board the C6's UART is on unrouted pads, so    *
 *     that link does not physically exist without soldering.          *
 *                                                                     *
 *  3. Wi-Fi, BLE and 802.15.4 share one 2.4 GHz front end. Even with  *
 *     the wiring, a Zigbee scan would take duty cycle away from the   *
 *     Wi-Fi and BLE screens rather than running alongside them.       *
 *                                                                     *
 *  Rather than invent plausible-looking rows, this module stays idle  *
 *  and says so. The plumbing below is real: when an 802.15.4 source   *
 *  does exist, zigbee_report() is the only function that needs        *
 *  calling. See docs/C6-OTA.md for the two upgrade paths.             *
 * ------------------------------------------------------------------ */

static scanner_status_t s_st;

/* The hook a future 802.15.4 source would call. Unused today, and kept
 * deliberately: it is the whole integration surface. */
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
    s_st.state = SCAN_UNAVAILABLE;
    strlcpy(s_st.detail, "802.15.4 not exposed by C6 firmware",
            sizeof(s_st.detail));
    ESP_LOGW(TAG, "Zigbee unavailable: the ESP-Hosted slave does not expose "
                  "802.15.4, and the C6 UART needed for an RCP link is on "
                  "unrouted pads. See docs/C6-OTA.md.");
    return ESP_ERR_NOT_SUPPORTED;
}

void scan_zigbee_status(scanner_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}
