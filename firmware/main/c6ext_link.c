#include "c6ext_link.h"

#include <string.h>

#include "bsp_pins.h"
#include "c6ext_proto.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdlog.h"
#include "store.h"
#include "chimera_ble.h"

static const char *TAG = "c6ext";

/* Three missed heartbeats. The XIAO sends one per second. */
#define C6EXT_LINK_TIMEOUT_MS   3500
#define RX_BUF_SZ               2048
#define RX_CHUNK                256

/* Declared in scan_zigbee.c -- the integration surface that module has
 * always exposed for "when an 802.15.4 source does exist". */
void zigbee_report(const uint8_t ext_addr[8], uint16_t panid,
                   uint8_t channel, int8_t rssi, uint8_t lqi);

static struct {
    volatile int64_t  last_hello_us;
    volatile uint32_t wifi, ble, zigbee;   /* accepted into the store   */
    volatile uint32_t bad_crc, resyncs;
    volatile uint32_t raw_bytes;   /* anything at all on the wire */
    c6ext_hello_t     hello;
    bool              hello_valid;
    bool              ver_warned;
    uint8_t           roles;
} s;

/* ------------------------------------------------------------------ *
 *  Sighting handlers -- each builds a detection_t and merges it       *
 * ------------------------------------------------------------------ */

static void on_wifi(const c6ext_wifi_t *w)
{
    detection_t d = {0};
    d.kind     = DET_WIFI;
    d.rssi     = w->rssi;
    d.channel  = w->channel;
    memcpy(d.mac, w->bssid, 6);
    d.x.wifi.authmode = w->authmode;
    d.x.wifi.hidden   = w->hidden ? true : false;

    uint8_t n = w->ssid_len;
    if (n > sizeof(d.name) - 1) {
        n = sizeof(d.name) - 1;
    }
    memcpy(d.name, w->ssid, n);
    d.name[n] = 0;

    s.wifi++;
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }
}

static void on_ble(const c6ext_ble_t *b)
{
    detection_t d = {0};
    d.kind = DET_BLE;
    d.rssi = b->rssi;
    memcpy(d.mac, b->addr, 6);
    d.x.ble.addr_type   = b->addr_type;
    d.x.ble.company     = b->company;
    d.x.ble.connectable = b->connectable ? true : false;

    uint8_t n = b->name_len;
    if (n > sizeof(d.name) - 1) {
        n = sizeof(d.name) - 1;
    }
    memcpy(d.name, b->name, n);
    d.name[n] = 0;

    s.ble++;
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }
    chimera_ingest_c6ext_ble(b);
}

static void on_zigbee(const c6ext_zigbee_t *z)
{
    s.zigbee++;
    zigbee_report(z->ext_addr, z->panid, z->channel, z->rssi, z->lqi);
}

static void on_hello(const c6ext_hello_t *h)
{
    if (h->proto_ver != C6EXT_PROTO_VERSION && !s.ver_warned) {
        s.ver_warned = true;
        ESP_LOGE(TAG, "protocol mismatch: XIAO speaks v%u, this build v%u. "
                      "Reflash both sides from the same shared/c6ext_proto.h.",
                 (unsigned)h->proto_ver, (unsigned)C6EXT_PROTO_VERSION);
    }
    s.hello         = *h;
    s.hello_valid   = true;
    s.last_hello_us = esp_timer_get_time();
}

/* ------------------------------------------------------------------ *
 *  Framing                                                            *
 * ------------------------------------------------------------------ */

static void dispatch(uint8_t type, const uint8_t *pl, uint8_t len)
{
    switch (type) {
    case C6EXT_MSG_WIFI:
        if (len == sizeof(c6ext_wifi_t))   { on_wifi((const c6ext_wifi_t *)pl); }
        break;
    case C6EXT_MSG_BLE:
        if (len == sizeof(c6ext_ble_t))    { on_ble((const c6ext_ble_t *)pl); }
        break;
    case C6EXT_MSG_ZIGBEE:
        if (len == sizeof(c6ext_zigbee_t)) { on_zigbee((const c6ext_zigbee_t *)pl); }
        break;
    case C6EXT_MSG_HELLO:
        if (len == sizeof(c6ext_hello_t))  { on_hello((const c6ext_hello_t *)pl); }
        break;
    default:
        break;   /* a newer XIAO may send things this build predates */
    }
}

/* Byte-at-a-time state machine. A corrupt frame costs one frame: we fall
 * back to hunting for the magic rather than trying to realign cleverly. */
static void feed(uint8_t c)
{
    static enum { W_M0, W_M1, W_LEN, W_TYPE, W_PAYLOAD, W_CRC } st = W_M0;
    static uint8_t len, type, idx, buf[C6EXT_MAX_PAYLOAD];

    switch (st) {
    case W_M0:
        if (c == C6EXT_MAGIC0) { st = W_M1; }
        break;

    case W_M1:
        /* A5 A5 is a plausible start-of-frame after a dropped byte, so
         * stay in W_M1 rather than falling all the way back. */
        if (c == C6EXT_MAGIC1)      { st = W_LEN; }
        else if (c == C6EXT_MAGIC0) { st = W_M1; }
        else                        { st = W_M0; s.resyncs++; }
        break;

    case W_LEN:
        if (c > C6EXT_MAX_PAYLOAD) { st = W_M0; s.resyncs++; }
        else                       { len = c; st = W_TYPE; }
        break;

    case W_TYPE:
        type = c;
        idx  = 0;
        st   = len ? W_PAYLOAD : W_CRC;
        break;

    case W_PAYLOAD:
        buf[idx++] = c;
        if (idx >= len) { st = W_CRC; }
        break;

    case W_CRC: {
        /* CRC covers type then payload. Run it over both in sequence,
         * which is what the sender does. */
        uint8_t crc = c6ext_crc8(&type, 1);
        for (uint8_t i = 0; i < len; i++) {
            crc ^= buf[i];
            for (int b = 0; b < 8; b++) {
                crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                                   : (uint8_t)(crc << 1);
            }
        }
        if (crc == c) { dispatch(type, buf, len); }
        else          { s.bad_crc++; }
        st = W_M0;
        break;
    }
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[RX_CHUNK];
    int64_t next_diag_us = 0;

    for (;;) {
        int n = uart_read_bytes(BSP_C6EXT_UART_PORT, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(200));
        if (n > 0) {
            s.raw_bytes += (uint32_t)n;
        }
        for (int i = 0; i < n; i++) {
            feed(chunk[i]);
        }

        /* While the link is down, say WHY once every 5 s. The distinction
         * that matters when bringing the wiring up is whether anything is
         * arriving at all: no bytes means the wire or the XIAO is dead,
         * whereas bytes with no valid frame means it is alive but
         * misconfigured -- wrong baud, or TX and RX crossed the wrong way. */
        int64_t now = esp_timer_get_time();
        if (!c6ext_link_up() && now >= next_diag_us) {
            next_diag_us = now + 5 * 1000 * 1000;
            if (s.raw_bytes == 0) {
                ESP_LOGW(TAG, "no heartbeat and NO BYTES on rx (gpio%d): "
                              "check GND, power, and that XIAO D6 reaches it",
                         (int)BSP_C6EXT_PIN_RX);
            } else {
                ESP_LOGW(TAG, "no heartbeat but %lu bytes seen "
                              "(crc errors %lu, resyncs %lu): wire is live, "
                              "so suspect baud or a protocol mismatch",
                         (unsigned long)s.raw_bytes,
                         (unsigned long)s.bad_crc,
                         (unsigned long)s.resyncs);
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Public                                                             *
 * ------------------------------------------------------------------ */

esp_err_t c6ext_link_start(void)
{
    const uart_config_t cfg = {
        .baud_rate  = BSP_C6EXT_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BSP_C6EXT_UART_PORT, RX_BUF_SZ, 0,
                                            0, NULL, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BSP_C6EXT_UART_PORT, &cfg),
                        TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BSP_C6EXT_UART_PORT, BSP_C6EXT_PIN_TX,
                                     BSP_C6EXT_PIN_RX, BSP_C6EXT_PIN_RTS,
                                     BSP_C6EXT_PIN_CTS),
                        TAG, "uart_set_pin");

    if (xTaskCreate(rx_task, "c6ext_rx", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "external C6 link open on UART%d (tx=%d rx=%d, %d baud); "
                  "waiting for a heartbeat",
             (int)BSP_C6EXT_UART_PORT, (int)BSP_C6EXT_PIN_TX,
             (int)BSP_C6EXT_PIN_RX, (int)BSP_C6EXT_BAUD_RATE);
    return ESP_OK;
}

bool c6ext_link_up(void)
{
    if (!s.hello_valid) {
        return false;
    }
    int64_t age_us = esp_timer_get_time() - s.last_hello_us;
    return age_us < (int64_t)C6EXT_LINK_TIMEOUT_MS * 1000;
}

void c6ext_link_counts(uint32_t *wifi, uint32_t *ble, uint32_t *zigbee)
{
    if (wifi)   { *wifi   = s.wifi; }
    if (ble)    { *ble    = s.ble; }
    if (zigbee) { *zigbee = s.zigbee; }
}

void c6ext_link_status(scanner_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!c6ext_link_up()) {
        out->state = SCAN_UNAVAILABLE;
        strlcpy(out->detail,
                s.hello_valid ? "external C6 stopped responding"
                              : "no external C6 fitted",
                sizeof(out->detail));
        return;
    }

    out->state   = SCAN_RUNNING;
    out->reports = s.wifi + s.ble + s.zigbee;
    snprintf(out->detail, sizeof(out->detail), "ext C6 ch%u %s%s%s",
             (unsigned)s.hello.channel,
             (s.hello.roles & C6EXT_ROLE_WIFI)   ? "W" : "",
             (s.hello.roles & C6EXT_ROLE_BLE)    ? "B" : "",
             (s.hello.roles & C6EXT_ROLE_ZIGBEE) ? "Z" : "");
}

esp_err_t c6ext_link_set_roles(uint8_t roles)
{
    uint8_t f[6];
    f[0] = C6EXT_MAGIC0;
    f[1] = C6EXT_MAGIC1;
    f[2] = sizeof(c6ext_set_role_t);
    f[3] = C6EXT_MSG_SET_ROLE;
    f[4] = roles;
    f[5] = c6ext_crc8(&f[3], 2);

    int n = uart_write_bytes(BSP_C6EXT_UART_PORT, f, sizeof(f));
    if (n != (int)sizeof(f)) {
        return ESP_FAIL;
    }
    s.roles = roles;
    return ESP_OK;
}
