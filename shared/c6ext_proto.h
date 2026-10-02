/*
 * c6ext_proto.h -- the wire protocol between the P4 and the external
 * ESP32-C6 (Seeed XIAO ESP32C6) acting as a second radio.
 *
 * This file is compiled into BOTH projects:
 *
 *      firmware/        the P4 dashboard  (consumes sightings)
 *      c6ext-firmware/  the XIAO scanner  (produces sightings)
 *
 * Both reference it from this one location, so there is no copy to drift.
 * Change the layout here and you MUST reflash both sides -- see
 * C6EXT_PROTO_VERSION, which the P4 checks on every HELLO.
 *
 * ---------------------------------------------------------------------
 *  Why decoded sightings and not raw packets
 * ---------------------------------------------------------------------
 *  The external C6 reaches the P4 over a 460800 baud UART, which is three
 *  orders of magnitude slower than the on-board C6's SDIO link. That would
 *  be fatal if we shipped packets. We don't: the XIAO decodes locally and
 *  sends ~40-byte summaries, so a busy street is ~1000 sightings/sec of
 *  headroom. The slow link costs us nothing a wardriver cares about.
 *
 * ---------------------------------------------------------------------
 *  Framing, deliberately dumb so a resync costs one frame
 * ---------------------------------------------------------------------
 *      A5 5A  len  type  payload[len]  crc8
 *
 *   len  is the payload length only (0..64), not counting type or crc.
 *   crc8 is CRC-8/ATM (poly 0x07, init 0x00) over type + payload.
 *
 * Little-endian, packed. Both ends are RISC-V, so this is a formality.
 */
#pragma once

#include <stdint.h>

#define C6EXT_MAGIC0            0xA5
#define C6EXT_MAGIC1            0x5A
#define C6EXT_MAX_PAYLOAD       64
#define C6EXT_PROTO_VERSION     1

/* Frame types. C6 -> P4 is 0x0n, P4 -> C6 is 0x1n. */
typedef enum {
    C6EXT_MSG_WIFI     = 0x01,  /* an AP beacon / probe response       */
    C6EXT_MSG_BLE      = 0x02,  /* a BLE advertisement                 */
    C6EXT_MSG_ZIGBEE   = 0x03,  /* a raw 802.15.4 frame                */
    C6EXT_MSG_HELLO    = 0x04,  /* periodic heartbeat + counters       */
    C6EXT_MSG_SET_ROLE = 0x11,  /* P4 reassigns what the XIAO scans    */
} c6ext_msg_t;

/* What the external radio spends its duty cycle on. It has ONE 2.4 GHz
 * front end, exactly like the on-board C6, so these are time-sliced, not
 * concurrent. Enabling everything means each gets a third of the airtime. */
#define C6EXT_ROLE_WIFI         (1u << 0)
#define C6EXT_ROLE_BLE          (1u << 1)
#define C6EXT_ROLE_ZIGBEE       (1u << 2)

typedef struct __attribute__((packed)) {
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;          /* wifi_auth_mode_t, as IDF numbers it  */
    uint8_t  hidden;
    uint8_t  ssid_len;          /* 0..32                                */
    char     ssid[32];          /* NOT nul-terminated; use ssid_len     */
} c6ext_wifi_t;

typedef struct __attribute__((packed)) {
    uint8_t  addr[6];
    int8_t   rssi;
    uint8_t  addr_type;
    uint16_t company;           /* manufacturer-data company ID, 0 none */
    uint8_t  connectable;
    uint8_t  name_len;          /* 0..24                                */
    char     name[24];          /* NOT nul-terminated; use name_len     */
} c6ext_ble_t;

/* `ext_addr` is the source address as an EUI-64 where the frame carried
 * one. Where it carried only a 16-bit short address, the scanner
 * synthesises a stable stand-in -- see c6ext_pack_short() -- so the P4's
 * store can dedup on it either way. */
typedef struct __attribute__((packed)) {
    uint8_t  ext_addr[8];
    uint16_t panid;
    uint8_t  channel;           /* 11..26                               */
    int8_t   rssi;
    uint8_t  lqi;
    uint8_t  frame_type;        /* 0 beacon, 1 data, 2 ack, 3 MAC cmd   */
    uint8_t  short_addr;        /* 1 when ext_addr is synthesised       */
    uint8_t  _pad;
} c6ext_zigbee_t;

/* Heartbeat. The P4 uses its absence to decide the XIAO is missing. */
typedef struct __attribute__((packed)) {
    uint16_t proto_ver;         /* C6EXT_PROTO_VERSION                  */
    uint8_t  roles;             /* C6EXT_ROLE_* bitmask, as configured  */
    uint8_t  channel;           /* what it is listening on right now    */
    uint32_t seen_wifi;         /* counters since boot                  */
    uint32_t seen_ble;
    uint32_t seen_zigbee;
    uint32_t dropped;           /* sightings lost to a full queue       */
    uint8_t  ext_antenna;       /* 1 when the u.FL path was selected    */
    uint8_t  _pad[3];
} c6ext_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t roles;              /* C6EXT_ROLE_* bitmask                 */
} c6ext_set_role_t;

/* A short address has no EUI-64, but the P4's store keys on 6 bytes of
 * MAC. Pack (panid, short addr) into the slot those 6 bytes come from so
 * two devices on different PANs never collide. 0xFFFE leads, because that
 * is the reserved "no extended address" short address and so cannot be
 * the opening bytes of a real EUI-64 here. */
static inline void c6ext_pack_short(uint8_t out[8], uint16_t panid,
                                    uint16_t short_addr)
{
    out[0] = 0xFF;
    out[1] = 0xFE;
    out[2] = (uint8_t)(panid >> 8);
    out[3] = (uint8_t)(panid & 0xFF);
    out[4] = 0x00;
    out[5] = 0x00;
    out[6] = (uint8_t)(short_addr >> 8);
    out[7] = (uint8_t)(short_addr & 0xFF);
}

/* CRC-8/ATM. Small enough that a table would cost more than it saves. */
static inline uint8_t c6ext_crc8(const uint8_t *p, uint32_t n)
{
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}
