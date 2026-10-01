/*
 * c6_ota.h -- flash the ESP32-C6 co-processor over the existing SDIO link.
 *
 * No soldering, no second USB cable: drop a slave image on the SD card as
 * /sdcard/c6_slave.bin and the P4 streams it to the C6 through esp_hosted.
 *
 * This is the only practical route on this board, because the C6's UART is
 * on unrouted pads (see docs/SPEC-ESP32-P4-C6.md section 4).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    C6_OTA_IDLE = 0,
    C6_OTA_CHECKING,
    C6_OTA_ERASING,
    C6_OTA_WRITING,
    C6_OTA_FINALISING,
    C6_OTA_DONE,
    C6_OTA_FAILED,
} c6_ota_state_t;

typedef struct {
    c6_ota_state_t state;
    uint8_t        percent;
    uint32_t       written;
    uint32_t       total;
    char           image_version[32];
    char           message[64];
} c6_ota_status_t;

/* Validate the image on the card without writing anything.
 * Fills image_version and total. */
esp_err_t c6_ota_inspect(const char *path, c6_ota_status_t *out);

/* Same, for the copy embedded in the P4's own flash (the `c6fw` partition,
 * written by `idf.py flash`). This is the no-SD-card path. */
esp_err_t c6_ota_inspect_partition(c6_ota_status_t *out);
esp_err_t c6_ota_run_partition(void);

/* True when the embedded image differs from the one we last pushed, so it
 * flashes exactly once per change rather than on every boot. */
bool c6_ota_partition_pending(void);

/* Run the update synchronously. Takes tens of seconds; call from a task,
 * not from the UI thread. The C6 reboots into the new image on success,
 * which briefly drops Wi-Fi and BLE. */
esp_err_t c6_ota_run(const char *path);

/* Kick the update off on its own task and return immediately. Poll with
 * c6_ota_status(). */
esp_err_t c6_ota_start_async(const char *path);

void c6_ota_status(c6_ota_status_t *out);

/* True while an update is in flight -- scanners should stand down. */
bool c6_ota_busy(void);

#ifdef __cplusplus
}
#endif
