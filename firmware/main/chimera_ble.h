/*
 * chimera_ble.h -- ChimeraBLE security research toolkit.
 *
 * Six operational modes for BLE security analysis against university-owned
 * test equipment inside a Faraday cage:
 *
 *   1. SCAN        Enhanced passive scanning with deep advert parsing
 *   2. FINGERPRINT Device type classification and manufacturer identification
 *   3. GATT_ENUM   Service/characteristic/descriptor enumeration
 *   4. MITM        Pairing interception and session relay
 *   5. CLONE       iBeacon/Eddystone/HID identity replication
 *   6. KEYSTROKE   HID-over-BLE report parsing and input recovery
 *
 * All modes share the NimBLE stack that scan_ble.c has already initialised.
 * The toolkit pauses passive scanning while an active mode owns the radio,
 * and resumes it when done.
 *
 * UALR Cybersecurity Lab -- authorized research under professor oversight.
 * Faraday cage RF containment. All targets are university-owned inventory.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "store.h"          /* detection_t, det_kind_t */
#include "c6ext_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  Mode enumeration                                                   *
 * ------------------------------------------------------------------ */

typedef enum {
    CHIMERA_MODE_IDLE = 0,      /* passive scan running (normal wardrive) */
    CHIMERA_MODE_SCAN,          /* enhanced scan with deep advert parse   */
    CHIMERA_MODE_FINGERPRINT,   /* device classification & mfr analysis   */
    CHIMERA_MODE_GATT_ENUM,     /* connect + service/char/desc discovery  */
    CHIMERA_MODE_MITM,          /* pairing intercept and relay            */
    CHIMERA_MODE_CLONE,         /* beacon / HID identity replication      */
    CHIMERA_MODE_KEYSTROKE,     /* HID report parsing and key recovery    */
    CHIMERA_MODE_COUNT
} chimera_mode_t;

/* ------------------------------------------------------------------ *
 *  Common status                                                      *
 * ------------------------------------------------------------------ */

typedef struct {
    chimera_mode_t mode;
    bool           active;          /* true while a mode task is running   */
    uint32_t       events;          /* mode-specific event counter         */
    char           status[128];     /* human-readable for the UI           */
    char           detail[256];     /* extended detail / last result       */
} chimera_status_t;

/* ------------------------------------------------------------------ *
 *  Enhanced scan result (mode 1)                                      *
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    uint8_t  event_type;        /* ADV_IND, ADV_DIRECT_IND, etc.       */
    bool     connectable;
    bool     scannable;

    /* Parsed advert fields */
    char     name[33];
    uint16_t company_id;        /* from manufacturer-specific data     */
    uint8_t  mfg_data[64];
    uint8_t  mfg_data_len;
    uint16_t appearance;
    int8_t   tx_power;          /* TX power level, 127 = not present   */

    /* Service UUIDs found */
    uint16_t svc_uuid16[8];
    uint8_t  svc_uuid16_count;
    uint8_t  svc_uuid128[16];   /* first 128-bit UUID, if any          */
    bool     has_svc_uuid128;

    /* iBeacon */
    bool     is_ibeacon;
    uint8_t  ibeacon_uuid[16];
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    int8_t   ibeacon_tx_power;

    /* Eddystone */
    bool     is_eddystone;
    uint8_t  eddystone_frame_type;  /* 0x00=UID, 0x10=URL, 0x20=TLM  */
    uint8_t  eddystone_data[20];
    uint8_t  eddystone_data_len;

    uint32_t sighting_count;
    int64_t  first_seen_us;
    int64_t  last_seen_us;
} chimera_scan_result_t;

typedef struct {
    char    name[33];
    int8_t  rssi;
    bool    is_ibeacon;
    bool    is_eddystone;
} chimera_scan_item_t;

/* ------------------------------------------------------------------ *
 *  Fingerprint result (mode 2)                                        *
 * ------------------------------------------------------------------ */

typedef enum {
    CHIMERA_DEV_UNKNOWN = 0,
    CHIMERA_DEV_PHONE,
    CHIMERA_DEV_TABLET,
    CHIMERA_DEV_LAPTOP,
    CHIMERA_DEV_WATCH,
    CHIMERA_DEV_FITNESS_TRACKER,
    CHIMERA_DEV_HEADPHONES,
    CHIMERA_DEV_SPEAKER,
    CHIMERA_DEV_KEYBOARD,
    CHIMERA_DEV_MOUSE,
    CHIMERA_DEV_GAMEPAD,
    CHIMERA_DEV_BEACON,
    CHIMERA_DEV_SMART_LOCK,
    CHIMERA_DEV_MEDICAL,
    CHIMERA_DEV_SMART_HOME,
    CHIMERA_DEV_TV,
    CHIMERA_DEV_CAR,
    CHIMERA_DEV_PRINTER,
    CHIMERA_DEV_CAMERA,
    CHIMERA_DEV_TOY,
    CHIMERA_DEV_HID_GENERIC,
    CHIMERA_DEV_TYPE_COUNT
} chimera_dev_type_t;

typedef struct {
    uint8_t           addr[6];
    uint8_t           addr_type;
    int8_t            rssi;
    char              name[33];

    chimera_dev_type_t dev_type;
    const char        *dev_type_str;     /* human readable             */
    const char        *manufacturer;     /* looked up from company ID  */
    uint16_t           company_id;
    uint16_t           appearance;

    /* BLE version heuristics from features/PDU analysis */
    uint8_t           ble_version_guess; /* 4 = 4.0, 5 = 5.0, etc.    */
    bool              supports_2m_phy;
    bool              supports_coded_phy;
    bool              supports_ext_adv;

    /* Security posture */
    bool              connectable;
    bool              is_random_addr;    /* random vs public address   */
    bool              uses_irk;          /* resolvable private addr    */

    float             confidence;        /* 0.0..1.0 classification    */
} chimera_fingerprint_t;

/* ------------------------------------------------------------------ *
 *  GATT enumeration result (mode 3)                                   *
 * ------------------------------------------------------------------ */

#define CHIMERA_MAX_SERVICES        32
#define CHIMERA_MAX_CHARS_PER_SVC   16
#define CHIMERA_MAX_DESCS_PER_CHAR  8
#define CHIMERA_MAX_VALUE_LEN       256

typedef struct {
    uint16_t handle;
    uint8_t  uuid128[16];
    uint16_t uuid16;            /* 0 if 128-bit only                   */
    char     name[48];          /* resolved name or "Unknown"          */
} chimera_descriptor_t;

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint8_t  uuid128[16];
    uint16_t uuid16;
    uint8_t  properties;        /* BLE GATT property bits              */
    char     name[48];

    /* Value, if readable */
    uint8_t  value[CHIMERA_MAX_VALUE_LEN];
    uint16_t value_len;
    bool     value_read_ok;

    chimera_descriptor_t descs[CHIMERA_MAX_DESCS_PER_CHAR];
    uint8_t  desc_count;
} chimera_characteristic_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    uint8_t  uuid128[16];
    uint16_t uuid16;
    char     name[48];

    chimera_characteristic_t chars[CHIMERA_MAX_CHARS_PER_SVC];
    uint8_t  char_count;
} chimera_service_t;

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    bool     connected;
    bool     enum_complete;
    uint16_t mtu;

    chimera_service_t services[CHIMERA_MAX_SERVICES];
    uint8_t  service_count;
    char     error[64];
} chimera_gatt_result_t;

typedef struct {
    uint8_t  addr[6];
    bool     connected;
    bool     enum_complete;
    uint16_t mtu;
    uint8_t  service_count;
} chimera_gatt_summary_t;

/* ------------------------------------------------------------------ *
 *  MITM proxy (mode 4)                                                *
 * ------------------------------------------------------------------ */

typedef enum {
    CHIMERA_PAIR_JUST_WORKS = 0,
    CHIMERA_PAIR_PASSKEY_ENTRY,
    CHIMERA_PAIR_NUMERIC_COMPARISON,
    CHIMERA_PAIR_OOB,
    CHIMERA_PAIR_UNKNOWN
} chimera_pairing_method_t;

typedef enum {
    CHIMERA_MITM_IDLE = 0,
    CHIMERA_MITM_SCANNING,
    CHIMERA_MITM_WAITING_CLIENT,
    CHIMERA_MITM_RELAYING,
    CHIMERA_MITM_PAIRING,
    CHIMERA_MITM_PAIRED,
    CHIMERA_MITM_ERROR,
} chimera_mitm_state_t;

typedef struct {
    chimera_mitm_state_t   state;
    chimera_pairing_method_t pairing;
    uint8_t  target_addr[6];
    uint8_t  client_addr[6];
    bool     target_connected;
    bool     client_connected;
    uint32_t relayed_packets;
    uint32_t intercepted_keys;  /* pairing keys captured               */
    uint32_t passkey;           /* captured passkey, if applicable      */
    char     status[128];
} chimera_mitm_status_t;

/* ------------------------------------------------------------------ *
 *  Clone (mode 5)                                                     *
 * ------------------------------------------------------------------ */

typedef enum {
    CHIMERA_CLONE_IBEACON = 0,
    CHIMERA_CLONE_EDDYSTONE_UID,
    CHIMERA_CLONE_EDDYSTONE_URL,
    CHIMERA_CLONE_HID_KEYBOARD,
    CHIMERA_CLONE_HID_MOUSE,
    CHIMERA_CLONE_GENERIC,
    CHIMERA_CLONE_TYPE_COUNT
} chimera_clone_type_t;

typedef struct {
    chimera_clone_type_t type;
    uint8_t  source_addr[6];    /* device being cloned                 */
    bool     broadcasting;

    /* iBeacon clone params */
    uint8_t  ibeacon_uuid[16];
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    int8_t   ibeacon_tx_power;

    /* Eddystone clone params */
    uint8_t  eddystone_namespace[10];
    uint8_t  eddystone_instance[6];
    char     eddystone_url[18];

    /* Generic / HID: raw advert data to replay */
    uint8_t  adv_data[31];
    uint8_t  adv_data_len;
    uint8_t  scan_rsp_data[31];
    uint8_t  scan_rsp_len;

    char     name[33];
    char     status[128];
} chimera_clone_state_t;

/* ------------------------------------------------------------------ *
 *  Keystroke decode (mode 6)                                          *
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t  modifier;          /* HID modifier byte                   */
    uint8_t  keycode;           /* HID usage ID                        */
    char     ascii;             /* decoded ASCII, 0 if not printable   */
    bool     key_down;          /* true = press, false = release       */
    int64_t  timestamp_us;
} chimera_keystroke_t;

#define CHIMERA_KEY_BUF_SIZE  1024

typedef struct {
    uint8_t  target_addr[6];
    bool     connected;
    bool     subscribed;        /* notifications enabled on HID report */
    uint32_t reports_received;
    uint32_t keys_decoded;

    /* Rolling buffer of decoded keystrokes */
    chimera_keystroke_t keys[CHIMERA_KEY_BUF_SIZE];
    uint16_t key_head;          /* write pointer (ring buffer)         */

    /* Plaintext reconstruction */
    char     text[CHIMERA_KEY_BUF_SIZE];
    uint16_t text_len;

    char     status[128];
} chimera_keystroke_state_t;

typedef struct {
    uint8_t  target_addr[6];
    bool     connected;
    bool     subscribed;
    uint32_t reports_received;
    uint32_t keys_decoded;
    char     recent_text[96];
} chimera_keystroke_summary_t;

/* ------------------------------------------------------------------ *
 *  Callbacks                                                          *
 * ------------------------------------------------------------------ */

typedef void (*chimera_scan_cb_t)(const chimera_scan_result_t *result);
typedef void (*chimera_fingerprint_cb_t)(const chimera_fingerprint_t *fp);
typedef void (*chimera_keystroke_cb_t)(const chimera_keystroke_t *key);

/* ------------------------------------------------------------------ *
 *  Logging                                                            *
 * ------------------------------------------------------------------ */

/* ChimeraBLE writes its own CSV log alongside the wardrive session.
 * Format matches the existing sdlog conventions. */
typedef struct {
    uint32_t events_logged;
    uint32_t events_dropped;
    char     path[64];
} chimera_log_status_t;

/* ------------------------------------------------------------------ *
 *  API                                                                *
 * ------------------------------------------------------------------ */

/* Initialise. Must be called after scan_ble_start(). */
esp_err_t chimera_init(void);

/* Get current mode and status. */
void chimera_get_status(chimera_status_t *out);

/* --- Mode 1: Enhanced Scan ---------------------------------------- */

/* Start enhanced scanning. Pauses normal passive scan and enables deep
 * advertisement parsing. Callback fires for each advertisement. */
esp_err_t chimera_scan_start(chimera_scan_cb_t cb);
esp_err_t chimera_scan_stop(void);

/* Snapshot of discovered devices, sorted by RSSI. */
size_t chimera_scan_snapshot(chimera_scan_result_t *out, size_t max_out);

/* Lightweight scan top items for UI displays (small stack footprint). */
size_t chimera_scan_get_top(chimera_scan_item_t *out, size_t max_out);

/* --- Mode 2: Fingerprint ------------------------------------------ */

/* Fingerprint a specific device by address, or all visible devices if
 * addr is NULL. Results arrive via callback. */
esp_err_t chimera_fingerprint_start(const uint8_t addr[6],
                                     chimera_fingerprint_cb_t cb);
esp_err_t chimera_fingerprint_stop(void);

/* Get the most recent fingerprint for a device. */
bool chimera_fingerprint_get(const uint8_t addr[6],
                              chimera_fingerprint_t *out);

/* --- Mode 3: GATT Enumerate --------------------------------------- */

/* Connect to a device and enumerate all services, characteristics,
 * and descriptors. Reads readable values. Non-blocking; poll result. */
esp_err_t chimera_gatt_enum_start(const uint8_t addr[6], uint8_t addr_type);
esp_err_t chimera_gatt_enum_stop(void);

/* Get the current enumeration result. */
bool chimera_gatt_enum_get(chimera_gatt_result_t *out);

/* Lightweight GATT summary for UI displays (small stack footprint). */
bool chimera_gatt_get_summary(chimera_gatt_summary_t *out);

/* Read a specific characteristic by handle. */
esp_err_t chimera_gatt_read(uint16_t handle, uint8_t *buf, uint16_t *len);

/* Write to a characteristic by handle. */
esp_err_t chimera_gatt_write(uint16_t handle, const uint8_t *data,
                              uint16_t len);

/* --- Mode 4: MITM Proxy ------------------------------------------- */

/* Begin MITM: connect to target as central, then advertise as the target
 * to intercept the client's pairing and relay traffic. */
esp_err_t chimera_mitm_start(const uint8_t target_addr[6],
                              uint8_t target_addr_type);
esp_err_t chimera_mitm_stop(void);
void      chimera_mitm_get_status(chimera_mitm_status_t *out);

/* --- Mode 5: Clone ------------------------------------------------ */

/* Clone a previously scanned beacon or HID device. The clone_state
 * must be populated with the source parameters. */
esp_err_t chimera_clone_start(const chimera_clone_state_t *params);
esp_err_t chimera_clone_stop(void);
void      chimera_clone_get_state(chimera_clone_state_t *out);

/* Helper: capture an iBeacon's identity for later cloning. */
esp_err_t chimera_clone_capture_ibeacon(const uint8_t addr[6],
                                         chimera_clone_state_t *out);

/* Helper: capture an Eddystone UID for later cloning. */
esp_err_t chimera_clone_capture_eddystone(const uint8_t addr[6],
                                           chimera_clone_state_t *out);

/* --- Mode 6: Keystroke Decode ------------------------------------- */

/* Connect to a BLE HID keyboard, subscribe to its report characteristic,
 * and decode incoming key reports. */
esp_err_t chimera_keystroke_start(const uint8_t addr[6],
                                   uint8_t addr_type,
                                   chimera_keystroke_cb_t cb);
esp_err_t chimera_keystroke_stop(void);
void      chimera_keystroke_get_state(chimera_keystroke_state_t *out);

/* Lightweight keystroke summary for UI displays (small stack footprint). */
bool      chimera_keystroke_get_summary(chimera_keystroke_summary_t *out);

/* Clear the captured text buffer. */
void chimera_keystroke_clear(void);

/* --- External C6 Integration -------------------------------------- */
void chimera_ingest_c6ext_ble(const c6ext_ble_t *b);

/* --- Logging ------------------------------------------------------ */

void chimera_log_status(chimera_log_status_t *out);

#ifdef __cplusplus
}
#endif
