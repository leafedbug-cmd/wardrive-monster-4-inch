/*
 * ui.c -- Galaxy Hacker Touchscreen Interface with Nested App-Icon Sub-Launchers.
 *
 * Implements:
 *   - Galaxy Hacker Theme: Deep-space navy/black, orbital motifs, cool-white
 *     telemetry, cyan/violet accents, distinct protocol colors.
 *   - Global Chrome: Status bar with dynamic Back/Home button (< HOME, < BLE, etc.),
 *     active session indicator, hosted & external radio states, GPS fix, SD free space.
 *   - Home Launcher: 6 large app tiles:
 *       1. Dashboard
 *       2. Wardrive Monster
 *       3. BLE Toolkit (Folder)
 *       4. Wi-Fi (Folder)
 *       5. Matter
 *       6. Zigbee (Folder)
 *   - Sub-Launcher Folders (replaces top tabs with phone-style app icons):
 *       - BLE Toolkit Folder -> Chimera, Scanner, Signal Finder, GATT Inspect
 *       - Wi-Fi Folder       -> AP Scanner, Channels, Monitor
 *       - Zigbee Folder      -> 802.15 Frames, Channels, Link Health
 *   - Dedicated Tool Apps:
 *       - ChimeraBLE Toolkit: Real interactive tool with Deep Scan, Fingerprint,
 *         Beacon Clone, and Stop/Reset controls, plus live event display.
 *       - Full-screen AP & BLE scanners, Spectrum histograms, and Signal meters.
 *   - Session Recorder: Clean non-overlapping layout, working start/stop toggle,
 *     and live record counter.
 */

#include "ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp_pins.h"
#include "c6_ota.h"
#include "c6ext_link.h"
#include "chimera_ble.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps.h"
#include "net_link.h"
#include "scanners.h"
#include "sdlog.h"
#include "store.h"

static const char *TAG = "ui";

/* ------------------------------------------------------------------ *
 *  Galaxy Hacker Palette & Constants                                  *
 * ------------------------------------------------------------------ */
#define COL_BG          lv_color_hex(0x060810)  /* Deep space black         */
#define COL_CARD        lv_color_hex(0x0E1424)  /* Navy nebula tile base    */
#define COL_CARD_SUB    lv_color_hex(0x0A0E1A)  /* Inner recessed card      */
#define COL_BORDER      lv_color_hex(0x1E2942)  /* Subtle starlight edge    */
#define COL_BORDER_HL   lv_color_hex(0x38BDF8)  /* Bright starlight focus   */
#define COL_TEXT        lv_color_hex(0xF1F5F9)  /* Cool white telemetry     */
#define COL_MUTED       lv_color_hex(0x64748B)  /* Cosmic dust / slate text */

#define COL_WIFI        lv_color_hex(0x00E5FF)  /* Electric cyan            */
#define COL_BLE         lv_color_hex(0x38BDF8)  /* Sky blue                 */
#define COL_MATTER      lv_color_hex(0xA855F7)  /* Cosmic violet / lavender */
#define COL_ZIGBEE      lv_color_hex(0xFBBF24)  /* Solar amber / gold       */
#define COL_GPS         lv_color_hex(0x10B981)  /* Satellite emerald        */
#define COL_WARDRIVE    lv_color_hex(0xF43F5E)  /* Signal rose / pink       */
#define COL_CHIMERA     lv_color_hex(0xF43F5E)  /* Security pink            */

#define COL_OK          lv_color_hex(0x22C55E)
#define COL_WARN        lv_color_hex(0xF59E0B)
#define COL_BAD         lv_color_hex(0xEF4444)

#define HEADER_H        28
#define FOOTER_H        20
#define BODY_H          (BSP_LCD_V_RES - HEADER_H)
#define DASH_BODY_H     (BSP_LCD_V_RES - HEADER_H - FOOTER_H)

#define LIST_ROWS       10
#define REFRESH_MS      1000

/* ------------------------------------------------------------------ *
 *  Navigation & Hierarchy Mapping                                     *
 * ------------------------------------------------------------------ */
static ui_view_t        s_active_view = UI_VIEW_LAUNCHER;
static ui_dash_screen_t s_dash_screen = UI_DASH_COMBINED;
static uint16_t         s_autocycle_s = 5; /* 5 second cycle when Dashboard app is open */
static int64_t          s_last_touch_ms;

static const ui_view_t s_view_parent[UI_VIEW_COUNT] = {
    [UI_VIEW_LAUNCHER]    = UI_VIEW_LAUNCHER,
    [UI_VIEW_DASHBOARD]   = UI_VIEW_LAUNCHER,
    [UI_VIEW_WARDRIVE]    = UI_VIEW_LAUNCHER,
    [UI_VIEW_BLE_MENU]    = UI_VIEW_LAUNCHER,
    [UI_VIEW_WIFI_MENU]   = UI_VIEW_LAUNCHER,
    [UI_VIEW_MATTER]      = UI_VIEW_LAUNCHER,
    [UI_VIEW_ZIGBEE_MENU] = UI_VIEW_LAUNCHER,

    [UI_VIEW_CHIMERA]     = UI_VIEW_BLE_MENU,
    [UI_VIEW_BLE_SCAN]    = UI_VIEW_BLE_MENU,
    [UI_VIEW_BLE_FOXHUNT] = UI_VIEW_BLE_MENU,
    [UI_VIEW_BLE_GATT]    = UI_VIEW_BLE_MENU,

    [UI_VIEW_WIFI_SCAN]    = UI_VIEW_WIFI_MENU,
    [UI_VIEW_WIFI_CHANS]   = UI_VIEW_WIFI_MENU,
    [UI_VIEW_WIFI_MONITOR] = UI_VIEW_WIFI_MENU,

    [UI_VIEW_ZB_FRAMES]   = UI_VIEW_ZIGBEE_MENU,
    [UI_VIEW_ZB_CHANS]    = UI_VIEW_ZIGBEE_MENU,
    [UI_VIEW_ZB_HEALTH]   = UI_VIEW_ZIGBEE_MENU,
};

/* Root containers for each view */
static lv_obj_t *s_view_cont[UI_VIEW_COUNT];

/* Global Chrome widgets */
static lv_obj_t *s_btn_home;
static lv_obj_t *s_lbl_home;
static lv_obj_t *s_hdr_title;
static lv_obj_t *s_hdr_rec_badge;
static lv_obj_t *s_hdr_link;
static lv_obj_t *s_hdr_ext;
static lv_obj_t *s_hdr_gps;
static lv_obj_t *s_hdr_sd;
static lv_obj_t *s_hdr_clock;

/* Dashboard widgets */
static lv_obj_t *s_dash_tiles;
static lv_obj_t *s_dash_tile[UI_DASH_COUNT];
static lv_obj_t *s_dash_dots[UI_DASH_COUNT];
static lv_obj_t *s_dash_tot_value, *s_dash_tot_rate, *s_dash_tot_sub;
static lv_obj_t *s_dash_card_val[DET_KIND_COUNT], *s_dash_card_sub[DET_KIND_COUNT];
static lv_obj_t *s_dash_big[UI_DASH_COUNT];
static lv_obj_t *s_dash_meta[UI_DASH_COUNT];
static lv_obj_t *s_dash_list[UI_DASH_COUNT];
static lv_obj_t *s_dash_lc_rssi[UI_DASH_COUNT];
static lv_obj_t *s_dash_lc_ch[UI_DASH_COUNT];
static lv_obj_t *s_dash_lc_age[UI_DASH_COUNT];
static lv_obj_t *s_dash_chart;
static lv_chart_series_t *s_dash_chart_ser;
static lv_obj_t *s_dash_gps_sats, *s_dash_gps_meta, *s_dash_gps_lat, *s_dash_gps_lon, *s_dash_gps_state;

/* Wardrive App widgets */
static lv_obj_t *s_wd_btn_session;
static lv_obj_t *s_wd_lbl_session_btn;
static lv_obj_t *s_wd_lbl_state;
static lv_obj_t *s_wd_lbl_file;
static lv_obj_t *s_wd_lbl_time;
static lv_obj_t *s_wd_lbl_records;
static lv_obj_t *s_wd_proto_val[DET_KIND_COUNT];
static lv_obj_t *s_wd_proto_rate[DET_KIND_COUNT];
static lv_obj_t *s_wd_proto_hits[DET_KIND_COUNT];
static lv_obj_t *s_wd_lbl_gps_coords, *s_wd_lbl_gps_stats;
static lv_obj_t *s_wd_lbl_storage_info;

/* Chimera App widgets */
static lv_obj_t *s_chimera_lbl_mode;
static lv_obj_t *s_chimera_lbl_events;
static lv_obj_t *s_chimera_lbl_status;
static lv_obj_t *s_chimera_lbl_detail;

/* BLE Scanner App widgets */
static lv_obj_t *s_ble_scan_name;
static lv_obj_t *s_ble_scan_rssi;
static lv_obj_t *s_ble_scan_type;

/* BLE Signal Finder widgets */
static lv_obj_t *s_ble_finder_target;
static lv_obj_t *s_ble_finder_rssi;
static lv_obj_t *s_ble_finder_peak;
static lv_obj_t *s_ble_finder_bar;

/* BLE GATT Inspector widgets */
static lv_obj_t *s_ble_gatt_info;

/* Wi-Fi AP Scanner widgets */
static lv_obj_t *s_wifi_scan_ssid;
static lv_obj_t *s_wifi_scan_rssi;
static lv_obj_t *s_wifi_scan_ch;
static lv_obj_t *s_wifi_scan_auth;

/* Wi-Fi Channels widgets */
static lv_obj_t *s_wifi_chart;
static lv_chart_series_t *s_wifi_chart_ser;
static lv_obj_t *s_wifi_chans_info;

/* Wi-Fi Monitor widgets */
static lv_obj_t *s_wifi_mon_ssid;
static lv_obj_t *s_wifi_mon_bar;
static lv_obj_t *s_wifi_mon_detail;

/* Matter App widgets */
static lv_obj_t *s_matter_ble_list;
static lv_obj_t *s_matter_mdns_list;
static lv_obj_t *s_matter_detail;

/* Zigbee App widgets */
static lv_obj_t *s_zb_frames_list;
static lv_obj_t *s_zb_pan_list;
static lv_obj_t *s_zb_diag_info;

/* Forward declarations */
static void update_chrome(void);
static void update_wardrive_app(void);

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------------ *
 *  UI Helper Primitives                                               *
 * ------------------------------------------------------------------ */

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static lv_obj_t *mk_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h, lv_color_t edge)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_border_color(c, edge, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void fmt_rssi(char *buf, size_t n, int8_t rssi)
{
    if (rssi == DET_RSSI_NA) {
        snprintf(buf, n, " --");
    } else {
        snprintf(buf, n, "%4d", rssi);
    }
}

static void fmt_age(char *buf, size_t n, int64_t last_us)
{
    if (last_us == 0) {
        snprintf(buf, n, "  -");
        return;
    }
    int64_t age_s = (esp_timer_get_time() - last_us) / 1000000;
    int64_t v;
    char unit;
    if (age_s < 60) {
        v = age_s;          unit = 's';
    } else if (age_s < 3600) {
        v = age_s / 60;     unit = 'm';
    } else {
        v = age_s / 3600;   unit = 'h';
    }
    if (v < 0)  { v = 0;  }
    if (v > 99) { v = 99; }
    snprintf(buf, n, "%2d%c", (int)v, unit);
}

/* ------------------------------------------------------------------ *
 *  Navigation & View Switching                                        *
 * ------------------------------------------------------------------ */

static void home_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_view_t p = s_view_parent[s_active_view];
    ESP_LOGI(TAG, "Nav button clicked -> returning to view %d", (int)p);
    ui_switch_view(p);
}

static void app_tile_clicked_cb(lv_event_t *e)
{
    ui_view_t target = (ui_view_t)(uintptr_t)lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    ESP_LOGI(TAG, "Tile clicked! target view = %d at point (%ld, %ld)", (int)target, (long)p.x, (long)p.y);
    ui_switch_view(target);
}

void ui_switch_view(ui_view_t view)
{
    if (view >= UI_VIEW_COUNT) {
        return;
    }
    s_active_view = view;
    s_last_touch_ms = now_ms();

    /* Manage view visibility */
    for (int i = 0; i < UI_VIEW_COUNT; i++) {
        if (s_view_cont[i]) {
            if (i == (int)view) {
                lv_obj_clear_flag(s_view_cont[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_view_cont[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* Update Home/Back button and header title */
    if (view == UI_VIEW_LAUNCHER) {
        lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hdr_title, "GALAXY MONSTER");
        lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
    } else {
        lv_obj_clear_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
        ui_view_t parent = s_view_parent[view];
        if (parent == UI_VIEW_BLE_MENU) {
            lv_label_set_text(s_lbl_home, "< BLE");
        } else if (parent == UI_VIEW_WIFI_MENU) {
            lv_label_set_text(s_lbl_home, "< WI-FI");
        } else if (parent == UI_VIEW_ZIGBEE_MENU) {
            lv_label_set_text(s_lbl_home, "< ZB");
        } else {
            lv_label_set_text(s_lbl_home, "< HOME");
        }

        switch (view) {
        case UI_VIEW_DASHBOARD:
            lv_label_set_text(s_hdr_title, "DASHBOARD");
            lv_obj_set_style_text_color(s_hdr_title, COL_TEXT, 0);
            break;
        case UI_VIEW_WARDRIVE:
            lv_label_set_text(s_hdr_title, "WARDRIVE MONSTER");
            lv_obj_set_style_text_color(s_hdr_title, COL_WARDRIVE, 0);
            break;
        case UI_VIEW_BLE_MENU:
            lv_label_set_text(s_hdr_title, "BLE TOOLKIT");
            lv_obj_set_style_text_color(s_hdr_title, COL_BLE, 0);
            break;
        case UI_VIEW_WIFI_MENU:
            lv_label_set_text(s_hdr_title, "WI-FI TOOLS");
            lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
            break;
        case UI_VIEW_MATTER:
            lv_label_set_text(s_hdr_title, "MATTER SURVEY");
            lv_obj_set_style_text_color(s_hdr_title, COL_MATTER, 0);
            break;
        case UI_VIEW_ZIGBEE_MENU:
            lv_label_set_text(s_hdr_title, "ZIGBEE TOOLS");
            lv_obj_set_style_text_color(s_hdr_title, COL_ZIGBEE, 0);
            break;
        case UI_VIEW_CHIMERA:
            lv_label_set_text(s_hdr_title, "CHIMERA BLE");
            lv_obj_set_style_text_color(s_hdr_title, COL_CHIMERA, 0);
            break;
        case UI_VIEW_BLE_SCAN:
            lv_label_set_text(s_hdr_title, "BLE SCANNER");
            lv_obj_set_style_text_color(s_hdr_title, COL_BLE, 0);
            break;
        case UI_VIEW_BLE_FOXHUNT:
            lv_label_set_text(s_hdr_title, "SIGNAL FINDER");
            lv_obj_set_style_text_color(s_hdr_title, COL_BLE, 0);
            break;
        case UI_VIEW_BLE_GATT:
            lv_label_set_text(s_hdr_title, "GATT INSPECT");
            lv_obj_set_style_text_color(s_hdr_title, COL_BLE, 0);
            break;
        case UI_VIEW_WIFI_SCAN:
            lv_label_set_text(s_hdr_title, "AP SCANNER");
            lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
            break;
        case UI_VIEW_WIFI_CHANS:
            lv_label_set_text(s_hdr_title, "WI-FI CHANNELS");
            lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
            break;
        case UI_VIEW_WIFI_MONITOR:
            lv_label_set_text(s_hdr_title, "WI-FI MONITOR");
            lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
            break;
        case UI_VIEW_ZB_FRAMES:
            lv_label_set_text(s_hdr_title, "802.15 FRAMES");
            lv_obj_set_style_text_color(s_hdr_title, COL_ZIGBEE, 0);
            break;
        case UI_VIEW_ZB_CHANS:
            lv_label_set_text(s_hdr_title, "ZIGBEE CHANNELS");
            lv_obj_set_style_text_color(s_hdr_title, COL_ZIGBEE, 0);
            break;
        case UI_VIEW_ZB_HEALTH:
            lv_label_set_text(s_hdr_title, "LINK HEALTH");
            lv_obj_set_style_text_color(s_hdr_title, COL_ZIGBEE, 0);
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Global Chrome (Top Status Bar)                                     *
 * ------------------------------------------------------------------ */

static void build_chrome(lv_obj_t *scr)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, BSP_LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(hdr, COL_CARD, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hdr, COL_BORDER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_scrollable(hdr, false);

    /* Left: Home/Back button */
    s_btn_home = lv_btn_create(hdr);
    lv_obj_set_size(s_btn_home, 74, 22);
    lv_obj_align(s_btn_home, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(s_btn_home, COL_CARD_SUB, 0);
    lv_obj_set_style_border_color(s_btn_home, COL_BORDER_HL, 0);
    lv_obj_set_style_border_width(s_btn_home, 1, 0);
    lv_obj_set_style_radius(s_btn_home, 4, 0);
    lv_obj_add_event_cb(s_btn_home, home_btn_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_home = mk_label(s_btn_home, &lv_font_montserrat_14, COL_BORDER_HL, "< HOME");
    lv_obj_align(s_lbl_home, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_lbl_home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lbl_home, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN); /* Initially hidden on launcher */

    /* Title label */
    s_hdr_title = mk_label(hdr, &lv_font_montserrat_16, COL_WIFI, "GALAXY MONSTER");
    lv_obj_align(s_hdr_title, LV_ALIGN_LEFT_MID, 84, 0);

    /* Session REC indicator */
    s_hdr_rec_badge = mk_label(hdr, &lv_font_montserrat_14, COL_WARDRIVE, "[REC]");
    lv_obj_align(s_hdr_rec_badge, LV_ALIGN_LEFT_MID, 230, 0);
    lv_obj_add_flag(s_hdr_rec_badge, LV_OBJ_FLAG_HIDDEN);

    /* Right status badges: Fixed alignment and widths */
    s_hdr_clock = mk_label(hdr, &lv_font_montserrat_14, COL_TEXT, "0m");
    lv_obj_set_width(s_hdr_clock, 44);
    lv_obj_set_style_text_align(s_hdr_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_hdr_clock, LV_ALIGN_RIGHT_MID, -4, 0);

    s_hdr_sd = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "SD --");
    lv_obj_set_width(s_hdr_sd, 54);
    lv_obj_set_style_text_align(s_hdr_sd, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_hdr_sd, LV_ALIGN_RIGHT_MID, -50, 0);

    s_hdr_gps = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "GPS -");
    lv_obj_set_width(s_hdr_gps, 52);
    lv_obj_set_style_text_align(s_hdr_gps, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_hdr_gps, LV_ALIGN_RIGHT_MID, -106, 0);

    s_hdr_ext = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "EXT -");
    lv_obj_set_width(s_hdr_ext, 46);
    lv_obj_set_style_text_align(s_hdr_ext, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_hdr_ext, LV_ALIGN_RIGHT_MID, -160, 0);

    s_hdr_link = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "C6 -");
    lv_obj_set_width(s_hdr_link, 44);
    lv_obj_set_style_text_align(s_hdr_link, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_hdr_link, LV_ALIGN_RIGHT_MID, -208, 0);
}

static void update_chrome(void)
{
    int64_t up_s = esp_timer_get_time() / 1000000;
    if (up_s < 3600) {
        lv_label_set_text_fmt(s_hdr_clock, "%lldm", (long long)(up_s / 60));
    } else {
        lv_label_set_text_fmt(s_hdr_clock, "%lldh%lldm",
                              (long long)(up_s / 3600), (long long)((up_s % 3600) / 60));
    }

    /* Active session indicator */
    sdlog_status_t sd;
    sdlog_status(&sd);
    if (sd.session_active) {
        lv_obj_clear_flag(s_hdr_rec_badge, LV_OBJ_FLAG_HIDDEN);
        int64_t dur_s = (esp_timer_get_time() - sd.session_start_us) / 1000000;
        lv_label_set_text_fmt(s_hdr_rec_badge, "[REC] %02lld:%02lld",
                              (long long)(dur_s / 60), (long long)(dur_s % 60));
        lv_obj_set_style_text_color(s_hdr_rec_badge,
                                    (dur_s % 2 == 0) ? COL_WARDRIVE : COL_TEXT, 0);
    } else {
        lv_obj_add_flag(s_hdr_rec_badge, LV_OBJ_FLAG_HIDDEN);
    }

    /* On-board C6 Hosted status */
    net_link_status_t ln;
    net_link_status(&ln);
    if (ln.hosted_up) {
        lv_label_set_text_fmt(s_hdr_link, "C6 %s%s",
                              ln.wifi_up ? "W" : "-", ln.ble_up ? "B" : "-");
        lv_obj_set_style_text_color(s_hdr_link,
                                    (ln.wifi_up && ln.ble_up) ? COL_OK : COL_WARN, 0);
    } else {
        lv_label_set_text(s_hdr_link, "C6 --");
        lv_obj_set_style_text_color(s_hdr_link, COL_MUTED, 0);
    }

    /* External C6 Zigbee status */
    if (c6ext_link_up()) {
        lv_label_set_text(s_hdr_ext, "EXT Z");
        lv_obj_set_style_text_color(s_hdr_ext, COL_ZIGBEE, 0);
    } else {
        lv_label_set_text(s_hdr_ext, "EXT -");
        lv_obj_set_style_text_color(s_hdr_ext, COL_MUTED, 0);
    }

    /* GPS satellite & fix status */
    gps_fix_t gf;
    gps_get(&gf);
    if (gps_has_fix()) {
        lv_label_set_text_fmt(s_hdr_gps, "GPS %u", (unsigned)gf.sats);
        lv_obj_set_style_text_color(s_hdr_gps, COL_GPS, 0);
    } else if (gf.sats > 0) {
        lv_label_set_text_fmt(s_hdr_gps, "GPS %u", (unsigned)gf.sats);
        lv_obj_set_style_text_color(s_hdr_gps, COL_WARN, 0);
    } else {
        lv_label_set_text(s_hdr_gps, "GPS -");
        lv_obj_set_style_text_color(s_hdr_gps, COL_MUTED, 0);
    }

    /* SD status */
    if (!sd.mounted) {
        lv_label_set_text(s_hdr_sd, "NO SD");
        lv_obj_set_style_text_color(s_hdr_sd, COL_BAD, 0);
    } else {
        lv_label_set_text_fmt(s_hdr_sd, "SD %lluM", (unsigned long long)sd.free_mb);
        lv_obj_set_style_text_color(s_hdr_sd,
                                    sd.free_mb < 64 ? COL_WARN : COL_OK, 0);
    }
}

/* ------------------------------------------------------------------ *
 *  Generic App Tile Builder                                           *
 * ------------------------------------------------------------------ */

static void build_launcher_tile(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h,
                                const char *icon, const char *title, const char *sub,
                                lv_color_t accent, ui_view_t view_target)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_bg_color(tile, COL_CARD, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_border_color(tile, accent, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_pad_all(tile, 8, 0);
    lv_obj_set_scrollable(tile, false);

    /* Make tile clickable and handle launch */
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(tile, COL_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(tile, COL_BORDER_HL, LV_STATE_PRESSED);
    lv_obj_add_event_cb(tile, app_tile_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)view_target);

    /* Children - clear clickable and enable event bubbling */
    lv_obj_t *ic = mk_label(tile, &lv_font_montserrat_28, accent, icon);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *ttl = mk_label(tile, &lv_font_montserrat_16, COL_TEXT, title);
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 10);
    lv_obj_clear_flag(ttl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ttl, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *s = mk_label(tile, &lv_font_montserrat_14, COL_MUTED, sub);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);
}

/* ------------------------------------------------------------------ *
 *  Home Launcher (3x2 Grid)                                           *
 * ------------------------------------------------------------------ */

static void build_launcher(lv_obj_t *cont)
{
    /* Background cosmic star grid (ASCII star motif) */
    static const struct { lv_coord_t x, y; const char *sym; } STARS[] = {
        { 45, 15, "." }, { 190, 8, "*" }, { 340, 20, "." }, { 460, 10, "+" },
        { 10, 150, "+" }, { 465, 160, "." }, { 240, 275, "*" }, { 80, 280, "." }
    };
    for (size_t i = 0; i < sizeof(STARS)/sizeof(STARS[0]); i++) {
        lv_obj_t *s = mk_label(cont, &lv_font_montserrat_14, COL_BORDER, STARS[i].sym);
        lv_obj_set_pos(s, STARS[i].x, STARS[i].y);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 3 columns x 2 rows of app tiles (w=140, h=126) */
    const lv_coord_t TW = 140;
    const lv_coord_t TH = 126;
    const lv_coord_t X0 = 15;
    const lv_coord_t X1 = 170;
    const lv_coord_t X2 = 325;
    const lv_coord_t Y0 = 10;
    const lv_coord_t Y1 = 144;

    /* 1. Dashboard */
    build_launcher_tile(cont, X0, Y0, TW, TH, LV_SYMBOL_LIST, "DASHBOARD", "6-Screen Survey",
                        COL_BORDER_HL, UI_VIEW_DASHBOARD);

    /* 2. Wardrive Monster */
    build_launcher_tile(cont, X1, Y0, TW, TH, LV_SYMBOL_DRIVE, "WARDRIVE", "Session Recorder",
                        COL_WARDRIVE, UI_VIEW_WARDRIVE);

    /* 3. BLE Toolkit (Folder) */
    build_launcher_tile(cont, X2, Y0, TW, TH, LV_SYMBOL_BLUETOOTH, "BLE TOOLKIT", "Chimera & Tools",
                        COL_BLE, UI_VIEW_BLE_MENU);

    /* 4. Wi-Fi (Folder) */
    build_launcher_tile(cont, X0, Y1, TW, TH, LV_SYMBOL_WIFI, "WI-FI", "Scan, Chans, Mon",
                        COL_WIFI, UI_VIEW_WIFI_MENU);

    /* 5. Matter */
    build_launcher_tile(cont, X1, Y1, TW, TH, LV_SYMBOL_HOME, "MATTER", "Commissioning",
                        COL_MATTER, UI_VIEW_MATTER);

    /* 6. Zigbee (Folder) */
    build_launcher_tile(cont, X2, Y1, TW, TH, LV_SYMBOL_SHUFFLE, "ZIGBEE", "Frames, Chans, Link",
                        COL_ZIGBEE, UI_VIEW_ZIGBEE_MENU);
}

/* ------------------------------------------------------------------ *
 *  BLE Sub-Launcher Folder                                            *
 * ------------------------------------------------------------------ */

static void build_ble_menu(lv_obj_t *cont)
{
    const lv_coord_t TW = 216;
    const lv_coord_t TH = 126;
    const lv_coord_t X0 = 16;
    const lv_coord_t X1 = 248;
    const lv_coord_t Y0 = 10;
    const lv_coord_t Y1 = 144;

    build_launcher_tile(cont, X0, Y0, TW, TH, LV_SYMBOL_SETTINGS, "CHIMERA", "Security Toolkit",
                        COL_CHIMERA, UI_VIEW_CHIMERA);

    build_launcher_tile(cont, X1, Y0, TW, TH, LV_SYMBOL_BLUETOOTH, "SCANNER", "Device Scanner",
                        COL_BLE, UI_VIEW_BLE_SCAN);

    build_launcher_tile(cont, X0, Y1, TW, TH, LV_SYMBOL_VOLUME_MAX, "FINDER", "Signal Foxhunt",
                        COL_BLE, UI_VIEW_BLE_FOXHUNT);

    build_launcher_tile(cont, X1, Y1, TW, TH, LV_SYMBOL_LIST, "GATT INSPECT", "Services & Chars",
                        COL_BLE, UI_VIEW_BLE_GATT);
}

/* ------------------------------------------------------------------ *
 *  Wi-Fi Sub-Launcher Folder                                          *
 * ------------------------------------------------------------------ */

static void build_wifi_menu(lv_obj_t *cont)
{
    const lv_coord_t TW = 140;
    const lv_coord_t TH = 250;
    const lv_coord_t X0 = 15;
    const lv_coord_t X1 = 170;
    const lv_coord_t X2 = 325;
    const lv_coord_t Y0 = 15;

    build_launcher_tile(cont, X0, Y0, TW, TH, LV_SYMBOL_WIFI, "AP SCANNER", "Discovered APs",
                        COL_WIFI, UI_VIEW_WIFI_SCAN);

    build_launcher_tile(cont, X1, Y0, TW, TH, LV_SYMBOL_AUDIO, "CHANNELS", "1..14 Histogram",
                        COL_WIFI, UI_VIEW_WIFI_CHANS);

    build_launcher_tile(cont, X2, Y0, TW, TH, LV_SYMBOL_EYE_OPEN, "MONITOR", "Signal Tracking",
                        COL_WIFI, UI_VIEW_WIFI_MONITOR);
}

/* ------------------------------------------------------------------ *
 *  Zigbee Sub-Launcher Folder                                         *
 * ------------------------------------------------------------------ */

static void build_zigbee_menu(lv_obj_t *cont)
{
    const lv_coord_t TW = 140;
    const lv_coord_t TH = 250;
    const lv_coord_t X0 = 15;
    const lv_coord_t X1 = 170;
    const lv_coord_t X2 = 325;
    const lv_coord_t Y0 = 15;

    build_launcher_tile(cont, X0, Y0, TW, TH, LV_SYMBOL_SHUFFLE, "802.15 FRAMES", "Captured Packets",
                        COL_ZIGBEE, UI_VIEW_ZB_FRAMES);

    build_launcher_tile(cont, X1, Y0, TW, TH, LV_SYMBOL_AUDIO, "CHANNELS", "11..26 Activity",
                        COL_ZIGBEE, UI_VIEW_ZB_CHANS);

    build_launcher_tile(cont, X2, Y0, TW, TH, LV_SYMBOL_CHARGE, "LINK HEALTH", "C6 Telemetry",
                        COL_ZIGBEE, UI_VIEW_ZB_HEALTH);
}

/* ------------------------------------------------------------------ *
 *  Dashboard App (6 Nested Screens with Swipe & Scoped Auto-Cycle)   *
 * ------------------------------------------------------------------ */

static void build_dash_combined(lv_obj_t *t)
{
    lv_obj_t *hero = mk_card(t, 6, 4, 190, DASH_BODY_H - 8, COL_BORDER);
    mk_label(hero, &lv_font_montserrat_14, COL_MUTED, "TOTAL UNIQUE");

    s_dash_tot_value = mk_label(hero, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_dash_tot_value, LV_ALIGN_LEFT_MID, 0, -14);

    s_dash_tot_rate = mk_label(hero, &lv_font_montserrat_16, COL_OK, "0 / min");
    lv_obj_align(s_dash_tot_rate, LV_ALIGN_LEFT_MID, 0, 18);

    s_dash_tot_sub = mk_label(hero, &lv_font_montserrat_14, COL_MUTED, "0 logged\n0 dropped\ntable 0% full");
    lv_obj_align(s_dash_tot_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    const char *titles[DET_KIND_COUNT] = { "WI-FI", "BLE", "MATTER", "ZIGBEE" };
    lv_color_t colors[DET_KIND_COUNT] = { COL_WIFI, COL_BLE, COL_MATTER, COL_ZIGBEE };
    const lv_coord_t cw = 132, ch = 112;

    for (int i = 0; i < DET_KIND_COUNT; i++) {
        lv_coord_t x = 204 + (i % 2) * (cw + 6);
        lv_coord_t y = 4 + (i / 2) * (ch + 8);
        lv_obj_t *c = mk_card(t, x, y, cw, ch, colors[i]);

        lv_obj_t *lbl = mk_label(c, &lv_font_montserrat_14, colors[i], titles[i]);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        s_dash_card_val[i] = mk_label(c, &lv_font_montserrat_28, COL_TEXT, "0");
        lv_obj_align(s_dash_card_val[i], LV_ALIGN_LEFT_MID, 0, -2);

        s_dash_card_sub[i] = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "+0/m  0 hits");
        lv_obj_align(s_dash_card_sub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void build_dash_protocol_view(lv_obj_t *t, ui_dash_screen_t which,
                                     const char *title, lv_color_t color)
{
    lv_obj_t *c_hero = mk_card(t, 6, 4, 150, DASH_BODY_H - 8, color);
    mk_label(c_hero, &lv_font_montserrat_14, color, title);

    s_dash_big[which] = mk_label(c_hero, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_dash_big[which], LV_ALIGN_LEFT_MID, 0, -18);

    s_dash_meta[which] = mk_label(c_hero, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_dash_meta[which], LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *c_list = mk_card(t, 162, 4, BSP_LCD_H_RES - 168, DASH_BODY_H - 8, COL_BORDER);

    s_dash_list[which] = mk_label(c_list, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(s_dash_list[which], 0, 0);
    lv_obj_set_width(s_dash_list[which], 174);

    s_dash_lc_rssi[which] = mk_label(c_list, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_dash_lc_rssi[which], 178, 0);
    lv_obj_set_width(s_dash_lc_rssi[which], 42);

    s_dash_lc_ch[which] = mk_label(c_list, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_dash_lc_ch[which], 224, 0);
    lv_obj_set_width(s_dash_lc_ch[which], 36);

    s_dash_lc_age[which] = mk_label(c_list, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_dash_lc_age[which], 264, 0);
    lv_obj_set_width(s_dash_lc_age[which], 36);
}

static void build_dash_wifi_view(lv_obj_t *t)
{
    build_dash_protocol_view(t, UI_DASH_WIFI, "WI-FI 2.4", COL_WIFI);
    s_dash_chart = lv_chart_create(t);
    lv_obj_remove_style_all(s_dash_chart);
    lv_obj_set_size(s_dash_chart, 138, 54);
    lv_obj_set_pos(s_dash_chart, 12, DASH_BODY_H - 68);
    lv_chart_set_type(s_dash_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_dash_chart, 14);
    lv_obj_set_style_pad_column(s_dash_chart, 2, 0);
    s_dash_chart_ser = lv_chart_add_series(s_dash_chart, COL_WIFI, LV_CHART_AXIS_PRIMARY_Y);
}

static void build_dash_gps_view(lv_obj_t *t)
{
    lv_obj_t *c_hero = mk_card(t, 6, 4, 150, DASH_BODY_H - 8, COL_GPS);
    mk_label(c_hero, &lv_font_montserrat_14, COL_GPS, "GPS FIX");

    s_dash_gps_sats = mk_label(c_hero, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_dash_gps_sats, LV_ALIGN_LEFT_MID, 0, -18);

    s_dash_gps_meta = mk_label(c_hero, &lv_font_montserrat_14, COL_MUTED, "0 sats\n0.0 kt\n0 m alt");
    lv_obj_align(s_dash_gps_meta, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *c_det = mk_card(t, 162, 4, BSP_LCD_H_RES - 168, DASH_BODY_H - 8, COL_BORDER);
    mk_label(c_det, &lv_font_montserrat_14, COL_MUTED, "GEODETIC TELEMETRY");

    s_dash_gps_lat = mk_label(c_det, &lv_font_montserrat_20, COL_TEXT, "Lat: --");
    lv_obj_align(s_dash_gps_lat, LV_ALIGN_LEFT_MID, 0, -28);

    s_dash_gps_lon = mk_label(c_det, &lv_font_montserrat_20, COL_TEXT, "Lon: --");
    lv_obj_align(s_dash_gps_lon, LV_ALIGN_LEFT_MID, 0, 0);

    s_dash_gps_state = mk_label(c_det, &lv_font_montserrat_14, COL_MUTED, "Acquiring satellites...");
    lv_obj_align(s_dash_gps_state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void dash_tileview_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *act = lv_tileview_get_tile_act(tv);
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        if (s_dash_tile[i] == act) {
            s_dash_screen = (ui_dash_screen_t)i;
            lv_obj_set_style_bg_color(s_dash_dots[i], COL_BORDER_HL, 0);
            lv_obj_set_style_width(s_dash_dots[i], 16, 0);
        } else {
            lv_obj_set_style_bg_color(s_dash_dots[i], COL_BORDER, 0);
            lv_obj_set_style_width(s_dash_dots[i], 6, 0);
        }
    }
    s_last_touch_ms = now_ms();
}

static void build_dashboard_app(lv_obj_t *cont)
{
    s_dash_tiles = lv_tileview_create(cont);
    lv_obj_remove_style_all(s_dash_tiles);
    lv_obj_set_pos(s_dash_tiles, 0, 0);
    lv_obj_set_size(s_dash_tiles, BSP_LCD_H_RES, DASH_BODY_H);
    lv_obj_set_style_bg_opa(s_dash_tiles, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_dash_tiles, dash_tileview_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < UI_DASH_COUNT; i++) {
        s_dash_tile[i] = lv_tileview_add_tile(s_dash_tiles, i, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
        lv_obj_set_style_bg_opa(s_dash_tile[i], LV_OPA_TRANSP, 0);
    }

    build_dash_combined(s_dash_tile[UI_DASH_COMBINED]);
    build_dash_wifi_view(s_dash_tile[UI_DASH_WIFI]);
    build_dash_protocol_view(s_dash_tile[UI_DASH_BLE], UI_DASH_BLE, "BLE", COL_BLE);
    build_dash_protocol_view(s_dash_tile[UI_DASH_MATTER], UI_DASH_MATTER, "MATTER", COL_MATTER);
    build_dash_protocol_view(s_dash_tile[UI_DASH_ZIGBEE], UI_DASH_ZIGBEE, "ZIGBEE", COL_ZIGBEE);
    build_dash_gps_view(s_dash_tile[UI_DASH_GPS]);

    lv_obj_t *foot = lv_obj_create(cont);
    lv_obj_remove_style_all(foot);
    lv_obj_set_pos(foot, 0, DASH_BODY_H);
    lv_obj_set_size(foot, BSP_LCD_H_RES, FOOTER_H);
    lv_obj_set_style_bg_color(foot, COL_BG, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(foot, false);

    const lv_coord_t dot_start_x = (BSP_LCD_H_RES - (UI_DASH_COUNT * 12 + 10)) / 2;
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        s_dash_dots[i] = lv_obj_create(foot);
        lv_obj_remove_style_all(s_dash_dots[i]);
        lv_obj_set_pos(s_dash_dots[i], dot_start_x + i * 14, 6);
        lv_obj_set_size(s_dash_dots[i], i == 0 ? 16 : 6, 6);
        lv_obj_set_style_radius(s_dash_dots[i], 3, 0);
        lv_obj_set_style_bg_color(s_dash_dots[i], i == 0 ? COL_BORDER_HL : COL_BORDER, 0);
        lv_obj_set_style_bg_opa(s_dash_dots[i], LV_OPA_COVER, 0);
    }
}

void ui_dashboard_show(ui_dash_screen_t screen)
{
    if (screen >= UI_DASH_COUNT || !s_dash_tiles || !s_dash_tile[screen]) {
        return;
    }
    s_dash_screen = screen;
    lv_obj_set_tile(s_dash_tiles, s_dash_tile[screen], LV_ANIM_ON);
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        if (i == (int)screen) {
            lv_obj_set_style_bg_color(s_dash_dots[i], COL_BORDER_HL, 0);
            lv_obj_set_style_width(s_dash_dots[i], 16, 0);
        } else {
            lv_obj_set_style_bg_color(s_dash_dots[i], COL_BORDER, 0);
            lv_obj_set_style_width(s_dash_dots[i], 6, 0);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Wardrive Monster App (Survey / Session Interface)                  *
 * ------------------------------------------------------------------ */

static void session_toggle_clicked_cb(lv_event_t *e)
{
    (void)e;
    sdlog_status_t sd;
    sdlog_status(&sd);
    ESP_LOGI(TAG, "Session toggle clicked! Current active = %d, mounted = %d",
             sd.session_active, sd.mounted);
    if (sd.session_active) {
        sdlog_session_stop();
    } else {
        /* If no card, start live-only session cleanly */
        sdlog_session_start(!sd.mounted);
    }
    update_wardrive_app();
    update_chrome();
}

static void build_wardrive_app(lv_obj_t *cont)
{
    /* Top action card: Start/Stop button and live session status (x=6, y=6, w=468, h=54) */
    lv_obj_t *act_card = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, 54, COL_WARDRIVE);

    s_wd_btn_session = lv_btn_create(act_card);
    lv_obj_set_size(s_wd_btn_session, 150, 42);
    lv_obj_set_pos(s_wd_btn_session, 4, 0);
    lv_obj_set_style_radius(s_wd_btn_session, 6, 0);
    lv_obj_set_style_bg_color(s_wd_btn_session, COL_CARD_SUB, 0);
    lv_obj_set_style_border_color(s_wd_btn_session, COL_OK, 0);
    lv_obj_set_style_border_width(s_wd_btn_session, 2, 0);
    lv_obj_add_event_cb(s_wd_btn_session, session_toggle_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_wd_lbl_session_btn = mk_label(s_wd_btn_session, &lv_font_montserrat_14, COL_OK, "START SESSION");
    lv_obj_align(s_wd_lbl_session_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_wd_lbl_session_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wd_lbl_session_btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_wd_lbl_state = mk_label(act_card, &lv_font_montserrat_14, COL_MUTED, "IDLE");
    lv_obj_set_pos(s_wd_lbl_state, 164, 4);

    s_wd_lbl_file = mk_label(act_card, &lv_font_montserrat_14, COL_TEXT, "Ready to record");
    lv_obj_set_pos(s_wd_lbl_file, 164, 24);

    s_wd_lbl_time = mk_label(act_card, &lv_font_montserrat_14, COL_TEXT, "00:00:00");
    lv_obj_set_pos(s_wd_lbl_time, 350, 4);

    s_wd_lbl_records = mk_label(act_card, &lv_font_montserrat_14, COL_MUTED, "0 logged");
    lv_obj_set_pos(s_wd_lbl_records, 350, 24);

    /* 4 Protocol Telemetry Cards (2x2 Grid) */
    const char *titles[DET_KIND_COUNT] = { "WI-FI", "BLE", "MATTER", "ZIGBEE" };
    lv_color_t colors[DET_KIND_COUNT] = { COL_WIFI, COL_BLE, COL_MATTER, COL_ZIGBEE };
    const lv_coord_t cw = 231, ch = 72;

    for (int i = 0; i < DET_KIND_COUNT; i++) {
        lv_coord_t x = 6 + (i % 2) * (cw + 6);
        lv_coord_t y = 64 + (i / 2) * (ch + 4);
        lv_obj_t *c = mk_card(cont, x, y, cw, ch, colors[i]);

        lv_obj_t *t = mk_label(c, &lv_font_montserrat_14, colors[i], titles[i]);
        lv_obj_set_pos(t, 4, 4);

        s_wd_proto_rate[i] = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "+0/m");
        lv_obj_align(s_wd_proto_rate[i], LV_ALIGN_TOP_RIGHT, -4, 4);

        s_wd_proto_val[i] = mk_label(c, &lv_font_montserrat_28, COL_TEXT, "0");
        lv_obj_set_pos(s_wd_proto_val[i], 6, 26);

        s_wd_proto_hits[i] = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "0 hits");
        lv_obj_align(s_wd_proto_hits[i], LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    }

    /* Bottom: GPS Survey Coordinates & Storage status */
    lv_obj_t *bot = mk_card(cont, 6, 218, BSP_LCD_H_RES - 12, 68, COL_BORDER);

    lv_obj_t *gtitle = mk_label(bot, &lv_font_montserrat_14, COL_GPS, "GPS TELEMETRY");
    lv_obj_set_pos(gtitle, 6, 4);

    s_wd_lbl_gps_coords = mk_label(bot, &lv_font_montserrat_14, COL_TEXT, "Lat: --  Lon: --");
    lv_obj_set_pos(s_wd_lbl_gps_coords, 6, 22);

    s_wd_lbl_gps_stats = mk_label(bot, &lv_font_montserrat_14, COL_MUTED, "0 sats | 0 kt | 0 m alt");
    lv_obj_set_pos(s_wd_lbl_gps_stats, 6, 44);

    s_wd_lbl_storage_info = mk_label(bot, &lv_font_montserrat_14, COL_MUTED, "SD: Checking...");
    lv_obj_align(s_wd_lbl_storage_info, LV_ALIGN_RIGHT_MID, -8, 0);
}

static void update_wardrive_app(void)
{
    sdlog_status_t sd;
    sdlog_status(&sd);

    if (sd.session_active) {
        lv_label_set_text(s_wd_lbl_session_btn, "STOP SESSION");
        lv_obj_set_style_text_color(s_wd_lbl_session_btn, COL_WARDRIVE, 0);
        lv_obj_set_style_border_color(s_wd_btn_session, COL_WARDRIVE, 0);

        if (sd.live_only) {
            lv_label_set_text(s_wd_lbl_state, "[LIVE NO-SD]");
            lv_obj_set_style_text_color(s_wd_lbl_state, COL_WARN, 0);
        } else {
            lv_label_set_text(s_wd_lbl_state, "[RECORDING]");
            lv_obj_set_style_text_color(s_wd_lbl_state, COL_WARDRIVE, 0);
        }

        lv_label_set_text(s_wd_lbl_file, sd.path);

        int64_t dur_s = (esp_timer_get_time() - sd.session_start_us) / 1000000;
        int h = dur_s / 3600;
        int m = (dur_s % 3600) / 60;
        int s = dur_s % 60;
        lv_label_set_text_fmt(s_wd_lbl_time, "%02d:%02d:%02d", h, m, s);
        lv_label_set_text_fmt(s_wd_lbl_records, "%lu logged (%lu drop)",
                              (unsigned long)sd.written, (unsigned long)sd.dropped);
    } else {
        lv_label_set_text(s_wd_lbl_session_btn, "START SESSION");
        lv_obj_set_style_text_color(s_wd_lbl_session_btn, COL_OK, 0);
        lv_obj_set_style_border_color(s_wd_btn_session, COL_OK, 0);

        lv_label_set_text(s_wd_lbl_state, "IDLE");
        lv_obj_set_style_text_color(s_wd_lbl_state, COL_MUTED, 0);
        lv_label_set_text(s_wd_lbl_file, sd.mounted ? "Ready to record" : "No SD card fitted");
        lv_label_set_text(s_wd_lbl_time, "00:00:00");
        lv_label_set_text(s_wd_lbl_records, "0 logged");
    }

    /* 4 Protocol Counts */
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        store_stats_t st;
        store_stats((det_kind_t)i, &st);
        lv_label_set_text_fmt(s_wd_proto_val[i], "%lu", (unsigned long)st.unique);
        lv_label_set_text_fmt(s_wd_proto_rate[i], "+%lu/m", (unsigned long)st.new_last_min);
        lv_label_set_text_fmt(s_wd_proto_hits[i], "%lu hits", (unsigned long)st.hits);
    }

    /* GPS Telemetry */
    gps_fix_t gf;
    gps_get(&gf);
    if (gps_has_fix()) {
        lv_label_set_text_fmt(s_wd_lbl_gps_coords, "Lat: %.5f  Lon: %.5f", gf.lat, gf.lon);
        lv_label_set_text_fmt(s_wd_lbl_gps_stats, "%u sats | %.1f kt | %.0f m alt",
                              (unsigned)gf.sats, (double)gf.speed_kts, (double)gf.alt_m);
        lv_obj_set_style_text_color(s_wd_lbl_gps_coords, COL_TEXT, 0);
    } else {
        lv_label_set_text(s_wd_lbl_gps_coords, "Lat: --  Lon: --");
        lv_label_set_text(s_wd_lbl_gps_stats, "Searching for satellites...");
        lv_obj_set_style_text_color(s_wd_lbl_gps_coords, COL_MUTED, 0);
    }

    /* Storage */
    if (sd.mounted) {
        lv_label_set_text_fmt(s_wd_lbl_storage_info, "SD: %lluM Free\nTotal: %lluM",
                              (unsigned long long)sd.free_mb, (unsigned long long)sd.card_size_mb);
    } else {
        lv_label_set_text(s_wd_lbl_storage_info, "SD: Missing\nLive Only");
    }
}

/* ------------------------------------------------------------------ *
 *  Chimera Toolkit App (Interactive Security Research Tool)           *
 * ------------------------------------------------------------------ */

static void chimera_action_clicked_cb(lv_event_t *e)
{
    int action = (int)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Chimera action button clicked: %d", action);
    switch (action) {
    case 1: /* Deep Scan */
        chimera_scan_start(NULL);
        break;
    case 2: /* Fingerprint */
        chimera_fingerprint_start(NULL, NULL);
        break;
    case 3: /* Clone Beacon */
        chimera_clone_capture_ibeacon(NULL, NULL);
        break;
    case 4: /* Stop */
        chimera_scan_stop();
        chimera_fingerprint_stop();
        chimera_gatt_enum_stop();
        chimera_mitm_stop();
        chimera_clone_stop();
        chimera_keystroke_stop();
        break;
    default:
        break;
    }
}

static void build_chimera_app(lv_obj_t *cont)
{
    /* Top status card (x=6, y=4, w=468, h=44) */
    lv_obj_t *stat_c = mk_card(cont, 6, 4, BSP_LCD_H_RES - 12, 44, COL_CHIMERA);

    s_chimera_lbl_mode = mk_label(stat_c, &lv_font_montserrat_16, COL_CHIMERA, "MODE: IDLE");
    lv_obj_set_pos(s_chimera_lbl_mode, 6, 6);

    s_chimera_lbl_events = mk_label(stat_c, &lv_font_montserrat_14, COL_TEXT, "0 events");
    lv_obj_align(s_chimera_lbl_events, LV_ALIGN_CENTER, 0, 0);

    s_chimera_lbl_status = mk_label(stat_c, &lv_font_montserrat_14, COL_MUTED, "Ready");
    lv_obj_align(s_chimera_lbl_status, LV_ALIGN_RIGHT_MID, -6, 0);

    /* Action Toolbar (x=6, y=52, w=468, h=42) */
    lv_obj_t *bar_c = mk_card(cont, 6, 52, BSP_LCD_H_RES - 12, 42, COL_BORDER);

    const struct { const char *name; lv_color_t color; int act; } BTNS[] = {
        { "DEEP SCAN", COL_BLE, 1 },
        { "FINGERPRINT", COL_MATTER, 2 },
        { "CLONE BEACON", COL_ZIGBEE, 3 },
        { "STOP", COL_BAD, 4 },
    };
    const lv_coord_t bw = (BSP_LCD_H_RES - 12 - 24) / 4;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(bar_c);
        lv_obj_set_size(b, bw, 30);
        lv_obj_set_pos(b, i * (bw + 6), 0);
        lv_obj_set_style_bg_color(b, COL_CARD_SUB, 0);
        lv_obj_set_style_border_color(b, BTNS[i].color, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_add_event_cb(b, chimera_action_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)BTNS[i].act);

        lv_obj_t *lbl = mk_label(b, &lv_font_montserrat_14, BTNS[i].color, BTNS[i].name);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* Output detail log card (x=6, y=98, w=468, h=190) */
    lv_obj_t *out_c = mk_card(cont, 6, 98, BSP_LCD_H_RES - 12, 190, COL_BORDER);
    s_chimera_lbl_detail = mk_label(out_c, &lv_font_montserrat_14, COL_TEXT, "ChimeraBLE Standby.\nSelect an operational mode above to begin analysis.");
    lv_obj_set_width(s_chimera_lbl_detail, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_chimera_lbl_detail, LV_LABEL_LONG_WRAP);
}

static void update_chimera_app(void)
{
    chimera_status_t st;
    chimera_get_status(&st);

    static const char *MODE_NAMES[] = {
        "IDLE", "ENHANCED SCAN", "FINGERPRINT", "GATT ENUM", "MITM", "CLONE", "KEYSTROKE", "?"
    };
    const char *mn = (st.mode < CHIMERA_MODE_COUNT) ? MODE_NAMES[st.mode] : "?";
    lv_label_set_text_fmt(s_chimera_lbl_mode, "MODE: %s", mn);
    lv_label_set_text_fmt(s_chimera_lbl_events, "%lu events", (unsigned long)st.events);
    lv_label_set_text(s_chimera_lbl_status, st.status);

    char buf[512];
    chimera_scan_item_t top[3];
    size_t n = chimera_scan_get_top(top, 3);
    size_t off = snprintf(buf, sizeof(buf), "Toolkit Status: %s | Active: %s\nTarget/Events: %lu\n\n",
                          st.status, st.active ? "YES" : "NO", (unsigned long)st.events);

    for (size_t i = 0; i < n && off < sizeof(buf) - 64; i++) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "[%s] %d dBm | %s\n",
                        top[i].name[0] ? top[i].name : "Unknown",
                        (int)top[i].rssi,
                        top[i].is_ibeacon ? "iBeacon" : top[i].is_eddystone ? "Eddystone" : "BLE");
    }
    if (n == 0 && off < sizeof(buf) - 40) {
        snprintf(buf + off, sizeof(buf) - off, "No active target data captured yet.");
    }
    lv_label_set_text(s_chimera_lbl_detail, buf);
}

/* ------------------------------------------------------------------ *
 *  BLE Scanner App (Full-Screen AP / Device List)                     *
 * ------------------------------------------------------------------ */

static void build_ble_scan_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_BLE);
    s_ble_scan_name = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(s_ble_scan_name, 0, 0);
    lv_obj_set_width(s_ble_scan_name, 280);

    s_ble_scan_rssi = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_ble_scan_rssi, 290, 0);
    lv_obj_set_width(s_ble_scan_rssi, 60);

    s_ble_scan_type = mk_label(c, &lv_font_montserrat_14, COL_BLE, "");
    lv_obj_set_pos(s_ble_scan_type, 360, 0);
    lv_obj_set_width(s_ble_scan_type, 90);
}

static void update_ble_scan_app(void)
{
    chimera_scan_item_t top[10];
    size_t n = chimera_scan_get_top(top, 10);
    char cname[256] = "", crssi[128] = "", ctype[128] = "";
    size_t on = 0, orssi = 0, ot = 0;

    for (size_t i = 0; i < n; i++) {
        on += snprintf(cname + on, sizeof(cname) - on, "%s\n",
                       top[i].name[0] ? top[i].name : "Unknown Device");
        orssi += snprintf(crssi + orssi, sizeof(crssi) - orssi, "%d dBm\n", (int)top[i].rssi);
        ot += snprintf(ctype + ot, sizeof(ctype) - ot, "%s\n",
                       top[i].is_ibeacon ? "iBeacon" :
                       top[i].is_eddystone ? "Eddystone" : "BLE Adv");
    }
    if (n == 0) snprintf(cname, sizeof(cname), "Scanning for BLE devices...");

    lv_label_set_text(s_ble_scan_name, cname);
    lv_label_set_text(s_ble_scan_rssi, crssi);
    lv_label_set_text(s_ble_scan_type, ctype);
}

/* ------------------------------------------------------------------ *
 *  BLE Signal Finder App                                              *
 * ------------------------------------------------------------------ */

static void build_ble_foxhunt_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_BLE);
    mk_label(c, &lv_font_montserrat_14, COL_MUTED, "TARGET SIGNAL TRACKER");

    s_ble_finder_target = mk_label(c, &lv_font_montserrat_16, COL_TEXT, "Target: Strongest Nearby Device");
    lv_obj_align(s_ble_finder_target, LV_ALIGN_TOP_LEFT, 0, 26);

    s_ble_finder_rssi = mk_label(c, &lv_font_montserrat_36, COL_BLE, "-- dBm");
    lv_obj_align(s_ble_finder_rssi, LV_ALIGN_LEFT_MID, 0, -10);

    s_ble_finder_peak = mk_label(c, &lv_font_montserrat_16, COL_OK, "Peak: -- dBm");
    lv_obj_align(s_ble_finder_peak, LV_ALIGN_LEFT_MID, 220, -10);

    s_ble_finder_bar = lv_bar_create(c);
    lv_obj_set_size(s_ble_finder_bar, BSP_LCD_H_RES - 36, 26);
    lv_obj_align(s_ble_finder_bar, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    lv_bar_set_range(s_ble_finder_bar, -100, -20);
    lv_bar_set_value(s_ble_finder_bar, -100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ble_finder_bar, COL_CARD_SUB, 0);
    lv_obj_set_style_bg_color(s_ble_finder_bar, COL_BLE, LV_PART_INDICATOR);
}

static void update_ble_foxhunt_app(void)
{
    chimera_scan_item_t top[1];
    size_t n = chimera_scan_get_top(top, 1);
    if (n > 0) {
        lv_label_set_text_fmt(s_ble_finder_target, "Target: %s",
                              top[0].name[0] ? top[0].name : "Nearest Device");
        lv_label_set_text_fmt(s_ble_finder_rssi, "%d dBm", (int)top[0].rssi);
        lv_bar_set_value(s_ble_finder_bar, top[0].rssi, LV_ANIM_OFF);
    } else {
        lv_label_set_text(s_ble_finder_rssi, "-- dBm");
        lv_bar_set_value(s_ble_finder_bar, -100, LV_ANIM_OFF);
    }
}

/* ------------------------------------------------------------------ *
 *  BLE GATT Inspector App                                             *
 * ------------------------------------------------------------------ */

static void build_ble_gatt_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_BORDER);
    s_ble_gatt_info = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "GATT Inspector: Standby");
    lv_obj_set_width(s_ble_gatt_info, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_ble_gatt_info, LV_LABEL_LONG_WRAP);
}

static void update_ble_gatt_app(void)
{
    chimera_gatt_summary_t g;
    if (chimera_gatt_get_summary(&g)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Target: %02X:%02X:%02X:%02X:%02X:%02X\n"
                 "Connected: %s  MTU: %u\n"
                 "Discovered Services: %u\n"
                 "Enumeration Complete: %s",
                 g.addr[0], g.addr[1], g.addr[2], g.addr[3], g.addr[4], g.addr[5],
                 g.connected ? "YES" : "NO", (unsigned)g.mtu,
                 (unsigned)g.service_count, g.enum_complete ? "YES" : "NO");
        lv_label_set_text(s_ble_gatt_info, buf);
    } else {
        lv_label_set_text(s_ble_gatt_info,
                          "GATT Inspector: Standby.\n\n"
                          "Select an authorized device in the companion CLI or menu\n"
                          "to perform read-only GATT enumeration.");
    }
}

/* ------------------------------------------------------------------ *
 *  Wi-Fi AP Scanner App                                               *
 * ------------------------------------------------------------------ */

static void build_wifi_scan_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_BORDER);
    s_wifi_scan_ssid = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(s_wifi_scan_ssid, 0, 0);
    lv_obj_set_width(s_wifi_scan_ssid, 260);

    s_wifi_scan_rssi = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_wifi_scan_rssi, 270, 0);
    lv_obj_set_width(s_wifi_scan_rssi, 54);

    s_wifi_scan_ch = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_wifi_scan_ch, 330, 0);
    lv_obj_set_width(s_wifi_scan_ch, 36);

    s_wifi_scan_auth = mk_label(c, &lv_font_montserrat_14, COL_WIFI, "");
    lv_obj_set_pos(s_wifi_scan_auth, 370, 0);
    lv_obj_set_width(s_wifi_scan_auth, 80);
}

static void update_wifi_scan_app(void)
{
    static detection_t rows[LIST_ROWS];
    size_t n = store_snapshot(DET_WIFI, rows, LIST_ROWS, true);

    char cssid[LIST_ROWS * 24 + 16] = "";
    char crssi[LIST_ROWS * 8 + 16] = "";
    char cch[LIST_ROWS * 6 + 16] = "";
    char cauth[LIST_ROWS * 12 + 16] = "";
    size_t os = 0, orssi = 0, och = 0, oa = 0;

    for (size_t i = 0; i < n; i++) {
        os += snprintf(cssid + os, sizeof(cssid) - os, "%.21s\n",
                       rows[i].name[0] ? rows[i].name : "[Hidden]");
        orssi += snprintf(crssi + orssi, sizeof(crssi) - orssi, "%d\n", (int)rows[i].rssi_best);
        och += snprintf(cch + och, sizeof(cch) - och, "%u\n", (unsigned)rows[i].channel);
        oa += snprintf(cauth + oa, sizeof(cauth) - oa, "%s\n",
                       rows[i].x.wifi.authmode == 0 ? "OPEN" :
                       rows[i].x.wifi.authmode == 5 ? "WPA3" : "WPA2");
    }
    if (n == 0) snprintf(cssid, sizeof(cssid), "Scanning for APs...");

    lv_label_set_text(s_wifi_scan_ssid, cssid);
    lv_label_set_text(s_wifi_scan_rssi, crssi);
    lv_label_set_text(s_wifi_scan_ch, cch);
    lv_label_set_text(s_wifi_scan_auth, cauth);
}

/* ------------------------------------------------------------------ *
 *  Wi-Fi Channels App (1-14 Histogram)                                *
 * ------------------------------------------------------------------ */

static void build_wifi_chans_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_BORDER);
    mk_label(c, &lv_font_montserrat_14, COL_MUTED, "2.4 GHz SPECTRUM OCCUPANCY (CHANNELS 1..14)");

    s_wifi_chart = lv_chart_create(c);
    lv_obj_remove_style_all(s_wifi_chart);
    lv_obj_set_size(s_wifi_chart, BSP_LCD_H_RES - 36, BODY_H - 80);
    lv_obj_set_pos(s_wifi_chart, 0, 24);
    lv_chart_set_type(s_wifi_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_wifi_chart, 14);
    lv_obj_set_style_pad_column(s_wifi_chart, 6, 0);
    s_wifi_chart_ser = lv_chart_add_series(s_wifi_chart, COL_WIFI, LV_CHART_AXIS_PRIMARY_Y);

    s_wifi_chans_info = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "1  2  3  4  5  6  7  8  9  10 11 12 13 14");
    lv_obj_align(s_wifi_chans_info, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void update_wifi_chans_app(void)
{
    uint16_t hist[14];
    store_wifi_channel_hist(hist);
    uint16_t peak = 1;
    for (int i = 0; i < 14; i++) {
        if (hist[i] > peak) peak = hist[i];
    }
    lv_chart_set_range(s_wifi_chart, LV_CHART_AXIS_PRIMARY_Y, 0, peak);
    for (int i = 0; i < 14; i++) {
        lv_chart_set_value_by_id(s_wifi_chart, s_wifi_chart_ser, i, hist[i]);
    }
}

/* ------------------------------------------------------------------ *
 *  Wi-Fi Signal Monitor App                                           *
 * ------------------------------------------------------------------ */

static void build_wifi_monitor_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_WIFI);
    mk_label(c, &lv_font_montserrat_14, COL_MUTED, "ACTIVE AP SIGNAL MONITOR");

    s_wifi_mon_ssid = mk_label(c, &lv_font_montserrat_20, COL_TEXT, "Searching...");
    lv_obj_align(s_wifi_mon_ssid, LV_ALIGN_TOP_LEFT, 0, 26);

    s_wifi_mon_detail = mk_label(c, &lv_font_montserrat_16, COL_MUTED, "RSSI: -- dBm");
    lv_obj_align(s_wifi_mon_detail, LV_ALIGN_LEFT_MID, 0, 0);

    s_wifi_mon_bar = lv_bar_create(c);
    lv_obj_set_size(s_wifi_mon_bar, BSP_LCD_H_RES - 36, 26);
    lv_obj_align(s_wifi_mon_bar, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    lv_bar_set_range(s_wifi_mon_bar, -100, -20);
    lv_bar_set_value(s_wifi_mon_bar, -100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_wifi_mon_bar, COL_CARD_SUB, 0);
    lv_obj_set_style_bg_color(s_wifi_mon_bar, COL_WIFI, LV_PART_INDICATOR);
}

static void update_wifi_monitor_app(void)
{
    static detection_t rows[1];
    size_t n = store_snapshot(DET_WIFI, rows, 1, true);
    if (n > 0) {
        lv_label_set_text_fmt(s_wifi_mon_ssid, "SSID: %s",
                              rows[0].name[0] ? rows[0].name : "[Hidden SSID]");
        lv_label_set_text_fmt(s_wifi_mon_detail, "Signal: %d dBm | Ch: %u",
                              (int)rows[0].rssi_best, (unsigned)rows[0].channel);
        lv_bar_set_value(s_wifi_mon_bar, rows[0].rssi_best, LV_ANIM_OFF);
    } else {
        lv_label_set_text(s_wifi_mon_ssid, "No AP signal detected");
        lv_label_set_text(s_wifi_mon_detail, "RSSI: -- dBm");
        lv_bar_set_value(s_wifi_mon_bar, -100, LV_ANIM_OFF);
    }
}

/* ------------------------------------------------------------------ *
 *  Matter App                                                         *
 * ------------------------------------------------------------------ */

static void build_matter_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_MATTER);
    mk_label(c, &lv_font_montserrat_14, COL_MATTER, "MATTER COMMISSIONING & MDNS SURVEY");

    s_matter_ble_list = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "Waiting for Matter BLE adverts...");
    lv_obj_align(s_matter_ble_list, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_width(s_matter_ble_list, BSP_LCD_H_RES - 36);

    s_matter_mdns_list = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_matter_mdns_list, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(s_matter_mdns_list, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_matter_mdns_list, LV_LABEL_LONG_WRAP);

    s_matter_detail = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "Listening for commissionable nodes...");
    lv_obj_align(s_matter_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void update_matter_app(void)
{
    static detection_t rows[LIST_ROWS];
    size_t n = store_snapshot(DET_MATTER, rows, LIST_ROWS, false);
    char buf[512] = "";
    size_t off = 0;

    for (size_t i = 0; i < n && off < sizeof(buf) - 64; i++) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "Node: %02X:%02X:%02X:%02X:%02X:%02X  Disc: %u  VID: 0x%04X\n",
                        rows[i].mac[0], rows[i].mac[1], rows[i].mac[2],
                        rows[i].mac[3], rows[i].mac[4], rows[i].mac[5],
                        (unsigned)rows[i].x.matter.discriminator,
                        (unsigned)rows[i].x.matter.vendor_id);
    }
    if (n == 0) {
        snprintf(buf, sizeof(buf), "No Commissionable BLE nodes detected nearby.");
    }
    lv_label_set_text(s_matter_ble_list, buf);
}

/* ------------------------------------------------------------------ *
 *  Zigbee Tools Apps                                                  *
 * ------------------------------------------------------------------ */

static void build_zb_frames_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_ZIGBEE);
    mk_label(c, &lv_font_montserrat_14, COL_ZIGBEE, "802.15.4 CAPTURED FRAMES");

    s_zb_frames_list = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "Waiting for 802.15.4 frames...");
    lv_obj_set_pos(s_zb_frames_list, 0, 26);
    lv_obj_set_width(s_zb_frames_list, BSP_LCD_H_RES - 36);
}

static void update_zb_frames_app(void)
{
    static detection_t rows[LIST_ROWS];
    size_t n = store_snapshot(DET_ZIGBEE, rows, LIST_ROWS, true);
    char buf[512] = "";
    size_t off = 0;
    for (size_t i = 0; i < n && off < sizeof(buf) - 64; i++) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "MAC: %02X:%02X:%02X:%02X:%02X:%02X  PAN: 0x%04X  LQI: %u  RSSI: %d\n",
                        rows[i].mac[0], rows[i].mac[1], rows[i].mac[2],
                        rows[i].mac[3], rows[i].mac[4], rows[i].mac[5],
                        (unsigned)rows[i].x.zigbee.panid,
                        (unsigned)rows[i].x.zigbee.lqi,
                        (int)rows[i].rssi_best);
    }
    if (n == 0) {
        snprintf(buf, sizeof(buf), "No IEEE 802.15.4 / Zigbee frames captured yet.");
    }
    lv_label_set_text(s_zb_frames_list, buf);
}

static void build_zb_chans_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_ZIGBEE);
    mk_label(c, &lv_font_montserrat_14, COL_ZIGBEE, "ZIGBEE CHANNELS & PAN OCCUPANCY");

    s_zb_pan_list = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "Scanning channels 11..26...");
    lv_obj_set_pos(s_zb_pan_list, 0, 26);
    lv_obj_set_width(s_zb_pan_list, BSP_LCD_H_RES - 36);
}

static void update_zb_chans_app(void)
{
    store_stats_t st;
    store_stats(DET_ZIGBEE, &st);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "IEEE 802.15.4 Activity\n\n"
             "Unique Devices: %lu\n"
             "Total Frame Hits: %lu\n"
             "Rate: %lu / min\n"
             "Channels: 11-26 2.4 GHz ISM",
             (unsigned long)st.unique,
             (unsigned long)st.hits,
             (unsigned long)st.new_last_min);
    lv_label_set_text(s_zb_pan_list, buf);
}

static void build_zb_health_app(lv_obj_t *cont)
{
    lv_obj_t *c = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, BODY_H - 12, COL_ZIGBEE);
    s_zb_diag_info = mk_label(c, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_width(s_zb_diag_info, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_zb_diag_info, LV_LABEL_LONG_WRAP);
}

static void update_zb_health_app(void)
{
    scanner_status_t sc;
    c6ext_link_status(&sc);
    uint32_t c_wifi = 0, c_ble = 0, c_zb = 0;
    c6ext_link_counts(&c_wifi, &c_ble, &c_zb);

    char buf[384];
    if (c6ext_link_up()) {
        snprintf(buf, sizeof(buf),
                 "External Seeed Studio XIAO ESP32-C6 Link: CONNECTED\n\n"
                 "Hardware: UART1 on GPIO47 (TX) / GPIO27 (RX)\n"
                 "Baud Rate: 460,800 baud\n"
                 "Link State: Active Heartbeat\n\n"
                 "Packets Contributed:\n"
                 "  - 802.15.4: %lu\n"
                 "  - Wi-Fi:    %lu\n"
                 "  - BLE:      %lu",
                 (unsigned long)c_zb, (unsigned long)c_wifi, (unsigned long)c_ble);
        lv_obj_set_style_text_color(s_zb_diag_info, COL_TEXT, 0);
    } else {
        snprintf(buf, sizeof(buf),
                 "External Seeed Studio XIAO ESP32-C6 Link: DISCONNECTED\n\n"
                 "Detail: %s\n\n"
                 "Wiring Verification:\n"
                 "  - XIAO 3V3 & GND connected to 40-pin header\n"
                 "  - XIAO D6 (TX) connected to P4 GPIO27 (RX)\n"
                 "  - XIAO D7 (RX) connected to P4 GPIO47 (TX)\n"
                 "  - Baud: 460800",
                 sc.detail[0] ? sc.detail : "Waiting for heartbeat");
        lv_obj_set_style_text_color(s_zb_diag_info, COL_WARN, 0);
    }
    lv_label_set_text(s_zb_diag_info, buf);
}

/* ------------------------------------------------------------------ *
 *  Main Timer Refresh Callback                                        *
 * ------------------------------------------------------------------ */

static void update_dash_combined(void)
{
    store_stats_t tot;
    store_stats_total(&tot);
    lv_label_set_text_fmt(s_dash_tot_value, "%lu", (unsigned long)tot.unique);
    lv_label_set_text_fmt(s_dash_tot_rate, "%lu / min", (unsigned long)tot.new_last_min);
    lv_obj_set_style_text_color(s_dash_tot_rate, tot.new_last_min ? COL_OK : COL_MUTED, 0);

    sdlog_status_t sd;
    sdlog_status(&sd);
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu logged\n%lu dropped\ntable %u%% full",
             (unsigned long)sd.written, (unsigned long)sd.dropped, store_fill_pct());
    lv_label_set_text(s_dash_tot_sub, buf);

    for (int i = 0; i < DET_KIND_COUNT; i++) {
        store_stats_t st;
        store_stats((det_kind_t)i, &st);
        lv_label_set_text_fmt(s_dash_card_val[i], "%lu", (unsigned long)st.unique);
        snprintf(buf, sizeof(buf), "+%lu/m  %lu hits",
                 (unsigned long)st.new_last_min, (unsigned long)st.hits);
        lv_label_set_text(s_dash_card_sub[i], buf);
    }
}

static void update_dash_protocol(ui_dash_screen_t which, det_kind_t kind, bool by_rssi)
{
    store_stats_t st;
    store_stats(kind, &st);
    lv_label_set_text_fmt(s_dash_big[which], "%lu", (unsigned long)st.unique);

    scanner_status_t sc = {0};
    switch (kind) {
    case DET_WIFI:   scan_wifi_status(&sc);   break;
    case DET_BLE:    scan_ble_status(&sc);    break;
    case DET_MATTER: scan_matter_status(&sc); break;
    default:         scan_zigbee_status(&sc); break;
    }

    char meta[160], best[8];
    fmt_rssi(best, sizeof(best), st.best_rssi == -128 ? DET_RSSI_NA : st.best_rssi);
    snprintf(meta, sizeof(meta), "+%lu/min  %lu hits\nbest %s dBm\n%s",
             (unsigned long)st.new_last_min, (unsigned long)st.hits, best, sc.detail);
    lv_label_set_text(s_dash_meta[which], meta);
    lv_obj_set_style_text_color(s_dash_meta[which],
                                sc.state == SCAN_UNAVAILABLE ? COL_BAD : COL_MUTED, 0);

    static detection_t rows[LIST_ROWS];
    size_t n = store_snapshot(kind, rows, LIST_ROWS, by_rssi);

    char cname[LIST_ROWS * 24 + 16] = "";
    char crssi[LIST_ROWS * 8 + 16] = "";
    char cch[LIST_ROWS * 6 + 16] = "";
    char cage[LIST_ROWS * 8 + 16] = "";
    size_t on = 0, orssi = 0, och = 0, oage = 0;

    for (size_t i = 0; i < n; i++) {
        char label[22];
        if (rows[i].name[0]) {
            snprintf(label, sizeof(label), "%.21s", rows[i].name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X:%02X:%02X:%02X",
                     rows[i].mac[0], rows[i].mac[1], rows[i].mac[2],
                     rows[i].mac[3], rows[i].mac[4], rows[i].mac[5]);
        }
        char age[8];
        fmt_age(age, sizeof(age), rows[i].last_us);
        int8_t r = by_rssi ? rows[i].rssi_best : rows[i].rssi;

        on += snprintf(cname + on, sizeof(cname) - on, "%s\n", label);
        if (r == DET_RSSI_NA) {
            orssi += snprintf(crssi + orssi, sizeof(crssi) - orssi, "--\n");
        } else {
            orssi += snprintf(crssi + orssi, sizeof(crssi) - orssi, "%d\n", (int)r);
        }
        if (rows[i].channel) {
            och += snprintf(cch + och, sizeof(cch) - och, "%u\n", (unsigned)rows[i].channel);
        } else {
            och += snprintf(cch + och, sizeof(cch) - och, "-\n");
        }
        oage += snprintf(cage + oage, sizeof(cage) - oage, "%s\n", age);
    }
    if (n == 0) {
        snprintf(cname, sizeof(cname), "nothing yet");
    }

    lv_label_set_text(s_dash_list[which],    cname);
    lv_label_set_text(s_dash_lc_rssi[which], crssi);
    lv_label_set_text(s_dash_lc_ch[which],   cch);
    lv_label_set_text(s_dash_lc_age[which],  cage);
}

static void update_dash_wifi(void)
{
    update_dash_protocol(UI_DASH_WIFI, DET_WIFI, true);
    uint16_t hist[14];
    store_wifi_channel_hist(hist);
    uint16_t peak = 1;
    for (int i = 0; i < 14; i++) {
        if (hist[i] > peak) peak = hist[i];
    }
    lv_chart_set_range(s_dash_chart, LV_CHART_AXIS_PRIMARY_Y, 0, peak);
    for (int i = 0; i < 14; i++) {
        lv_chart_set_value_by_id(s_dash_chart, s_dash_chart_ser, i, hist[i]);
    }
}

static void update_dash_gps(void)
{
    gps_fix_t gf;
    gps_get(&gf);
    lv_label_set_text_fmt(s_dash_gps_sats, "%u", (unsigned)gf.sats);

    char meta[96];
    snprintf(meta, sizeof(meta), "%u sats\n%.1f kt\n%.0f m alt",
             (unsigned)gf.sats, (double)gf.speed_kts, (double)gf.alt_m);
    lv_label_set_text(s_dash_gps_meta, meta);

    if (gf.valid) {
        lv_label_set_text_fmt(s_dash_gps_lat, "Lat: %.5f", gf.lat);
        lv_label_set_text_fmt(s_dash_gps_lon, "Lon: %.5f", gf.lon);
    } else {
        lv_label_set_text(s_dash_gps_lat, "Lat: --");
        lv_label_set_text(s_dash_gps_lon, "Lon: --");
    }

    scanner_status_t gs;
    gps_status(&gs);
    if (gs.state == SCAN_UNAVAILABLE) {
        lv_label_set_text(s_dash_gps_state, "No NMEA data from module.");
        lv_obj_set_style_text_color(s_dash_gps_state, COL_BAD, 0);
    } else if (!gf.valid) {
        lv_label_set_text(s_dash_gps_state, "NMEA stream alive, acquiring satellites...");
        lv_obj_set_style_text_color(s_dash_gps_state, COL_WARN, 0);
    } else {
        lv_label_set_text(s_dash_gps_state, "Fix good -- GPS coordinates live.");
        lv_obj_set_style_text_color(s_dash_gps_state, COL_OK, 0);
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Always update global chrome */
    update_chrome();

    static uint32_t s_refresh_count = 0;
    if (++s_refresh_count % 10 == 0) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "LVGL task stack high-water mark: %u bytes free out of 8192 | Heap free: %u/%u bytes (largest: %u, used: %u%%)",
                 (unsigned)hwm, (unsigned)mon.free_size, (unsigned)mon.total_size,
                 (unsigned)mon.free_biggest_size, (unsigned)mon.used_pct);
    }

    /* Refresh only active view */
    switch (s_active_view) {
    case UI_VIEW_LAUNCHER:
    case UI_VIEW_BLE_MENU:
    case UI_VIEW_WIFI_MENU:
    case UI_VIEW_ZIGBEE_MENU:
        /* Menus are static launcher grids */
        break;

    case UI_VIEW_DASHBOARD:
        switch (s_dash_screen) {
        case UI_DASH_COMBINED: update_dash_combined(); break;
        case UI_DASH_WIFI:     update_dash_wifi();     break;
        case UI_DASH_BLE:      update_dash_protocol(UI_DASH_BLE, DET_BLE, true); break;
        case UI_DASH_MATTER:   update_dash_protocol(UI_DASH_MATTER, DET_MATTER, false); break;
        case UI_DASH_ZIGBEE:   update_dash_protocol(UI_DASH_ZIGBEE, DET_ZIGBEE, true); break;
        case UI_DASH_GPS:      update_dash_gps();      break;
        default: break;
        }

        /* Scoped auto-cycling within Dashboard only */
        if (s_autocycle_s && (now_ms() - s_last_touch_ms > (int64_t)s_autocycle_s * 1000)) {
            ui_dash_screen_t next = (s_dash_screen + 1) % UI_DASH_COUNT;
            ESP_LOGI(TAG, "Dashboard auto-cycle: transitioning to screen %d", (int)next);
            ui_dashboard_show(next);
            s_last_touch_ms = now_ms();
        }
        break;

    case UI_VIEW_WARDRIVE:
        update_wardrive_app();
        break;

    case UI_VIEW_CHIMERA:
        update_chimera_app();
        break;

    case UI_VIEW_BLE_SCAN:
        update_ble_scan_app();
        break;

    case UI_VIEW_BLE_FOXHUNT:
        update_ble_foxhunt_app();
        break;

    case UI_VIEW_BLE_GATT:
        update_ble_gatt_app();
        break;

    case UI_VIEW_WIFI_SCAN:
        update_wifi_scan_app();
        break;

    case UI_VIEW_WIFI_CHANS:
        update_wifi_chans_app();
        break;

    case UI_VIEW_WIFI_MONITOR:
        update_wifi_monitor_app();
        break;

    case UI_VIEW_MATTER:
        update_matter_app();
        break;

    case UI_VIEW_ZB_FRAMES:
        update_zb_frames_app();
        break;

    case UI_VIEW_ZB_CHANS:
        update_zb_chans_app();
        break;

    case UI_VIEW_ZB_HEALTH:
        update_zb_health_app();
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ *
 *  Public Initialization & Control API                                *
 * ------------------------------------------------------------------ */

esp_err_t ui_init(void)
{
    if (!display_lock(0)) {
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 1. Build Top Status Chrome */
    build_chrome(scr);

    /* 2. Build Root View Containers */
    for (int i = 0; i < UI_VIEW_COUNT; i++) {
        s_view_cont[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_view_cont[i]);
        lv_obj_set_pos(s_view_cont[i], 0, HEADER_H);
        lv_obj_set_size(s_view_cont[i], BSP_LCD_H_RES, BODY_H);
        lv_obj_set_style_bg_color(s_view_cont[i], COL_BG, 0);
        lv_obj_set_style_bg_opa(s_view_cont[i], LV_OPA_COVER, 0);
        lv_obj_set_scrollable(s_view_cont[i], false);
        lv_obj_set_scrollbar_mode(s_view_cont[i], LV_SCROLLBAR_MODE_OFF);
    }

    /* 3. Build View Content with heap monitoring */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL Heap before widgets: %u free / %u total (largest: %u)",
             (unsigned)mon.free_size, (unsigned)mon.total_size, (unsigned)mon.free_biggest_size);

    /* Launcher & Primary Apps */
    build_launcher(s_view_cont[UI_VIEW_LAUNCHER]);
    build_dashboard_app(s_view_cont[UI_VIEW_DASHBOARD]);
    build_wardrive_app(s_view_cont[UI_VIEW_WARDRIVE]);
    build_matter_app(s_view_cont[UI_VIEW_MATTER]);

    /* Sub-Launcher Folders */
    build_ble_menu(s_view_cont[UI_VIEW_BLE_MENU]);
    build_wifi_menu(s_view_cont[UI_VIEW_WIFI_MENU]);
    build_zigbee_menu(s_view_cont[UI_VIEW_ZIGBEE_MENU]);

    /* Dedicated Tool Apps */
    build_chimera_app(s_view_cont[UI_VIEW_CHIMERA]);
    build_ble_scan_app(s_view_cont[UI_VIEW_BLE_SCAN]);
    build_ble_foxhunt_app(s_view_cont[UI_VIEW_BLE_FOXHUNT]);
    build_ble_gatt_app(s_view_cont[UI_VIEW_BLE_GATT]);

    build_wifi_scan_app(s_view_cont[UI_VIEW_WIFI_SCAN]);
    build_wifi_chans_app(s_view_cont[UI_VIEW_WIFI_CHANS]);
    build_wifi_monitor_app(s_view_cont[UI_VIEW_WIFI_MONITOR]);

    build_zb_frames_app(s_view_cont[UI_VIEW_ZB_FRAMES]);
    build_zb_chans_app(s_view_cont[UI_VIEW_ZB_CHANS]);
    build_zb_health_app(s_view_cont[UI_VIEW_ZB_HEALTH]);

    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after all views: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    /* Hide inactive views now that all widgets are fully constructed */
    for (int i = 0; i < UI_VIEW_COUNT; i++) {
        if (i != (int)UI_VIEW_LAUNCHER) {
            lv_obj_add_flag(s_view_cont[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Boot into Launcher */
    s_active_view = UI_VIEW_LAUNCHER;
    s_last_touch_ms = now_ms();
    update_chrome();

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    display_unlock();
    ESP_LOGI(TAG, "Galaxy Hacker UI initialized (Booting to Launcher)");
    return ESP_OK;
}

void ui_set_autocycle(uint16_t seconds)
{
    s_autocycle_s = seconds;
    s_last_touch_ms = now_ms();
}
