/*
 * gps.h -- NMEA position from an ATGM336H (GPS + BeiDou) on a UART.
 *
 * The logger has always carried lat/lon columns and always left them
 * empty. This is what fills them, so a session CSV becomes an actual
 * wardrive track rather than a list of sightings with no places.
 *
 * The module is 2.7-3.6 V. Feed it 3V3, never 5 V.
 * Default line settings are 9600 8-N-1, NMEA 0183.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "scanners.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    valid;        /* RMC reported an active fix                 */
    double  lat;          /* decimal degrees, + north                   */
    double  lon;          /* decimal degrees, + east                    */
    float   alt_m;        /* metres above mean sea level, GGA           */
    float   speed_kts;    /* over ground, RMC                           */
    uint8_t sats;         /* satellites used in the solution, GGA       */
    uint8_t quality;      /* GGA fix quality: 0 none, 1 GPS, 2 DGPS     */
    int64_t last_fix_us;  /* esp_timer stamp of the last valid fix      */
} gps_fix_t;

/* Open the UART and start the reader. Returns ESP_OK once the port is up,
 * NOT once satellites are acquired -- a cold start is minutes outdoors and
 * never indoors, so absence of a fix is normal, not an error. */
esp_err_t gps_start(void);

/* Most recent state. Safe from any task. */
void gps_get(gps_fix_t *out);

/* True when a fix arrived recently enough to be worth logging. */
bool gps_has_fix(void);

/* Link health for the UI, same shape as the scanners use. */
void gps_status(scanner_status_t *out);

#ifdef __cplusplus
}
#endif
