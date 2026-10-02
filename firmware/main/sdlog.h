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
    bool     session_active;
    bool     live_only;        /* session without SD logging */
    uint64_t card_size_mb;
    uint64_t free_mb;
    uint32_t written;          /* rows committed in current session */
    uint32_t dropped;          /* rows lost to a full queue */
    uint32_t bytes;
    int64_t  session_start_us;
    char     path[64];         /* active session file */
    char     error[48];        /* last error message if any */
} sdlog_status_t;

/* Mounts the card and starts logger queue/task. Does NOT start logging.
 * Safe to call with no card inserted -- returns an error and permits
 * live-only operation. */
esp_err_t sdlog_init(void);

/* Explicit survey session controls: */
esp_err_t sdlog_session_start(bool live_only);
void      sdlog_session_stop(void);
bool      sdlog_session_is_active(void);
uint32_t  sdlog_session_seq(void);

/* Queue one detection. Non-blocking; dropped if no session active or queue full. */
void sdlog_submit(const detection_t *det);

/* Force the current buffer to the card. */
void sdlog_flush(void);

void sdlog_status(sdlog_status_t *out);

/* True if an SD card is physically mounted. */
bool sdlog_ready(void);

#ifdef __cplusplus
}
#endif
