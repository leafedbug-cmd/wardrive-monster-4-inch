/*
 * Wardrive Monster -- 4" Wi-Fi / BLE / Matter survey dashboard.
 *
 *   ESP32-P4   application, display, storage, UI
 *   ESP32-C6   the radio, reached over SDIO via ESP-Hosted
 *   ST7796S    4.0" 480x320 SPI panel on the 40-pin header
 *   microSD    the P4's own SDIO slot -- never the one on the display
 *
 * See docs/ for the spec sheets, the pin map, and the C6 OTA procedure.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp_pins.h"
#include "c6ext_link.h"
#include "c6_ota.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_link.h"
#include "scanners.h"
#include "sdlog.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "wardrive";

/* Holding ~4k devices costs about 400 KB of the 32 MB PSRAM. */
#define STORE_CAPACITY 4096

/* Dropping this file next to the image on the card asks for one update. */
#define C6_UPDATE_FLAG BSP_SD_MOUNT_POINT "/c6_update.flag"

/* ------------------------------------------------------------------ *
 *  Boot splash -- something on screen before the radio comes up,      *
 *  which can take a couple of seconds.                                *
 * ------------------------------------------------------------------ */

static lv_obj_t *s_splash_status;

static void splash_show(void)
{
    if (!display_lock(0)) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x080B10), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x22D3EE), 0);
    lv_label_set_text(title, "WARDRIVE MONSTER");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -24);

    s_splash_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_splash_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_splash_status, lv_color_hex(0x7A8699), 0);
    lv_label_set_text(s_splash_status, "starting...");
    lv_obj_align(s_splash_status, LV_ALIGN_CENTER, 0, 14);

    display_unlock();
}

static void splash_say(const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
    if (s_splash_status && display_lock(100)) {
        lv_label_set_text(s_splash_status, msg);
        display_unlock();
    }
}

/* ------------------------------------------------------------------ *
 *  C6 firmware update                                                 *
 *                                                                     *
 *  OTA travels over the ESP-Hosted RPC link, so it only works when    *
 *  the C6 is already running *some* working slave firmware. A C6 that *
 *  is blank or mismatched has to be recovered over its UART pads --   *
 *  see docs/C6-OTA.md.                                                *
 * ------------------------------------------------------------------ */

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Is there an image on the SD card we have been explicitly asked to use? */
static bool sd_update_requested(c6_ota_status_t *info)
{
    if (!sdlog_ready() || !file_exists(BSP_C6_FIRMWARE_PATH)) {
        return false;
    }
    if (c6_ota_inspect(BSP_C6_FIRMWARE_PATH, info) != ESP_OK) {
        ESP_LOGW(TAG, "c6_slave.bin present but not a valid image: %s",
                 info->message);
        return false;
    }
    ESP_LOGI(TAG, "c6_slave.bin on card: version '%s', %lu bytes",
             info->image_version, (unsigned long)info->total);

    if (!file_exists(C6_UPDATE_FLAG)) {
        ESP_LOGI(TAG, "no %s -- card image will be ignored this boot",
                 C6_UPDATE_FLAG);
        return false;
    }
    return true;
}

static void maybe_update_c6(void)
{
    c6_ota_status_t info = {0};
    bool  from_sd = sd_update_requested(&info);
    bool  from_part = false;

    if (!from_sd) {
        /* Nothing asked for from the card -- fall back to the copy embedded
         * in our own flash, but only when it differs from the last one we
         * actually pushed, so this does not re-flash on every boot. */
        c6_ota_status_t pinfo = {0};
        if (c6_ota_inspect_partition(&pinfo) == ESP_OK) {
            ESP_LOGI(TAG, "c6fw partition holds version '%s', %lu bytes",
                     pinfo.image_version, (unsigned long)pinfo.total);
            if (c6_ota_partition_pending()) {
                from_part = true;
                info = pinfo;
            } else {
                ESP_LOGI(TAG, "C6 already has this image -- nothing to do");
            }
        }
    }

    if (!from_sd && !from_part) {
        return;
    }

    net_link_status_t ln;
    net_link_status(&ln);
    if (!ln.hosted_up) {
        ESP_LOGE(TAG, "C6 link is down, so OTA cannot run -- it travels over "
                      "the hosted RPC link. Recover over the C6 UART pads "
                      "instead; see docs/C6-OTA.md.");
        return;
    }

    splash_say("updating C6 firmware...");
    ESP_LOGI(TAG, "starting C6 OTA from %s (C6 currently esp_hosted v%s)",
             from_sd ? "SD card" : "c6fw partition", ln.cp_fw);

    esp_err_t err = from_sd ? c6_ota_run(BSP_C6_FIRMWARE_PATH)
                            : c6_ota_run_partition();

    if (err == ESP_OK) {
        if (from_sd) {
            /* Consume the flag so the next boot does not repeat it. */
            unlink(C6_UPDATE_FLAG);
        }
        splash_say("C6 updated -- restarting radio");
        ESP_LOGI(TAG, "C6 updated to '%s'; giving it time to restart",
                 info.image_version);
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
        c6_ota_status_t st;
        c6_ota_status(&st);
        ESP_LOGE(TAG, "C6 OTA failed: %s", st.message);
        splash_say("C6 update failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Wardrive Monster starting");

    /* Display first: everything after this can take seconds, and a blank
     * screen during boot looks like a dead board. */
    ESP_ERROR_CHECK(display_init());
    splash_show();

    splash_say("mounting SD card...");
    if (sdlog_init() == ESP_OK) {
        sdlog_status_t sd;
        sdlog_status(&sd);
        ESP_LOGI(TAG, "logging to %s (%llu MB card, %llu MB free)",
                 sd.path, (unsigned long long)sd.card_size_mb,
                 (unsigned long long)sd.free_mb);
    } else {
        ESP_LOGW(TAG, "no SD card -- detections will not be logged");
    }

    splash_say("bringing up C6 radio...");
    esp_err_t link = net_link_start();
    if (link != ESP_OK) {
        splash_say("C6 radio FAILED -- see console");
        ESP_LOGE(TAG, "radio link failed; scanners will not run");
    }

    /* Do this before the scanners start, so the radio is idle for it. */
    maybe_update_c6();

    splash_say("preparing store...");
    ESP_ERROR_CHECK(store_init(STORE_CAPACITY));

    /* The external C6 is its own chip on its own UART, so it is brought up
     * independently of the SDIO radio link -- it still scans when the
     * on-board C6 is dead, and it is the only source of 802.15.4. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(c6ext_link_start());
    scan_zigbee_start();

    if (link == ESP_OK) {
        splash_say("starting scanners...");
        ESP_ERROR_CHECK(scan_wifi_start());
        ESP_ERROR_CHECK(scan_ble_start());
        ESP_ERROR_CHECK(scan_matter_start());
    }

    ESP_ERROR_CHECK(ui_init());

    /* Hands-free by default: rotate screens, but a swipe takes over. */
    ui_set_autocycle(12);

    net_link_status_t ln;
    net_link_status(&ln);
    ESP_LOGI(TAG, "up. co-processor %s, esp_hosted v%s",
             ln.cp_target, ln.cp_fw);
}
