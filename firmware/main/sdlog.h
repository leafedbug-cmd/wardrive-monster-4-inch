/*
 * sdlog.h -- CSV detection log on the P4's own SDIO card slot.
 *
 * Deliberately NOT the SD slot on the display: that one shares the LCD SPI
 * bus. See docs/WIRING.md for the reasoning.
 *
 * Writes are queued and drained by a background task so a scanner callback
 * never blocks on the filesystem.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     mounted;
    uint64_t card_size_mb;
    uint64_t free_mb;
    uint32_t written;       /* rows committed                      */
    uint32_t dropped;       /* rows lost to a full queue           */
    uint32_t bytes;
    char     path[64];      /* active session file                 */
} sdlog_status_t;

/* Mounts the card and opens a new session file. Safe to call with no card
 * inserted -- returns an error and the rest of the API becomes a no-op. */
esp_err_t sdlog_init(void);

/* Queue one detection. Non-blocking; increments `dropped` if the queue is
 * full. Call this only for genuinely new devices. */
void sdlog_submit(const detection_t *det);

/* Force the current buffer to the card. Called periodically by the logger
 * task; also worth calling before a deliberate reboot. */
void sdlog_flush(void);

void sdlog_status(sdlog_status_t *out);

/* True once a card is mounted and a session file is open. */
bool sdlog_ready(void);

#ifdef __cplusplus
}
#endif
