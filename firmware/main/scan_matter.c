#include "scanners.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "c6_ota.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdlog.h"
#include "store.h"

static const char *TAG = "scan_matter";

/* Matter needs no 802.15.4 to be found here:
 *
 *   _matterc._udp  a node with an open commissioning window
 *   _matter._tcp   a node already operational on a fabric
 *
 * plus BLE adverts under service UUID 0xFFF6, which scan_ble.c forwards.
 * Between them these cover Matter-over-Wi-Fi and the BLE commissioning
 * phase of every Matter device, Thread ones included. */
#define BROWSE_TIMEOUT_MS  3000
#define MAX_RESULTS        32
#define SWEEP_GAP_MS       10000

static scanner_status_t s_st;

/* ------------------------------------------------------------------ */

static const char *txt_lookup(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcasecmp(r->txt[i].key, key) == 0) {
            return r->txt[i].value;
        }
    }
    return NULL;
}

/* Matter operational hostnames are the node's 64-bit MAC in hex, e.g.
 * "B827EBFFFE1A2B3C.local". Pull the low 6 bytes when we can. */
static bool mac_from_hostname(const char *host, uint8_t out[6])
{
    if (!host) {
        return false;
    }
    char hex[17];
    size_t n = 0;
    for (const char *p = host; *p && *p != '.' && n < 16; p++) {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        hex[n++] = *p;
    }
    if (n != 12 && n != 16) {
        return false;
    }
    hex[n] = '\0';

    /* Use the last 12 hex digits so EUI-64 and EUI-48 both land sensibly. */
    const char *tail = hex + (n - 12);
    for (int i = 0; i < 6; i++) {
        char byte[3] = { tail[i * 2], tail[i * 2 + 1], '\0' };
        out[i] = (uint8_t)strtoul(byte, NULL, 16);
    }
    return true;
}

/* Fallback identity: fold the service instance name into 6 bytes so the
 * store can still dedup. Synthetic, but stable for a given device. */
static void mac_from_name(const char *name, uint8_t out[6])
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)name; p && *p; p++) {
        h = (h ^ *p) * 1099511628211ULL;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)(h >> (i * 8));
    }
    out[0] |= 0x02;  /* mark locally administered -- this is not a real MAC */
}

static void record_result(const mdns_result_t *r, matter_via_t via)
{
    detection_t d = {0};
    d.kind = DET_MATTER;
    d.rssi = DET_RSSI_NA;          /* mDNS carries no signal strength */
    d.x.matter.via = via;

    if (!mac_from_hostname(r->hostname, d.mac)) {
        mac_from_name(r->instance_name ? r->instance_name : "?", d.mac);
    }

    /* Prefer a human-set device name, else the service instance. */
    const char *dn = txt_lookup(r, "DN");
    const char *label = dn ? dn : r->instance_name;
    if (label) {
        strlcpy(d.name, label, sizeof(d.name));
    }

    const char *disc = txt_lookup(r, "D");
    if (disc) {
        d.x.matter.discriminator = (uint16_t)strtoul(disc, NULL, 10);
    }

    /* VP is "vendor+product" in decimal, e.g. "65521+32769". */
    const char *vp = txt_lookup(r, "VP");
    if (vp) {
        char *end = NULL;
        d.x.matter.vendor_id = (uint16_t)strtoul(vp, &end, 10);
        if (end && *end == '+') {
            d.x.matter.product_id = (uint16_t)strtoul(end + 1, NULL, 10);
        }
    }

    s_st.reports++;
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }
}

static int browse(const char *service, const char *proto, matter_via_t via)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(service, proto, BROWSE_TIMEOUT_MS,
                                   MAX_RESULTS, &results);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "%s%s query: %s", service, proto, esp_err_to_name(err));
        return 0;
    }

    int n = 0;
    for (mdns_result_t *r = results; r; r = r->next, n++) {
        record_result(r, via);
    }
    mdns_query_results_free(results);
    return n;
}

static void matter_task(void *arg)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed");
        s_st.state = SCAN_UNAVAILABLE;
        strlcpy(s_st.detail, "mDNS unavailable", sizeof(s_st.detail));
        vTaskDelete(NULL);
        return;
    }
    mdns_hostname_set("wardrive-monster");

    s_st.state = SCAN_RUNNING;
    strlcpy(s_st.detail, "browsing _matterc/_matter", sizeof(s_st.detail));

    for (;;) {
        if (c6_ota_busy()) {
            s_st.state = SCAN_RESTARTING;
            strlcpy(s_st.detail, "paused: C6 update", sizeof(s_st.detail));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_st.state = SCAN_RUNNING;
        int comm = browse("_matterc", "_udp", MATTER_VIA_MDNS_COMM);
        int oper = browse("_matter",  "_tcp", MATTER_VIA_MDNS_OPER);
        s_st.cycles++;

        snprintf(s_st.detail, sizeof(s_st.detail),
                 "mDNS %d commissionable, %d operational", comm, oper);

        vTaskDelay(pdMS_TO_TICKS(SWEEP_GAP_MS));
    }
}

/* ------------------------------------------------------------------ *
 *  Called from the BLE scanner when it decodes a 0xFFF6 advert.       *
 * ------------------------------------------------------------------ */
void scan_matter_note_ble(const uint8_t mac[6], int8_t rssi,
                          uint16_t discriminator, uint16_t vendor_id,
                          uint16_t product_id)
{
    detection_t d = {0};
    d.kind = DET_MATTER;
    d.rssi = rssi;
    memcpy(d.mac, mac, 6);
    d.x.matter.via           = MATTER_VIA_BLE;
    d.x.matter.discriminator = discriminator;
    d.x.matter.vendor_id     = vendor_id;
    d.x.matter.product_id    = product_id;
    snprintf(d.name, sizeof(d.name), "commissionable %04X:%04X",
             vendor_id, product_id);

    s_st.reports++;
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }
}

esp_err_t scan_matter_start(void)
{
    if (xTaskCreate(matter_task, "scan_matter", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void scan_matter_status(scanner_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}
