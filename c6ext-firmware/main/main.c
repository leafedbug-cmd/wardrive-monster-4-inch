/*
 * Wardrive Monster -- external ESP32-C6 scanner (Seeed XIAO ESP32C6).
 *
 * This is NOT an ESP-Hosted slave. The board's on-board C6 is that, and it
 * talks SDIO. This chip is a standalone second radio hanging off a UART,
 * and its whole job is: listen, decode, and report finished sightings to
 * the P4 in the format shared/c6ext_proto.h defines.
 *
 * Why that division of labour:
 *
 *   - The on-board C6 has ONE 2.4 GHz front end shared by Wi-Fi, BLE and
 *     802.15.4, so every protocol steals airtime from the others. A second
 *     chip is a second front end, and this one has the external antenna.
 *
 *   - 802.15.4 is impossible on the on-board C6 here at all: the RCP role
 *     is built with Wi-Fi off, and the spinel UART it needs is on unrouted
 *     pads. See docs/SPEC-ESP32-P4-C6.md section 6.
 *
 * One front end here too, so the roles below are TIME-SLICED, not
 * concurrent. The scheduler is a plain round robin; a phase owns the radio
 * outright, and the other stacks are stopped while it does.
 */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "c6ext_proto.h"

static const char *TAG = "c6ext";

/* ------------------------------------------------------------------ *
 *  Wiring -- see docs/WIRING.md                                       *
 *                                                                     *
 *  XIAO D6 == GPIO16 == our TX  -> P4 GPIO27 (hdr 38)                 *
 *  XIAO D7 == GPIO17 == our RX  <- P4 GPIO47 (hdr 37)                 *
 *                                                                     *
 *  Those are also UART0's default pins on the C6, which is why the    *
 *  console is moved to USB Serial/JTAG in sdkconfig.defaults. Plug a   *
 *  USB-C cable in for logs; the data link keeps the pads to itself.   *
 * ------------------------------------------------------------------ */
#define LINK_UART       UART_NUM_1
#define LINK_PIN_TX     GPIO_NUM_16     /* D6 */
#define LINK_PIN_RX     GPIO_NUM_17     /* D7 */
#define LINK_BAUD       460800

/* The XIAO does not switch to its u.FL connector on its own, and the two
 * pins are easy to get the wrong way round. Per Seeed's wiki:
 *
 *      GPIO3  LOW   activates RF switch control   (required first)
 *      GPIO14 HIGH  selects the external u.FL antenna
 *             LOW   selects the onboard ceramic antenna (power-on default)
 *
 * Getting this inverted is silent: the radio works fine on the ceramic
 * antenna and nothing reports an error, you just never get the range you
 * soldered the connector on for. */
#define ANT_PIN_ENABLE  GPIO_NUM_3
#define ANT_PIN_SELECT  GPIO_NUM_14

/* Phase lengths. Wi-Fi gets the longest slice because a passive sweep of
 * 14 channels cannot usefully be cut short, and because the external
 * antenna helps Wi-Fi more than anything else here. */
#define PHASE_WIFI_MS       4000
#define PHASE_ZIGBEE_MS     3000
#define ZB_DWELL_MS         180     /* per 802.15.4 channel              */
#define HELLO_PERIOD_MS     1000

#define ZB_CHAN_LO          11
#define ZB_CHAN_HI          26

#define SIGHT_Q_LEN         64

/* ------------------------------------------------------------------ *
 *  State                                                              *
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[C6EXT_MAX_PAYLOAD];
} queued_t;

static QueueHandle_t s_q;

static struct {
    volatile uint8_t  roles;
    volatile uint8_t  channel;
    volatile uint32_t seen_wifi, seen_ble, seen_zigbee, dropped;
    volatile uint32_t tx_frames, tx_bytes;
    bool              ext_antenna;
} s;

/* ------------------------------------------------------------------ *
 *  Link TX                                                            *
 * ------------------------------------------------------------------ */

static void link_send(uint8_t type, const void *payload, uint8_t len)
{
    uint8_t hdr[4] = { C6EXT_MAGIC0, C6EXT_MAGIC1, len, type };
    uint8_t crc;

    /* CRC covers type + payload, in that order. */
    crc = c6ext_crc8(&type, 1);
    for (uint8_t i = 0; i < len; i++) {
        crc ^= ((const uint8_t *)payload)[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }

    int n = 0;
    n += uart_write_bytes(LINK_UART, hdr, sizeof(hdr));
    if (len) {
        n += uart_write_bytes(LINK_UART, payload, len);
    }
    n += uart_write_bytes(LINK_UART, &crc, 1);

    s.tx_frames++;
    if (n > 0) {
        s.tx_bytes += (uint32_t)n;
    }
}

/* Called from radio callbacks, so it must never block. A full queue drops
 * the sighting and bumps a counter the P4 can see in the heartbeat --
 * silently losing them would make the dashboard lie. */
static void emit(uint8_t type, const void *payload, uint8_t len)
{
    queued_t q;
    q.type = type;
    q.len  = len;
    memcpy(q.payload, payload, len);

    if (xQueueSend(s_q, &q, 0) != pdTRUE) {
        s.dropped++;
    }
}

static void emit_from_isr(uint8_t type, const void *payload, uint8_t len)
{
    queued_t q;
    BaseType_t woke = pdFALSE;

    q.type = type;
    q.len  = len;
    memcpy(q.payload, payload, len);

    if (xQueueSendFromISR(s_q, &q, &woke) != pdTRUE) {
        s.dropped++;
    }
    if (woke) {
        portYIELD_FROM_ISR();
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    queued_t q;

    for (;;) {
        if (xQueueReceive(s_q, &q, portMAX_DELAY) == pdTRUE) {
            link_send(q.type, q.payload, q.len);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Role: Wi-Fi                                                        *
 *                                                                     *
 *  A passive scan, for the same reason the P4 uses one: wardriving     *
 *  observes, it does not transmit probe requests.                      *
 * ------------------------------------------------------------------ */

static void wifi_phase(void)
{
    wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,            /* all channels */
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time   = { .passive = 120 },
    };

    ESP_ERROR_CHECK(esp_wifi_start());

    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        esp_wifi_stop();
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        esp_wifi_stop();
        return;
    }

    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        esp_wifi_scan_get_ap_records(&n, NULL);   /* free driver-side */
        esp_wifi_stop();
        return;
    }

    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            c6ext_wifi_t w = {0};
            memcpy(w.bssid, recs[i].bssid, 6);
            w.rssi     = recs[i].rssi;
            w.channel  = recs[i].primary;
            w.authmode = (uint8_t)recs[i].authmode;

            size_t sl = strnlen((const char *)recs[i].ssid,
                                sizeof(recs[i].ssid));
            if (sl > sizeof(w.ssid)) {
                sl = sizeof(w.ssid);
            }
            w.ssid_len = (uint8_t)sl;
            w.hidden   = (sl == 0);
            memcpy(w.ssid, recs[i].ssid, sl);

            s.seen_wifi++;
            emit(C6EXT_MSG_WIFI, &w, sizeof(w));
        }
    }

    free(recs);
    esp_wifi_stop();
}

/* ------------------------------------------------------------------ *
 *  Role: 802.15.4 / Zigbee                                            *
 * ------------------------------------------------------------------ */

/* Parse enough of an 802.15.4 MAC header to get the source address and
 * PAN. Frame control field, little-endian:
 *
 *   bits 0-2   frame type (0 beacon, 1 data, 2 ack, 3 MAC command)
 *   bit  6     PAN ID compression
 *   bits 10-11 destination addressing mode
 *   bits 14-15 source addressing mode
 *
 * Addressing modes: 0 absent, 2 short (16-bit), 3 extended (64-bit).
 *
 * This implements the 2003/2006 field ordering, which is what Zigbee
 * actually puts on the air. Frame version 2 (802.15.4-2015) reorders
 * things, so those are skipped rather than mis-parsed into fiction. */
static bool parse_802154(const uint8_t *psdu, uint8_t len,
                         c6ext_zigbee_t *out)
{
    if (len < 3) {
        return false;
    }

    uint16_t fcf = (uint16_t)psdu[0] | ((uint16_t)psdu[1] << 8);
    uint8_t  ftype    = fcf & 0x07;
    bool     pan_comp = (fcf >> 6) & 0x01;
    uint8_t  dst_mode = (fcf >> 10) & 0x03;
    uint8_t  version  = (fcf >> 12) & 0x03;
    uint8_t  src_mode = (fcf >> 14) & 0x03;

    if (version >= 2) {
        return false;   /* 2015 framing: different layout, do not guess */
    }
    if (ftype == 2) {
        return false;   /* acks carry no addressing worth logging       */
    }
    if (src_mode == 0) {
        return false;   /* nothing to identify the sender by            */
    }

    uint8_t p = 3;      /* past FCF (2) + sequence number (1)           */

    uint16_t dst_pan = 0;
    if (dst_mode != 0) {
        if (len < p + 2) { return false; }
        dst_pan = (uint16_t)psdu[p] | ((uint16_t)psdu[p + 1] << 8);
        p += 2;
        p += (dst_mode == 3) ? 8 : 2;
        if (len < p) { return false; }
    }

    uint16_t src_pan;
    if (pan_comp && dst_mode != 0) {
        src_pan = dst_pan;          /* compressed: shares the dest PAN  */
    } else {
        if (len < p + 2) { return false; }
        src_pan = (uint16_t)psdu[p] | ((uint16_t)psdu[p + 1] << 8);
        p += 2;
    }

    if (src_mode == 3) {
        if (len < p + 8) { return false; }
        /* On the air an EUI-64 is little-endian; store it big-endian so
         * the P4 prints it the way the label on the device reads. */
        for (int i = 0; i < 8; i++) {
            out->ext_addr[i] = psdu[p + 7 - i];
        }
        out->short_addr = 0;
    } else {
        if (len < p + 2) { return false; }
        uint16_t sa = (uint16_t)psdu[p] | ((uint16_t)psdu[p + 1] << 8);
        c6ext_pack_short(out->ext_addr, src_pan, sa);
        out->short_addr = 1;
    }

    out->panid      = src_pan;
    out->frame_type = ftype;
    return true;
}

/* IDF calls this from the 802.15.4 driver context. Decode and queue only;
 * the UART write happens on tx_task. */
void esp_ieee802154_receive_done(uint8_t *frame,
                                 esp_ieee802154_frame_info_t *frame_info)
{
    if (frame && frame_info && frame[0] >= 2) {
        c6ext_zigbee_t z = {0};
        /* frame[0] is the PHY length byte; the PSDU follows, and the last
         * two bytes of it are the FCS. */
        uint8_t psdu_len = frame[0] > 2 ? (uint8_t)(frame[0] - 2) : 0;

        if (parse_802154(&frame[1], psdu_len, &z)) {
            z.channel = frame_info->channel;
            z.rssi    = frame_info->rssi;
            z.lqi     = frame_info->lqi;
            s.seen_zigbee++;
            emit_from_isr(C6EXT_MSG_ZIGBEE, &z, sizeof(z));
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

static void zigbee_phase(void)
{
    ESP_ERROR_CHECK(esp_ieee802154_enable());
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);

    int64_t spent = 0;
    for (uint8_t ch = ZB_CHAN_LO; ch <= ZB_CHAN_HI && spent < PHASE_ZIGBEE_MS;
         ch++) {
        esp_ieee802154_set_channel(ch);
        s.channel = ch;
        esp_ieee802154_receive();
        vTaskDelay(pdMS_TO_TICKS(ZB_DWELL_MS));
        spent += ZB_DWELL_MS;
    }

    esp_ieee802154_disable();
}

/* ------------------------------------------------------------------ *
 *  Link RX -- the P4 reassigning roles                                *
 * ------------------------------------------------------------------ */

static void apply_roles(uint8_t roles)
{
    /* BLE is deliberately not implemented here. The on-board C6 already
     * runs a working NimBLE observer over SDIO, and adding a third
     * claimant to THIS chip's single front end would cost Wi-Fi and
     * 802.15.4 airtime to duplicate something that already works. The
     * protocol carries the bit so a later build can honour it; this one
     * reports back only what it actually does. */
    s.roles = roles & (C6EXT_ROLE_WIFI | C6EXT_ROLE_ZIGBEE);

    if (roles & C6EXT_ROLE_BLE) {
        ESP_LOGW(TAG, "BLE role requested but not built into this firmware; "
                      "the on-board C6 covers BLE over SDIO");
    }
    ESP_LOGI(TAG, "roles now: %s%s",
             (s.roles & C6EXT_ROLE_WIFI)   ? "wifi " : "",
             (s.roles & C6EXT_ROLE_ZIGBEE) ? "zigbee" : "");
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t  byte;
    enum { W_M0, W_M1, W_LEN, W_TYPE, W_PAYLOAD, W_CRC } st = W_M0;
    uint8_t  len = 0, type = 0, idx = 0, buf[C6EXT_MAX_PAYLOAD];

    for (;;) {
        if (uart_read_bytes(LINK_UART, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }

        switch (st) {
        case W_M0: if (byte == C6EXT_MAGIC0) { st = W_M1; } break;
        case W_M1:
            if (byte == C6EXT_MAGIC1)      { st = W_LEN; }
            else if (byte == C6EXT_MAGIC0) { st = W_M1; }
            else                           { st = W_M0; }
            break;
        case W_LEN:
            if (byte > C6EXT_MAX_PAYLOAD) { st = W_M0; }
            else                          { len = byte; st = W_TYPE; }
            break;
        case W_TYPE: type = byte; idx = 0; st = len ? W_PAYLOAD : W_CRC; break;
        case W_PAYLOAD:
            buf[idx++] = byte;
            if (idx >= len) { st = W_CRC; }
            break;
        case W_CRC: {
            uint8_t crc = c6ext_crc8(&type, 1);
            for (uint8_t i = 0; i < len; i++) {
                crc ^= buf[i];
                for (int b = 0; b < 8; b++) {
                    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                                       : (uint8_t)(crc << 1);
                }
            }
            if (crc == byte && type == C6EXT_MSG_SET_ROLE &&
                len == sizeof(c6ext_set_role_t)) {
                apply_roles(((const c6ext_set_role_t *)buf)->roles);
            }
            st = W_M0;
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Heartbeat                                                          *
 * ------------------------------------------------------------------ */

static void hello_task(void *arg)
{
    (void)arg;
    uint32_t ticks = 0;
    for (;;) {
        c6ext_hello_t h = {
            .proto_ver   = C6EXT_PROTO_VERSION,
            .roles       = s.roles,
            .channel     = s.channel,
            .seen_wifi   = s.seen_wifi,
            .seen_ble    = s.seen_ble,
            .seen_zigbee = s.seen_zigbee,
            .dropped     = s.dropped,
            .ext_antenna = s.ext_antenna ? 1 : 0,
        };
        emit(C6EXT_MSG_HELLO, &h, sizeof(h));

        /* Say out loud what we have pushed at the wire. If the P4 reports
         * no bytes while this keeps climbing, the firmware is fine and the
         * fault is the cable, the ground, or the pin it lands on. */
        if (++ticks % 5 == 0) {
            ESP_LOGI(TAG, "tx %lu frames / %lu bytes on gpio%d; "
                          "wifi %lu zigbee %lu dropped %lu",
                     (unsigned long)s.tx_frames, (unsigned long)s.tx_bytes,
                     (int)LINK_PIN_TX,
                     (unsigned long)s.seen_wifi, (unsigned long)s.seen_zigbee,
                     (unsigned long)s.dropped);
        }
        vTaskDelay(pdMS_TO_TICKS(HELLO_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ *
 *  Setup                                                              *
 * ------------------------------------------------------------------ */

static void antenna_select_external(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ANT_PIN_ENABLE) | (1ULL << ANT_PIN_SELECT),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(ANT_PIN_ENABLE, 0);   /* GPIO3  low  = switch enabled */
    gpio_set_level(ANT_PIN_SELECT, 1);   /* GPIO14 high = external u.FL  */
    s.ext_antenna = true;

    ESP_LOGI(TAG, "RF switch set to the external antenna "
                  "(GPIO%d low enables, GPIO%d high selects u.FL)",
             (int)ANT_PIN_ENABLE, (int)ANT_PIN_SELECT);
}

static void link_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 1024, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_PIN_TX, LINK_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Started and stopped per phase -- the 802.15.4 phase needs the front
     * end to itself, and leaving Wi-Fi running would fight it. */
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    antenna_select_external();
    link_init();
    wifi_init();

    s_q = xQueueCreate(SIGHT_Q_LEN, sizeof(queued_t));
    ESP_ERROR_CHECK(s_q ? ESP_OK : ESP_ERR_NO_MEM);

    apply_roles(C6EXT_ROLE_WIFI | C6EXT_ROLE_ZIGBEE);

    xTaskCreate(tx_task,    "c6ext_tx",    3072, NULL, 6, NULL);
    xTaskCreate(rx_task,    "c6ext_rx",    3072, NULL, 5, NULL);
    xTaskCreate(hello_task, "c6ext_hello", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "external scanner up: proto v%d, %d baud to the P4",
             C6EXT_PROTO_VERSION, LINK_BAUD);

    /* Round robin. Each phase owns the radio for its slice; a role that is
     * switched off simply never gets one, giving the remaining role the
     * whole duty cycle. */
    for (;;) {
        if (s.roles & C6EXT_ROLE_WIFI) {
            wifi_phase();
        }
        if (s.roles & C6EXT_ROLE_ZIGBEE) {
            zigbee_phase();
        }
        if (s.roles == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
