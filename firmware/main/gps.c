#include "gps.h"

#include <stdlib.h>
#include <string.h>

#include "bsp_pins.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gps";

/* A fix older than this is stale: we keep reporting the last position but
 * stop calling it valid, so a logged row never claims a place the receiver
 * has not confirmed recently. */
#define GPS_FIX_TTL_US   (10 * 1000 * 1000)
#define NMEA_MAX         96
#define RX_BUF_SZ        2048
#define RX_CHUNK         256

static gps_fix_t          s_fix;
static SemaphoreHandle_t  s_lock;
static volatile uint32_t  s_sentences, s_bad_checksum, s_raw_bytes;
static volatile bool      s_seen_any;

/* ------------------------------------------------------------------ *
 *  NMEA                                                               *
 * ------------------------------------------------------------------ */

/* "ddmm.mmmm" plus a hemisphere character. Degrees are the leading two
 * digits for latitude and three for longitude; everything after is
 * minutes, which is why this cannot just be atof(). */
static bool parse_coord(const char *val, const char *hemi, int deg_digits,
                        double *out)
{
    if (!val || !*val || !hemi || !*hemi) {
        return false;
    }

    char degbuf[4] = {0};
    if ((int)strlen(val) < deg_digits + 2) {
        return false;
    }
    memcpy(degbuf, val, deg_digits);

    double deg = atof(degbuf);
    double min = atof(val + deg_digits);
    double d   = deg + min / 60.0;

    if (*hemi == 'S' || *hemi == 'W') {
        d = -d;
    }
    *out = d;
    return true;
}

/* NMEA checksum: XOR of everything between '$' and '*', in hex. */
static bool checksum_ok(const char *s)
{
    const char *star = strrchr(s, '*');
    if (!star || s[0] != '$' || strlen(star) < 3) {
        return false;
    }

    uint8_t sum = 0;
    for (const char *p = s + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }

    char hex[3] = { star[1], star[2], 0 };
    return (uint8_t)strtol(hex, NULL, 16) == sum;
}

/* Split on commas in place. Returns the field count. Empty fields are
 * preserved as empty strings, which matters: NMEA leaves fields blank
 * rather than zero when it has nothing, and treating blank as 0 is how
 * you end up logging a position of exactly 0,0 off West Africa. */
static int split(char *s, char *f[], int max)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == ',') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

static void handle_rmc(char *f[], int n)
{
    if (n < 10) {
        return;
    }
    /* f[2] is 'A' active or 'V' void. */
    bool active = (f[2][0] == 'A');

    double lat = 0, lon = 0;
    bool got = active &&
               parse_coord(f[3], f[4], 2, &lat) &&
               parse_coord(f[5], f[6], 3, &lon);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (got) {
        s_fix.lat         = lat;
        s_fix.lon         = lon;
        s_fix.valid       = true;
        s_fix.last_fix_us = esp_timer_get_time();
        if (f[7][0]) {
            s_fix.speed_kts = (float)atof(f[7]);
        }
    } else {
        s_fix.valid = false;
    }
    xSemaphoreGive(s_lock);
}

static void handle_gga(char *f[], int n)
{
    if (n < 10) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (f[6][0]) { s_fix.quality = (uint8_t)atoi(f[6]); }
    if (f[7][0]) { s_fix.sats    = (uint8_t)atoi(f[7]); }
    if (f[9][0]) { s_fix.alt_m   = (float)atof(f[9]); }
    xSemaphoreGive(s_lock);
}

/* The ATGM336H talks all constellations, so sentences arrive with $GP
 * (GPS), $BD (BeiDou) and $GN (combined) talker IDs depending on what it
 * is tracking. Match on the last three characters and ignore the talker. */
static void handle_sentence(char *line)
{
    s_sentences++;

    if (!checksum_ok(line)) {
        s_bad_checksum++;
        return;
    }

    char *f[24];
    int n = split(line, f, 24);
    if (n < 2 || strlen(f[0]) < 6) {
        return;
    }

    const char *type = f[0] + 3;
    if (strncmp(type, "RMC", 3) == 0) {
        handle_rmc(f, n);
    } else if (strncmp(type, "GGA", 3) == 0) {
        handle_gga(f, n);
    }
}

static void gps_task(void *arg)
{
    (void)arg;
    uint8_t chunk[RX_CHUNK];
    char    line[NMEA_MAX];
    size_t  len = 0;
    int64_t next_diag_us = 0;

    for (;;) {
        int n = uart_read_bytes(BSP_GPS_UART_PORT, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(200));
        if (n > 0) {
            s_raw_bytes += (uint32_t)n;
            s_seen_any = true;
        }

        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (len > 6) {
                    line[len] = '\0';
                    handle_sentence(line);
                }
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0;   /* overlong: drop it rather than truncate */
            }
        }

        /* Same reasoning as the external C6 link: when there is no fix,
         * the useful distinction is whether the receiver is talking at
         * all. Silence means wiring or power; chatter without a fix just
         * means no satellites yet, which indoors is the normal state. */
        int64_t now = esp_timer_get_time();
        if (!gps_has_fix() && now >= next_diag_us) {
            next_diag_us = now + 15 * 1000 * 1000;
            if (!s_seen_any) {
                ESP_LOGW(TAG, "no NMEA at all on gpio%d -- check 3V3, GND, "
                              "and that the module TX reaches it",
                         (int)BSP_GPS_PIN_RX);
            } else {
                ESP_LOGI(TAG, "receiver alive (%lu sentences, %lu bad crc) "
                              "but no fix yet -- %u sats tracked",
                         (unsigned long)s_sentences,
                         (unsigned long)s_bad_checksum,
                         (unsigned)s_fix.sats);
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Public                                                             *
 * ------------------------------------------------------------------ */

esp_err_t gps_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t cfg = {
        .baud_rate  = BSP_GPS_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BSP_GPS_UART_PORT, RX_BUF_SZ, 0,
                                            0, NULL, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BSP_GPS_UART_PORT, &cfg),
                        TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BSP_GPS_UART_PORT, BSP_GPS_PIN_TX,
                                     BSP_GPS_PIN_RX, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");

    if (xTaskCreate(gps_task, "gps", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GPS open on UART%d (rx=%d tx=%d, %d baud)",
             (int)BSP_GPS_UART_PORT, (int)BSP_GPS_PIN_RX,
             (int)BSP_GPS_PIN_TX, (int)BSP_GPS_BAUD_RATE);
    return ESP_OK;
}

void gps_get(gps_fix_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_fix;
    xSemaphoreGive(s_lock);
}

bool gps_has_fix(void)
{
    if (!s_lock) {
        return false;
    }
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = s_fix.valid &&
         (esp_timer_get_time() - s_fix.last_fix_us) < GPS_FIX_TTL_US;
    xSemaphoreGive(s_lock);
    return ok;
}

void gps_status(scanner_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reports = s_sentences;

    if (!s_seen_any) {
        out->state = SCAN_UNAVAILABLE;
        strlcpy(out->detail, "no GPS fitted", sizeof(out->detail));
        return;
    }

    gps_fix_t f;
    gps_get(&f);

    if (gps_has_fix()) {
        out->state = SCAN_RUNNING;
        snprintf(out->detail, sizeof(out->detail), "fix, %u sats", f.sats);
    } else {
        out->state = SCAN_RESTARTING;
        snprintf(out->detail, sizeof(out->detail), "no fix, %u sats", f.sats);
    }
}
