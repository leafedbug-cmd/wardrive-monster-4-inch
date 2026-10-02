#include "sdlog.h"

#include "gps.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bsp_pins.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdlog";

#define LOG_DIR        BSP_SD_MOUNT_POINT "/wardrive"
#define QUEUE_DEPTH    128
#define FLUSH_EVERY_MS 5000
#define FLUSH_EVERY_N  32

static const char *CSV_HEADER =
    "uptime_ms,kind,mac,name,rssi,rssi_best,channel,hits,"
    "auth,addr_type,company,vendor_id,product_id,discriminator,via,"
    "panid,lqi,lat,lon\n";

static const char *KIND_NAME[DET_KIND_COUNT] = {
    "wifi", "ble", "matter", "zigbee"
};

static const char *VIA_NAME[] = {
    "", "ble", "mdns-comm", "mdns-oper"
};

typedef struct {
    uint32_t    session_seq;
    detection_t det;
} logged_record_t;

typedef enum {
    LOG_CMD_START,
    LOG_CMD_STOP,
    LOG_CMD_FLUSH
} log_cmd_type_t;

typedef struct {
    log_cmd_type_t type;
    bool           live_only;
} log_cmd_t;

static QueueHandle_t   s_record_queue;
static QueueHandle_t   s_cmd_queue;
static FILE           *s_fp;
static sdmmc_card_t   *s_card;
static sdlog_status_t  s_st;
static uint32_t        s_since_flush;
static uint32_t        s_current_session_seq = 0;

/* ------------------------------------------------------------------ */

/* Monotonic session number in NVS, so log files never collide across boots
 * even without an RTC. */
static uint32_t next_session_id(void)
{
    nvs_handle_t h;
    uint32_t id = 0;
    if (nvs_open("wardrive", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "session", &id);
        id++;
        nvs_set_u32(h, "session", id);
        nvs_commit(h);
        nvs_close(h);
    }
    return id;
}

/* CSV field quoting: wrap in quotes, double any embedded quote, and drop
 * control characters. SSIDs are arbitrary bytes and will contain both. */
static void csv_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (out_len < 3) {
        if (out_len) {
            out[0] = '\0';
        }
        return;
    }
    out[o++] = '"';
    for (const unsigned char *p = (const unsigned char *)in; *p && o < out_len - 2; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            continue;
        }
        if (*p == '"') {
            if (o >= out_len - 3) {
                break;
            }
            out[o++] = '"';
        }
        out[o++] = (char)*p;
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* Three tries, a third of a second apart. See the retry loop below. */
#define SD_MOUNT_ATTEMPTS  3
#define SD_MOUNT_RETRY_MS  350

static esp_err_t mount_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = BSP_SD_MAX_FREQ_KHZ;

    /* CRITICAL: the ESP32-P4 has two SDMMC slots and BOTH this card and the
     * ESP32-C6 radio hang off that peripheral. esp_hosted is hard-wired to
     * slot 1 (CONFIG_ESP_HOSTED_SDIO_SLOT_1), and SDMMC_HOST_DEFAULT() also
     * returns slot 1 -- so leaving this at the default makes the card and the
     * radio fight over one slot. Whichever initialises first wins and the
     * other fails with a confusing timeout.
     *
     * The card goes on slot 0. Do not change this without moving the C6. */
    host.slot = BSP_SD_SLOT;

    /* On the P4 the card's IO rail comes from an on-chip LDO rather than a
     * fixed supply, so it has to be switched on explicitly. Skip this and the
     * card simply never responds -- mounting fails with
     * ESP_ERR_INVALID_RESPONSE, which looks exactly like a missing card. */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = BSP_SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t perr = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "card LDO (chan %d) setup failed: %s",
                 BSP_SD_LDO_CHAN, esp_err_to_name(perr));
    } else {
        host.pwr_ctrl_handle = pwr;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = BSP_SD_BUS_WIDTH;
    slot.clk   = BSP_SD_PIN_CLK;
    slot.cmd   = BSP_SD_PIN_CMD;
    slot.d0    = BSP_SD_PIN_D0;
    slot.d1    = BSP_SD_PIN_D1;
    slot.d2    = BSP_SD_PIN_D2;
    slot.d3    = BSP_SD_PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   /* never reformat a user's card */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    /* Retry. A card that has just had its IO rail switched on by the LDO
     * can need a moment before it answers ACMD41, and the symptom when it
     * does not is send_op_cond returning ESP_ERR_TIMEOUT -- which is
     * indistinguishable from no card at all. One attempt turned that race
     * into "logging is off for this whole drive"; three attempts a third
     * of a second apart costs nothing on a card that was ready anyway. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
        err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot,
                                      &mcfg, &s_card);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "mount attempt %d/%d failed: %s",
                 attempt, SD_MOUNT_ATTEMPTS, esp_err_to_name(err));
        if (attempt < SD_MOUNT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(SD_MOUNT_RETRY_MS));
        }
    }
    if (err != ESP_OK) {
        if (pwr) {
            sd_pwr_ctrl_del_on_chip_ldo(pwr);
        }
        return err;
    }

    s_st.card_size_mb =
        ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
    ESP_LOGI(TAG, "card mounted: %s, %llu MB",
             s_card->cid.name, s_st.card_size_mb);
    return ESP_OK;
}

static void refresh_free_space(void)
{
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        s_st.free_mb = freeb / (1024 * 1024);
    }
}

static esp_err_t open_session(void)
{
    mkdir(LOG_DIR, 0777);

    uint32_t id = next_session_id();
    snprintf(s_st.path, sizeof(s_st.path), LOG_DIR "/sess-%04lu.csv",
             (unsigned long)id);

    bool fresh = true;
    struct stat st;
    if (stat(s_st.path, &st) == 0 && st.st_size > 0) {
        fresh = false;
    }

    s_fp = fopen(s_st.path, "a");
    if (!s_fp) {
        ESP_LOGE(TAG, "cannot open %s", s_st.path);
        return ESP_FAIL;
    }
    if (fresh) {
        fputs(CSV_HEADER, s_fp);
        fflush(s_fp);
    }

    ESP_LOGI(TAG, "logging to %s", s_st.path);
    return ESP_OK;
}

static void write_row(const detection_t *d)
{
    char name[DET_NAME_LEN * 2 + 4];
    csv_escape(d->name, name, sizeof(name));

    const char *kind = (d->kind < DET_KIND_COUNT) ? KIND_NAME[d->kind] : "?";

    /* Columns not relevant to a given kind are left empty rather than zeroed,
     * so a parser can tell "not applicable" from "measured zero". */
    char auth[8]       = "";
    char addr_type[8]  = "";
    char company[8]    = "";
    char vid[8]        = "";
    char pid[8]        = "";
    char disc[8]       = "";
    char via[12]       = "";
    char panid[8]      = "";
    char lqi[8]        = "";

    switch (d->kind) {
    case DET_WIFI:
        snprintf(auth, sizeof(auth), "%u", d->x.wifi.authmode);
        break;
    case DET_BLE:
        snprintf(addr_type, sizeof(addr_type), "%u", d->x.ble.addr_type);
        if (d->x.ble.company) {
            snprintf(company, sizeof(company), "0x%04X", d->x.ble.company);
        }
        break;
    case DET_MATTER:
        if (d->x.matter.vendor_id) {
            snprintf(vid, sizeof(vid), "0x%04X", d->x.matter.vendor_id);
        }
        if (d->x.matter.product_id) {
            snprintf(pid, sizeof(pid), "0x%04X", d->x.matter.product_id);
        }
        snprintf(disc, sizeof(disc), "%u", d->x.matter.discriminator);
        if (d->x.matter.via < sizeof(VIA_NAME) / sizeof(VIA_NAME[0])) {
            snprintf(via, sizeof(via), "%s", VIA_NAME[d->x.matter.via]);
        }
        break;
    case DET_ZIGBEE:
        snprintf(panid, sizeof(panid), "0x%04X", d->x.zigbee.panid);
        snprintf(lqi, sizeof(lqi), "%u", d->x.zigbee.lqi);
        break;
    default:
        break;
    }

    /* Position, if there is one. These columns stay EMPTY rather than
     * 0 when there is no fix: a literal 0,0 is a real place in the Gulf
     * of Guinea, and every mapping tool will cheerfully plot the whole
     * drive there. */
    char lat[16] = "", lon[16] = "";
    if (gps_has_fix()) {
        gps_fix_t f;
        gps_get(&f);
        snprintf(lat, sizeof(lat), "%.6f", f.lat);
        snprintf(lon, sizeof(lon), "%.6f", f.lon);
    }

    int n = fprintf(s_fp,
        "%lld,%s,%02X:%02X:%02X:%02X:%02X:%02X,%s,%d,%d,%u,%u,"
        "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
        (long long)(d->first_us / 1000), kind,
        d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
        name, d->rssi, d->rssi_best, d->channel, d->hits,
        auth, addr_type, company, vid, pid, disc, via, panid, lqi,
        lat, lon);

    if (n > 0) {
        s_st.written++;
        s_st.bytes += (uint32_t)n;
        s_since_flush++;
    }
}

static SemaphoreHandle_t s_log_lock;

static void do_session_start(bool live_only)
{
    if (s_fp || (s_st.session_active && s_st.live_only && s_st.path[0])) {
        return;
    }

    s_current_session_seq++;
    if (s_record_queue) {
        xQueueReset(s_record_queue);
    }

    s_st.session_start_us = esp_timer_get_time();
    s_st.written = 0;
    s_st.dropped = 0;
    s_st.bytes = 0;
    s_st.error[0] = '\0';

    if (live_only || !s_st.mounted) {
        s_st.live_only = true;
        s_st.session_active = true;
        snprintf(s_st.path, sizeof(s_st.path), "[LIVE ONLY - NO SD]");
        ESP_LOGI(TAG, "session started: live-only mode (seq %lu)", (unsigned long)s_current_session_seq);
        return;
    }

    esp_err_t err = open_session();
    if (err != ESP_OK) {
        s_st.session_active = false;
        s_st.live_only = false;
        snprintf(s_st.error, sizeof(s_st.error), "Failed to open session file");
        ESP_LOGE(TAG, "Failed to open session file: %s", esp_err_to_name(err));
        return;
    }

    s_st.session_active = true;
    s_st.live_only = false;
    refresh_free_space();
    ESP_LOGI(TAG, "session started: logging to %s (seq %lu)", s_st.path, (unsigned long)s_current_session_seq);
}

static void do_session_stop(void)
{
    if (!s_fp && !s_st.session_active) {
        return;
    }

    if (s_record_queue && s_fp && !s_st.live_only) {
        logged_record_t rec;
        while (xQueueReceive(s_record_queue, &rec, 0) == pdTRUE) {
            if (rec.session_seq == s_current_session_seq) {
                write_row(&rec.det);
            }
        }
    }

    if (s_fp) {
        fflush(s_fp);
        fsync(fileno(s_fp));
        fclose(s_fp);
        s_fp = NULL;
        s_since_flush = 0;
        ESP_LOGI(TAG, "session stopped: closed %s (%lu rows)",
                 s_st.path, (unsigned long)s_st.written);
    } else {
        ESP_LOGI(TAG, "session stopped (live-only)");
    }

    if (s_record_queue) {
        xQueueReset(s_record_queue);
    }

    s_st.session_active = false;
    s_st.live_only = false;
}

static void logger_task(void *arg)
{
    (void)arg;
    logged_record_t rec;
    log_cmd_t cmd;
    TickType_t last_flush = xTaskGetTickCount();

    for (;;) {
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            if (s_log_lock) {
                xSemaphoreTake(s_log_lock, portMAX_DELAY);
                if (cmd.type == LOG_CMD_START) {
                    do_session_start(cmd.live_only);
                } else if (cmd.type == LOG_CMD_STOP) {
                    do_session_stop();
                } else if (cmd.type == LOG_CMD_FLUSH && s_fp) {
                    fflush(s_fp);
                    fsync(fileno(s_fp));
                    s_since_flush = 0;
                }
                xSemaphoreGive(s_log_lock);
            }
        }

        if (xQueueReceive(s_record_queue, &rec, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_log_lock) {
                xSemaphoreTake(s_log_lock, portMAX_DELAY);
                if (s_fp && s_st.session_active && !s_st.live_only) {
                    if (rec.session_seq == s_current_session_seq) {
                        write_row(&rec.det);
                    }
                }
                xSemaphoreGive(s_log_lock);
            }
        }

        bool due = (xTaskGetTickCount() - last_flush) >= pdMS_TO_TICKS(FLUSH_EVERY_MS);
        if (s_log_lock) {
            xSemaphoreTake(s_log_lock, portMAX_DELAY);
            if (s_fp && (s_since_flush >= FLUSH_EVERY_N || (due && s_since_flush))) {
                fflush(s_fp);
                fsync(fileno(s_fp));
                s_since_flush = 0;
                last_flush = xTaskGetTickCount();
                refresh_free_space();
            }
            xSemaphoreGive(s_log_lock);
        }
    }
}

/* ------------------------------------------------------------------ */

esp_err_t sdlog_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    if (!s_log_lock) {
        s_log_lock = xSemaphoreCreateMutex();
    }

    s_record_queue = xQueueCreate(QUEUE_DEPTH, sizeof(logged_record_t));
    s_cmd_queue = xQueueCreate(8, sizeof(log_cmd_t));
    if (!s_record_queue || !s_cmd_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = mount_card();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card mounted (%s) -- ready for live-only sessions",
                 esp_err_to_name(err));
        s_st.mounted = false;
        snprintf(s_st.error, sizeof(s_st.error), "No SD card");
    } else {
        refresh_free_space();
        s_st.mounted = true;
        s_st.error[0] = '\0';
        ESP_LOGI(TAG, "card mounted: %llu MB free of %llu MB",
                 (unsigned long long)s_st.free_mb,
                 (unsigned long long)s_st.card_size_mb);
    }

    xTaskCreate(logger_task, "sdlog", 4096, NULL, 4, NULL);

    return ESP_OK;
}

esp_err_t sdlog_session_start(bool live_only)
{
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    log_cmd_t cmd = {
        .type = LOG_CMD_START,
        .live_only = live_only
    };
    return (xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

void sdlog_session_stop(void)
{
    if (!s_cmd_queue) {
        return;
    }

    log_cmd_t cmd = {
        .type = LOG_CMD_STOP,
        .live_only = false
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

bool sdlog_session_is_active(void)
{
    return s_st.session_active;
}

uint32_t sdlog_session_seq(void)
{
    return s_current_session_seq;
}

void sdlog_submit(const detection_t *det)
{
    if (!s_st.session_active || s_st.live_only || !s_record_queue || !det) {
        return;
    }
    logged_record_t rec = {
        .session_seq = s_current_session_seq,
        .det = *det
    };
    if (xQueueSend(s_record_queue, &rec, 0) != pdTRUE) {
        s_st.dropped++;
    }
}

void sdlog_flush(void)
{
    if (!s_cmd_queue) {
        return;
    }
    log_cmd_t cmd = {
        .type = LOG_CMD_FLUSH,
        .live_only = false
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void sdlog_status(sdlog_status_t *out)
{
    if (out) {
        if (s_log_lock) {
            xSemaphoreTake(s_log_lock, portMAX_DELAY);
            *out = s_st;
            xSemaphoreGive(s_log_lock);
        } else {
            *out = s_st;
        }
    }
}

bool sdlog_ready(void)
{
    return s_st.mounted;
}
