/*
 * ui.c -- Galaxy Hacker Touchscreen Interface & Phone-Style App Launcher.
 *
 * Implements:
 *   - Galaxy Hacker Theme: Deep-space navy/black, orbital motifs, cool-white
 *     telemetry, cyan/violet accents, distinct protocol colors.
 *   - Global Chrome: Status bar with Home/Back button, active session badge,
 *     radios (Hosted C6, Ext C6), GPS sats, SD status, and uptime clock.
 *   - Home Launcher: 3-column x 2-row manual layout with 6 large touch tiles.
 *   - Dashboard App: Nested 6-screen swiping telemetry (Combined, Wi-Fi, BLE,
 *     Matter, Zigbee, GPS) with scoped auto-cycling and live-only viewing.
 *   - Wardrive Monster App: Survey / session controller with explicit Start/Stop,
 *     session time, protocol counts, GPS status, and SD logging state.
 *   - BLE App: Scanning list, ChimeraBLE companion toolkit, Signal Finder, GATT inspector.
 *   - Wi-Fi App: AP scanner list, 2.4 GHz channel occupancy chart, Signal monitor.
 *   - Matter App: BLE commissioning discovery, mDNS discovery with network requirements.
 *   - Zigbee App: 802.15.4 frame observations, channel distribution, external C6 link diagnostics.
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
 *  Navigation & State                                                 *
 * ------------------------------------------------------------------ */
static ui_view_t        s_active_view = UI_VIEW_LAUNCHER;
static ui_dash_screen_t s_dash_screen = UI_DASH_COMBINED;
static uint16_t         s_autocycle_s = 0;
static int64_t          s_last_touch_ms;

/* Root containers for each view */
static lv_obj_t *s_view_cont[UI_VIEW_COUNT];

/* Global Chrome widgets */
static lv_obj_t *s_btn_home;
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
static lv_obj_t *s_wd_proto_sub[DET_KIND_COUNT];
static lv_obj_t *s_wd_lbl_gps_coords, *s_wd_lbl_gps_stats;
static lv_obj_t *s_wd_lbl_storage_info;

/* BLE App widgets */
static lv_obj_t *s_ble_tabs[4];
static lv_obj_t *s_ble_tab_btns[4];
static uint8_t   s_ble_active_tab = 0;
static lv_obj_t *s_ble_list_name, *s_ble_list_rssi, *s_ble_list_type;
static lv_obj_t *s_ble_chimera_mode, *s_ble_chimera_events, *s_ble_chimera_status, *s_ble_chimera_detail;
static lv_obj_t *s_ble_finder_rssi, *s_ble_finder_bar, *s_ble_finder_peak, *s_ble_finder_target;
static lv_obj_t *s_ble_gatt_info;

/* Wi-Fi App widgets */
static lv_obj_t *s_wifi_tabs[3];
static lv_obj_t *s_wifi_tab_btns[3];
static uint8_t   s_wifi_active_tab = 0;
static lv_obj_t *s_wifi_list_ssid, *s_wifi_list_rssi, *s_wifi_list_ch, *s_wifi_list_auth;
static lv_obj_t *s_wifi_chart;
static lv_chart_series_t *s_wifi_chart_ser;
static lv_obj_t *s_wifi_mon_ssid, *s_wifi_mon_bar, *s_wifi_mon_detail;

/* Matter App widgets */
static lv_obj_t *s_matter_tabs[3];
static lv_obj_t *s_matter_tab_btns[3];
static uint8_t   s_matter_active_tab = 0;
static lv_obj_t *s_matter_ble_list, *s_matter_mdns_list, *s_matter_detail;

/* Zigbee App widgets */
static lv_obj_t *s_zb_tabs[3];
static lv_obj_t *s_zb_tab_btns[3];
static uint8_t   s_zb_active_tab = 0;
static lv_obj_t *s_zb_frames_list, *s_zb_pan_list, *s_zb_diag_info;

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
    ui_switch_view(UI_VIEW_LAUNCHER);
}

static void app_tile_clicked_cb(lv_event_t *e)
{
    ui_view_t target = (ui_view_t)(uintptr_t)lv_event_get_user_data(e);
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

    /* Update Home button visibility & title */
    if (view == UI_VIEW_LAUNCHER) {
        lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hdr_title, "✦ GALAXY MONSTER");
        lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
    } else {
        lv_obj_clear_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
        switch (view) {
        case UI_VIEW_DASHBOARD:
            lv_label_set_text(s_hdr_title, "DASHBOARD");
            lv_obj_set_style_text_color(s_hdr_title, COL_TEXT, 0);
            break;
        case UI_VIEW_WARDRIVE:
            lv_label_set_text(s_hdr_title, "WARDRIVE MONSTER");
            lv_obj_set_style_text_color(s_hdr_title, COL_WARDRIVE, 0);
            break;
        case UI_VIEW_BLE:
            lv_label_set_text(s_hdr_title, "BLE TOOLKIT");
            lv_obj_set_style_text_color(s_hdr_title, COL_BLE, 0);
            break;
        case UI_VIEW_WIFI:
            lv_label_set_text(s_hdr_title, "WI-FI SCANNER");
            lv_obj_set_style_text_color(s_hdr_title, COL_WIFI, 0);
            break;
        case UI_VIEW_MATTER:
            lv_label_set_text(s_hdr_title, "MATTER SURVEY");
            lv_obj_set_style_text_color(s_hdr_title, COL_MATTER, 0);
            break;
        case UI_VIEW_ZIGBEE:
            lv_label_set_text(s_hdr_title, "ZIGBEE / 802.15.4");
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

    lv_obj_t *lbl_b = mk_label(s_btn_home, &lv_font_montserrat_14, COL_BORDER_HL, "< HOME");
    lv_obj_align(lbl_b, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(lbl_b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN); /* Initially hidden on launcher */

    /* Title label */
    s_hdr_title = mk_label(hdr, &lv_font_montserrat_16, COL_WIFI, "✦ GALAXY MONSTER");
    lv_obj_align(s_hdr_title, LV_ALIGN_LEFT_MID, 84, 0);

    /* Session REC indicator */
    s_hdr_rec_badge = mk_label(hdr, &lv_font_montserrat_14, COL_WARDRIVE, "● REC");
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
        lv_label_set_text_fmt(s_hdr_rec_badge, "● REC %02lld:%02lld",
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
        lv_obj_set_style_text_color(s_hdr_link, COL_BAD, 0);
    }

    /* External C6 status */
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
 *  Home Launcher (3x2 Grid)                                           *
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
    lv_obj_add_event_cb(tile, app_tile_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)view_target);

    /* Decorative children - clear clickable so clicks bubble up */
    lv_obj_t *ic = mk_label(tile, &lv_font_montserrat_28, accent, icon);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ttl = mk_label(tile, &lv_font_montserrat_16, COL_TEXT, title);
    lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 4, 10);
    lv_obj_clear_flag(ttl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *s = mk_label(tile, &lv_font_montserrat_14, COL_MUTED, sub);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
}

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

    /* 3 columns x 2 rows of generously sized app tiles (w=140, h=126) */
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

    /* 3. BLE */
    build_launcher_tile(cont, X2, Y0, TW, TH, LV_SYMBOL_BLUETOOTH, "BLE TOOLKIT", "Scan & Chimera",
                        COL_BLE, UI_VIEW_BLE);

    /* 4. Wi-Fi */
    build_launcher_tile(cont, X0, Y1, TW, TH, LV_SYMBOL_WIFI, "WI-FI", "2.4 GHz AP Scan",
                        COL_WIFI, UI_VIEW_WIFI);

    /* 5. Matter */
    build_launcher_tile(cont, X1, Y1, TW, TH, LV_SYMBOL_HOME, "MATTER", "Commissioning",
                        COL_MATTER, UI_VIEW_MATTER);

    /* 6. Zigbee */
    build_launcher_tile(cont, X2, Y1, TW, TH, LV_SYMBOL_SHUFFLE, "ZIGBEE", "802.15.4 Sniffer",
                        COL_ZIGBEE, UI_VIEW_ZIGBEE);
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

    s_dash_tot_sub = mk_label(hero, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_dash_tot_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    static const char *titles[DET_KIND_COUNT] = { "WI-FI", "BLE", "MATTER", "ZIGBEE" };
    const lv_color_t colors[DET_KIND_COUNT] = { COL_WIFI, COL_BLE, COL_MATTER, COL_ZIGBEE };

    const lv_coord_t cw = 134, ch = (DASH_BODY_H - 14) / 2;
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        lv_coord_t x = 202 + (i % 2) * (cw + 6);
        lv_coord_t y = 4 + (i / 2) * (ch + 6);
        lv_obj_t *c = mk_card(t, x, y, cw, ch, colors[i]);

        lv_obj_t *ttl = mk_label(c, &lv_font_montserrat_14, colors[i], titles[i]);
        lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

        s_dash_card_val[i] = mk_label(c, &lv_font_montserrat_28, COL_TEXT, "0");
        lv_obj_align(s_dash_card_val[i], LV_ALIGN_LEFT_MID, 0, 2);

        s_dash_card_sub[i] = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
        lv_obj_align(s_dash_card_sub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void build_dash_protocol(lv_obj_t *t, ui_dash_screen_t which, bool has_chart)
{
    const lv_coord_t COL_L_X = 6, COL_L_W = 150;
    const lv_coord_t STAT_Y = 4, STAT_H = 68;
    const lv_coord_t META_Y = 76, META_H = DASH_BODY_H - META_Y - 4;
    const lv_coord_t COL_R_X = 162, COL_R_W = BSP_LCD_H_RES - COL_R_X - 6;

    lv_color_t col = COL_TEXT;
    switch (which) {
    case UI_DASH_WIFI:   col = COL_WIFI; break;
    case UI_DASH_BLE:    col = COL_BLE; break;
    case UI_DASH_MATTER: col = COL_MATTER; break;
    default:             col = COL_ZIGBEE; break;
    }

    lv_obj_t *stat = mk_card(t, COL_L_X, STAT_Y, COL_L_W, STAT_H, col);
    lv_obj_t *cap = mk_label(stat, &lv_font_montserrat_14, COL_MUTED, "TOTAL UNIQUE");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    s_dash_big[which] = mk_label(stat, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_dash_big[which], LV_ALIGN_BOTTOM_LEFT, 0, 2);

    lv_obj_t *metac = mk_card(t, COL_L_X, META_Y, COL_L_W, META_H, COL_BORDER);
    s_dash_meta[which] = mk_label(metac, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(s_dash_meta[which], COL_L_W - 14);
    lv_label_set_long_mode(s_dash_meta[which], LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dash_meta[which], LV_ALIGN_TOP_LEFT, 0, 0);

    lv_coord_t list_y = 4;
    lv_coord_t list_h = DASH_BODY_H - 8;

    if (has_chart) {
        const lv_coord_t CHART_H = 64;
        lv_obj_t *cc = mk_card(t, COL_R_X, 4, COL_R_W, CHART_H, COL_BORDER);
        s_dash_chart = lv_chart_create(cc);
        lv_obj_remove_style_all(s_dash_chart);
        lv_obj_set_size(s_dash_chart, COL_R_W - 12, CHART_H - 12);
        lv_obj_set_pos(s_dash_chart, 0, 0);
        lv_chart_set_type(s_dash_chart, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(s_dash_chart, 14);
        lv_obj_set_style_pad_column(s_dash_chart, 2, 0);
        s_dash_chart_ser = lv_chart_add_series(s_dash_chart, COL_WIFI, LV_CHART_AXIS_PRIMARY_Y);
        list_y += CHART_H + 4;
        list_h -= (CHART_H + 4);
    }

    lv_obj_t *rc = mk_card(t, COL_R_X, list_y, COL_R_W, list_h, COL_BORDER);
    const lv_coord_t C_AGE_W  = 30;
    const lv_coord_t C_CH_W   = 24;
    const lv_coord_t C_RSSI_W = 38;
    const lv_coord_t C_NAME_W = COL_R_W - 14 - C_AGE_W - C_CH_W - C_RSSI_W;

    s_dash_list[which] = mk_label(rc, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(s_dash_list[which], 0, 0);
    lv_obj_set_width(s_dash_list[which], C_NAME_W);
    lv_label_set_long_mode(s_dash_list[which], LV_LABEL_LONG_CLIP);

    s_dash_lc_rssi[which] = mk_label(rc, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_dash_lc_rssi[which], C_NAME_W, 0);
    lv_obj_set_width(s_dash_lc_rssi[which], C_RSSI_W);
    lv_obj_set_style_text_align(s_dash_lc_rssi[which], LV_TEXT_ALIGN_RIGHT, 0);

    s_dash_lc_ch[which] = mk_label(rc, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_dash_lc_ch[which], C_NAME_W + C_RSSI_W, 0);
    lv_obj_set_width(s_dash_lc_ch[which], C_CH_W);
    lv_obj_set_style_text_align(s_dash_lc_ch[which], LV_TEXT_ALIGN_RIGHT, 0);

    s_dash_lc_age[which] = mk_label(rc, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_dash_lc_age[which], C_NAME_W + C_RSSI_W + C_CH_W, 0);
    lv_obj_set_width(s_dash_lc_age[which], C_AGE_W);
    lv_obj_set_style_text_align(s_dash_lc_age[which], LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_dash_gps(lv_obj_t *t)
{
    const lv_coord_t COL_L_X = 6, COL_L_W = 150;
    const lv_coord_t STAT_Y = 4, STAT_H = 68;
    const lv_coord_t META_Y = 76, META_H = DASH_BODY_H - META_Y - 4;
    const lv_coord_t COL_R_X = 162, COL_R_W = BSP_LCD_H_RES - COL_R_X - 6;

    lv_obj_t *stat = mk_card(t, COL_L_X, STAT_Y, COL_L_W, STAT_H, COL_GPS);
    lv_obj_t *cap = mk_label(stat, &lv_font_montserrat_14, COL_MUTED, "SATELLITES");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    s_dash_gps_sats = mk_label(stat, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_dash_gps_sats, LV_ALIGN_BOTTOM_LEFT, 0, 2);

    lv_obj_t *metac = mk_card(t, COL_L_X, META_Y, COL_L_W, META_H, COL_BORDER);
    s_dash_gps_meta = mk_label(metac, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(s_dash_gps_meta, COL_L_W - 14);
    lv_label_set_long_mode(s_dash_gps_meta, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dash_gps_meta, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *pos = mk_card(t, COL_R_X, 4, COL_R_W, DASH_BODY_H - 8, COL_GPS);
    lv_obj_t *latcap = mk_label(pos, &lv_font_montserrat_14, COL_MUTED, "LATITUDE");
    lv_obj_align(latcap, LV_ALIGN_TOP_LEFT, 0, 4);
    s_dash_gps_lat = mk_label(pos, &lv_font_montserrat_28, COL_TEXT, "--");
    lv_obj_align(s_dash_gps_lat, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t *loncap = mk_label(pos, &lv_font_montserrat_14, COL_MUTED, "LONGITUDE");
    lv_obj_align(loncap, LV_ALIGN_TOP_LEFT, 0, 74);
    s_dash_gps_lon = mk_label(pos, &lv_font_montserrat_28, COL_TEXT, "--");
    lv_obj_align(s_dash_gps_lon, LV_ALIGN_TOP_LEFT, 0, 94);

    s_dash_gps_state = mk_label(pos, &lv_font_montserrat_16, COL_MUTED, "");
    lv_obj_set_width(s_dash_gps_state, COL_R_W - 14);
    lv_label_set_long_mode(s_dash_gps_state, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dash_gps_state, LV_ALIGN_TOP_LEFT, 0, 150);
}

static void dash_tile_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *cur = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        if (s_dash_tile[i] == cur) {
            s_dash_screen = (ui_dash_screen_t)i;
            break;
        }
    }
    s_last_touch_ms = now_ms();
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        lv_obj_set_style_bg_color(s_dash_dots[i],
                                  i == s_dash_screen ? COL_BORDER_HL : COL_BORDER, 0);
    }
}

static void build_dashboard_app(lv_obj_t *cont)
{
    s_dash_tiles = lv_tileview_create(cont);
    lv_obj_set_pos(s_dash_tiles, 0, 0);
    lv_obj_set_size(s_dash_tiles, BSP_LCD_H_RES, DASH_BODY_H);
    lv_obj_set_style_bg_color(s_dash_tiles, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_dash_tiles, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dash_tiles, 0, 0);
    lv_obj_add_event_cb(s_dash_tiles, dash_tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < UI_DASH_COUNT; i++) {
        s_dash_tile[i] = lv_tileview_add_tile(s_dash_tiles, i, 0, LV_DIR_HOR);
        lv_obj_set_style_pad_all(s_dash_tile[i], 0, 0);
        lv_obj_set_scrollbar_mode(s_dash_tile[i], LV_SCROLLBAR_MODE_OFF);
    }

    build_dash_combined(s_dash_tile[UI_DASH_COMBINED]);
    build_dash_protocol(s_dash_tile[UI_DASH_WIFI],   UI_DASH_WIFI,   true);
    build_dash_protocol(s_dash_tile[UI_DASH_BLE],    UI_DASH_BLE,    false);
    build_dash_protocol(s_dash_tile[UI_DASH_MATTER], UI_DASH_MATTER, false);
    build_dash_protocol(s_dash_tile[UI_DASH_ZIGBEE], UI_DASH_ZIGBEE, false);
    build_dash_gps(s_dash_tile[UI_DASH_GPS]);

    /* Footer: 6 indicator dots */
    lv_obj_t *ftr = lv_obj_create(cont);
    lv_obj_remove_style_all(ftr);
    lv_obj_set_pos(ftr, 0, DASH_BODY_H);
    lv_obj_set_size(ftr, BSP_LCD_H_RES, FOOTER_H);
    lv_obj_set_style_bg_color(ftr, COL_CARD, 0);
    lv_obj_set_style_bg_opa(ftr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(ftr, false);

    const lv_coord_t gap = 18;
    const lv_coord_t x0 = BSP_LCD_H_RES / 2 - (UI_DASH_COUNT - 1) * gap / 2;
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(ftr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, i == 0 ? COL_BORDER_HL : COL_BORDER, 0);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, x0 + i * gap, 0);
        s_dash_dots[i] = d;
    }
}

void ui_dashboard_show(ui_dash_screen_t screen)
{
    if (screen >= UI_DASH_COUNT || !s_dash_tiles) {
        return;
    }
    lv_tileview_set_tile_by_index(s_dash_tiles, screen, 0, LV_ANIM_ON);
    s_dash_screen = screen;
    for (int i = 0; i < UI_DASH_COUNT; i++) {
        lv_obj_set_style_bg_color(s_dash_dots[i],
                                  i == screen ? COL_BORDER_HL : COL_BORDER, 0);
    }
}

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
    gps_fix_t f;
    gps_get(&f);
    scanner_status_t sc;
    gps_status(&sc);

    lv_label_set_text_fmt(s_dash_gps_sats, "%u", (unsigned)f.sats);
    if (gps_has_fix()) {
        lv_label_set_text_fmt(s_dash_gps_lat, "%.5f", f.lat);
        lv_label_set_text_fmt(s_dash_gps_lon, "%.5f", f.lon);
        lv_obj_set_style_text_color(s_dash_gps_lat, COL_TEXT, 0);
        lv_obj_set_style_text_color(s_dash_gps_lon, COL_TEXT, 0);
    } else {
        lv_label_set_text(s_dash_gps_lat, "--");
        lv_label_set_text(s_dash_gps_lon, "--");
        lv_obj_set_style_text_color(s_dash_gps_lat, COL_MUTED, 0);
        lv_obj_set_style_text_color(s_dash_gps_lon, COL_MUTED, 0);
    }

    static const char *QUALITY[] = { "no fix", "GPS", "DGPS" };
    const char *q = (f.quality < 3) ? QUALITY[f.quality] : "fix";
    char meta[160];
    snprintf(meta, sizeof(meta), "%s\n%.0f m alt\n%.1f kt\n%lu sentences",
             q, (double)f.alt_m, (double)f.speed_kts, (unsigned long)sc.reports);
    lv_label_set_text(s_dash_gps_meta, meta);

    if (sc.state == SCAN_UNAVAILABLE) {
        lv_label_set_text(s_dash_gps_state, "No NMEA on GPS pins.\nCheck 3V3, GND, TX->RX.");
        lv_obj_set_style_text_color(s_dash_gps_state, COL_BAD, 0);
    } else if (!gps_has_fix()) {
        lv_label_set_text(s_dash_gps_state, "Receiver talking, waiting for satellites.\nA cold start takes minutes.");
        lv_obj_set_style_text_color(s_dash_gps_state, COL_WARN, 0);
    } else {
        lv_label_set_text(s_dash_gps_state, "Fix good -- GPS coordinates live.");
        lv_obj_set_style_text_color(s_dash_gps_state, COL_OK, 0);
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
    if (sd.session_active) {
        sdlog_session_stop();
    } else {
        /* If no card, start live-only session cleanly */
        sdlog_session_start(!sd.mounted);
    }
}

static void build_wardrive_app(lv_obj_t *cont)
{
    /* Top action card: Start/Stop button and live session status */
    lv_obj_t *act_card = mk_card(cont, 6, 6, BSP_LCD_H_RES - 12, 60, COL_WARDRIVE);

    s_wd_btn_session = lv_btn_create(act_card);
    lv_obj_set_size(s_wd_btn_session, 150, 46);
    lv_obj_align(s_wd_btn_session, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_radius(s_wd_btn_session, 6, 0);
    lv_obj_set_style_bg_color(s_wd_btn_session, COL_CARD_SUB, 0);
    lv_obj_set_style_border_color(s_wd_btn_session, COL_OK, 0);
    lv_obj_set_style_border_width(s_wd_btn_session, 2, 0);
    lv_obj_add_event_cb(s_wd_btn_session, session_toggle_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_wd_lbl_session_btn = mk_label(s_wd_btn_session, &lv_font_montserrat_14, COL_OK, "▶ START SESSION");
    lv_obj_align(s_wd_lbl_session_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_wd_lbl_session_btn, LV_OBJ_FLAG_CLICKABLE);

    s_wd_lbl_state = mk_label(act_card, &lv_font_montserrat_16, COL_MUTED, "IDLE");
    lv_obj_align(s_wd_lbl_state, LV_ALIGN_TOP_LEFT, 168, 4);

    s_wd_lbl_file = mk_label(act_card, &lv_font_montserrat_14, COL_TEXT, "No active file");
    lv_obj_align(s_wd_lbl_file, LV_ALIGN_LEFT_MID, 168, 6);

    s_wd_lbl_time = mk_label(act_card, &lv_font_montserrat_14, COL_MUTED, "Time: 00:00:00");
    lv_obj_align(s_wd_lbl_time, LV_ALIGN_BOTTOM_LEFT, 168, -4);

    s_wd_lbl_records = mk_label(act_card, &lv_font_montserrat_14, COL_MUTED, "0 rows");
    lv_obj_align(s_wd_lbl_records, LV_ALIGN_BOTTOM_RIGHT, -8, -4);

    /* 4 Protocol Telemetry Cards (2x2 Grid) */
    static const char *titles[DET_KIND_COUNT] = { "WI-FI", "BLE", "MATTER", "ZIGBEE" };
    const lv_color_t colors[DET_KIND_COUNT] = { COL_WIFI, COL_BLE, COL_MATTER, COL_ZIGBEE };
    const lv_coord_t cw = 231, ch = 68;

    for (int i = 0; i < DET_KIND_COUNT; i++) {
        lv_coord_t x = 6 + (i % 2) * (cw + 6);
        lv_coord_t y = 72 + (i / 2) * (ch + 6);
        lv_obj_t *c = mk_card(cont, x, y, cw, ch, colors[i]);

        lv_obj_t *t = mk_label(c, &lv_font_montserrat_14, colors[i], titles[i]);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

        s_wd_proto_val[i] = mk_label(c, &lv_font_montserrat_28, COL_TEXT, "0");
        lv_obj_align(s_wd_proto_val[i], LV_ALIGN_LEFT_MID, 0, 4);

        s_wd_proto_sub[i] = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
        lv_obj_align(s_wd_proto_sub[i], LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }

    /* Bottom: GPS Survey Coordinates & Storage status */
    lv_obj_t *bot = mk_card(cont, 6, 222, BSP_LCD_H_RES - 12, 64, COL_BORDER);

    lv_obj_t *gtitle = mk_label(bot, &lv_font_montserrat_14, COL_GPS, "GPS TELEMETRY");
    lv_obj_align(gtitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_wd_lbl_gps_coords = mk_label(bot, &lv_font_montserrat_16, COL_TEXT, "Lat: --  Lon: --");
    lv_obj_align(s_wd_lbl_gps_coords, LV_ALIGN_LEFT_MID, 0, 4);

    s_wd_lbl_gps_stats = mk_label(bot, &lv_font_montserrat_14, COL_MUTED, "0 sats | 0 kt");
    lv_obj_align(s_wd_lbl_gps_stats, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_wd_lbl_storage_info = mk_label(bot, &lv_font_montserrat_14, COL_MUTED, "SD: Checking...");
    lv_obj_align(s_wd_lbl_storage_info, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void update_wardrive_app(void)
{
    sdlog_status_t sd;
    sdlog_status(&sd);

    if (sd.session_active) {
        lv_label_set_text(s_wd_lbl_session_btn, "■ STOP SESSION");
        lv_obj_set_style_text_color(s_wd_lbl_session_btn, COL_WARDRIVE, 0);
        lv_obj_set_style_border_color(s_wd_btn_session, COL_WARDRIVE, 0);

        if (sd.live_only) {
            lv_label_set_text(s_wd_lbl_state, "● LIVE SURVEY (NO SD)");
            lv_obj_set_style_text_color(s_wd_lbl_state, COL_WARN, 0);
        } else {
            lv_label_set_text(s_wd_lbl_state, "● RECORDING SESSION");
            lv_obj_set_style_text_color(s_wd_lbl_state, COL_WARDRIVE, 0);
        }

        lv_label_set_text(s_wd_lbl_file, sd.path);

        int64_t dur_s = (esp_timer_get_time() - sd.session_start_us) / 1000000;
        int h = dur_s / 3600;
        int m = (dur_s % 3600) / 60;
        int s = dur_s % 60;
        lv_label_set_text_fmt(s_wd_lbl_time, "Time: %02d:%02d:%02d", h, m, s);
        lv_label_set_text_fmt(s_wd_lbl_records, "%lu logged (%lu drop)",
                              (unsigned long)sd.written, (unsigned long)sd.dropped);
    } else {
        lv_label_set_text(s_wd_lbl_session_btn, "▶ START SESSION");
        lv_obj_set_style_text_color(s_wd_lbl_session_btn, COL_OK, 0);
        lv_obj_set_style_border_color(s_wd_btn_session, COL_OK, 0);

        lv_label_set_text(s_wd_lbl_state, "IDLE");
        lv_obj_set_style_text_color(s_wd_lbl_state, COL_MUTED, 0);
        lv_label_set_text(s_wd_lbl_file, sd.mounted ? "Ready to record" : "No SD card fitted");
        lv_label_set_text(s_wd_lbl_time, "Time: 00:00:00");
        lv_label_set_text(s_wd_lbl_records, "0 rows");
    }

    /* 4 Protocol Counts */
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        store_stats_t st;
        store_stats((det_kind_t)i, &st);
        lv_label_set_text_fmt(s_wd_proto_val[i], "%lu", (unsigned long)st.unique);
        char buf[48];
        snprintf(buf, sizeof(buf), "+%lu/m  %lu hits",
                 (unsigned long)st.new_last_min, (unsigned long)st.hits);
        lv_label_set_text(s_wd_proto_sub[i], buf);
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
        lv_label_set_text(s_wd_lbl_gps_coords, "Lat: --  Lon: -- (Acquiring)");
        lv_label_set_text_fmt(s_wd_lbl_gps_stats, "%u sats tracked", (unsigned)gf.sats);
        lv_obj_set_style_text_color(s_wd_lbl_gps_coords, COL_MUTED, 0);
    }

    /* Storage & Hardware */
    if (sd.mounted) {
        lv_label_set_text_fmt(s_wd_lbl_storage_info, "SD: %llu MB Free\nTotal: %llu MB",
                              (unsigned long long)sd.free_mb, (unsigned long long)sd.card_size_mb);
    } else {
        lv_label_set_text(s_wd_lbl_storage_info, "SD: Missing\nLive Only Mode");
    }
}

/* ------------------------------------------------------------------ *
 *  BLE App (Devices, Chimera, Finder, GATT)                           *
 * ------------------------------------------------------------------ */

static void ble_tab_click_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_ble_active_tab = idx;
    for (int i = 0; i < 4; i++) {
        if (i == idx) {
            lv_obj_clear_flag(s_ble_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_ble_tab_btns[i], COL_BLE, 0);
        } else {
            lv_obj_add_flag(s_ble_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_ble_tab_btns[i], COL_BORDER, 0);
        }
    }
}

static void build_ble_app(lv_obj_t *cont)
{
    /* Top tab selector bar (4 tabs: Devices, Chimera, Finder, GATT) */
    static const char *tab_names[] = { "DEVICES", "CHIMERA", "FINDER", "GATT" };
    const lv_coord_t btn_w = (BSP_LCD_H_RES - 12 - 12) / 4;

    for (int i = 0; i < 4; i++) {
        s_ble_tab_btns[i] = lv_btn_create(cont);
        lv_obj_set_size(s_ble_tab_btns[i], btn_w, 26);
        lv_obj_set_pos(s_ble_tab_btns[i], 6 + i * (btn_w + 4), 4);
        lv_obj_set_style_bg_color(s_ble_tab_btns[i], COL_CARD, 0);
        lv_obj_set_style_border_color(s_ble_tab_btns[i], i == 0 ? COL_BLE : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_ble_tab_btns[i], 1, 0);
        lv_obj_set_style_radius(s_ble_tab_btns[i], 4, 0);
        lv_obj_add_event_cb(s_ble_tab_btns[i], ble_tab_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *l = mk_label(s_ble_tab_btns[i], &lv_font_montserrat_14, COL_TEXT, tab_names[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);

        s_ble_tabs[i] = lv_obj_create(cont);
        lv_obj_remove_style_all(s_ble_tabs[i]);
        lv_obj_set_pos(s_ble_tabs[i], 6, 34);
        lv_obj_set_size(s_ble_tabs[i], BSP_LCD_H_RES - 12, BODY_H - 38);
        lv_obj_set_scrollable(s_ble_tabs[i], false);
    }

    /* Tab 0: Devices list */
    lv_obj_t *dev_card = mk_card(s_ble_tabs[0], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_ble_list_name = mk_label(dev_card, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(s_ble_list_name, 0, 0);
    lv_obj_set_width(s_ble_list_name, 280);

    s_ble_list_rssi = mk_label(dev_card, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_ble_list_rssi, 290, 0);
    lv_obj_set_width(s_ble_list_rssi, 60);

    s_ble_list_type = mk_label(dev_card, &lv_font_montserrat_14, COL_BLE, "");
    lv_obj_set_pos(s_ble_list_type, 360, 0);
    lv_obj_set_width(s_ble_list_type, 90);

    /* Tab 1: Chimera Toolkit */
    lv_obj_t *stat_c = mk_card(s_ble_tabs[1], 0, 0, 160, BODY_H - 38, COL_CHIMERA);
    mk_label(stat_c, &lv_font_montserrat_14, COL_MUTED, "TOOLKIT MODE");
    s_ble_chimera_mode = mk_label(stat_c, &lv_font_montserrat_20, COL_TEXT, "IDLE");
    lv_obj_align(s_ble_chimera_mode, LV_ALIGN_LEFT_MID, 0, -20);

    s_ble_chimera_events = mk_label(stat_c, &lv_font_montserrat_14, COL_MUTED, "0 events");
    lv_obj_align(s_ble_chimera_events, LV_ALIGN_LEFT_MID, 0, 10);

    s_ble_chimera_status = mk_label(stat_c, &lv_font_montserrat_14, COL_MUTED, "Offline");
    lv_obj_align(s_ble_chimera_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *det_c = mk_card(s_ble_tabs[1], 168, 0, BSP_LCD_H_RES - 180, BODY_H - 38, COL_BORDER);
    s_ble_chimera_detail = mk_label(det_c, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_width(s_ble_chimera_detail, BSP_LCD_H_RES - 200);
    lv_label_set_long_mode(s_ble_chimera_detail, LV_LABEL_LONG_WRAP);

    /* Tab 2: Signal Finder / Foxhunt */
    lv_obj_t *find_c = mk_card(s_ble_tabs[2], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BLE);
    mk_label(find_c, &lv_font_montserrat_14, COL_MUTED, "TARGET SIGNAL TRACKER");

    s_ble_finder_target = mk_label(find_c, &lv_font_montserrat_16, COL_TEXT, "Target: Strongest Nearby Device");
    lv_obj_align(s_ble_finder_target, LV_ALIGN_TOP_LEFT, 0, 22);

    s_ble_finder_rssi = mk_label(find_c, &lv_font_montserrat_36, COL_BLE, "-- dBm");
    lv_obj_align(s_ble_finder_rssi, LV_ALIGN_LEFT_MID, 0, -10);

    s_ble_finder_peak = mk_label(find_c, &lv_font_montserrat_16, COL_OK, "Peak: -- dBm");
    lv_obj_align(s_ble_finder_peak, LV_ALIGN_LEFT_MID, 220, -10);

    s_ble_finder_bar = lv_bar_create(find_c);
    lv_obj_set_size(s_ble_finder_bar, BSP_LCD_H_RES - 36, 24);
    lv_obj_align(s_ble_finder_bar, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    lv_bar_set_range(s_ble_finder_bar, -100, -20);
    lv_bar_set_value(s_ble_finder_bar, -100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ble_finder_bar, COL_CARD_SUB, 0);
    lv_obj_set_style_bg_color(s_ble_finder_bar, COL_BLE, LV_PART_INDICATOR);

    /* Tab 3: GATT Inspector */
    lv_obj_t *gatt_c = mk_card(s_ble_tabs[3], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_ble_gatt_info = mk_label(gatt_c, &lv_font_montserrat_14, COL_TEXT, "GATT Inspector: Standby");
    lv_obj_set_width(s_ble_gatt_info, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_ble_gatt_info, LV_LABEL_LONG_WRAP);

    for (int i = 1; i < 4; i++) {
        lv_obj_add_flag(s_ble_tabs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_ble_app(void)
{
    if (s_ble_active_tab == 0) {
        /* Devices list */
        chimera_scan_item_t top[8];
        size_t n = chimera_scan_get_top(top, 8);
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
        if (n == 0) snprintf(cname, sizeof(cname), "No devices scanned yet");

        lv_label_set_text(s_ble_list_name, cname);
        lv_label_set_text(s_ble_list_rssi, crssi);
        lv_label_set_text(s_ble_list_type, ctype);

    } else if (s_ble_active_tab == 1) {
        /* Chimera Toolkit summary */
        chimera_status_t st;
        chimera_get_status(&st);

        static const char *MODE_NAMES[] = {
            "IDLE", "SCAN", "FINGERPRINT", "GATT ENUM", "MITM", "CLONE", "KEYSTROKE", "?"
        };
        const char *mn = (st.mode < CHIMERA_MODE_COUNT) ? MODE_NAMES[st.mode] : "?";
        lv_label_set_text(s_ble_chimera_mode, mn);
        lv_label_set_text_fmt(s_ble_chimera_events, "%lu events", (unsigned long)st.events);
        lv_label_set_text(s_ble_chimera_status, st.status);

        char detail[256];
        snprintf(detail, sizeof(detail),
                 "ChimeraBLE Toolkit\nActive: %s\nEvents: %lu\n\nReady for session dispatch.",
                 st.active ? "YES" : "NO", (unsigned long)st.events);
        lv_label_set_text(s_ble_chimera_detail, detail);

    } else if (s_ble_active_tab == 2) {
        /* Signal Finder */
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

    } else if (s_ble_active_tab == 3) {
        /* GATT Inspector */
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
}

/* ------------------------------------------------------------------ *
 *  Wi-Fi App (AP Scan, Channels Histogram, Signal Monitor)            *
 * ------------------------------------------------------------------ */

static void wifi_tab_click_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_wifi_active_tab = idx;
    for (int i = 0; i < 3; i++) {
        if (i == idx) {
            lv_obj_clear_flag(s_wifi_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_wifi_tab_btns[i], COL_WIFI, 0);
        } else {
            lv_obj_add_flag(s_wifi_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_wifi_tab_btns[i], COL_BORDER, 0);
        }
    }
}

static void build_wifi_app(lv_obj_t *cont)
{
    static const char *tab_names[] = { "AP SCANNER", "CHANNELS", "MONITOR" };
    const lv_coord_t btn_w = (BSP_LCD_H_RES - 12 - 8) / 3;

    for (int i = 0; i < 3; i++) {
        s_wifi_tab_btns[i] = lv_btn_create(cont);
        lv_obj_set_size(s_wifi_tab_btns[i], btn_w, 26);
        lv_obj_set_pos(s_wifi_tab_btns[i], 6 + i * (btn_w + 4), 4);
        lv_obj_set_style_bg_color(s_wifi_tab_btns[i], COL_CARD, 0);
        lv_obj_set_style_border_color(s_wifi_tab_btns[i], i == 0 ? COL_WIFI : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_wifi_tab_btns[i], 1, 0);
        lv_obj_set_style_radius(s_wifi_tab_btns[i], 4, 0);
        lv_obj_add_event_cb(s_wifi_tab_btns[i], wifi_tab_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *l = mk_label(s_wifi_tab_btns[i], &lv_font_montserrat_14, COL_TEXT, tab_names[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);

        s_wifi_tabs[i] = lv_obj_create(cont);
        lv_obj_remove_style_all(s_wifi_tabs[i]);
        lv_obj_set_pos(s_wifi_tabs[i], 6, 34);
        lv_obj_set_size(s_wifi_tabs[i], BSP_LCD_H_RES - 12, BODY_H - 38);
        lv_obj_set_scrollable(s_wifi_tabs[i], false);
    }

    /* Tab 0: AP List */
    lv_obj_t *list_c = mk_card(s_wifi_tabs[0], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_wifi_list_ssid = mk_label(list_c, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(s_wifi_list_ssid, 0, 0);
    lv_obj_set_width(s_wifi_list_ssid, 260);

    s_wifi_list_rssi = mk_label(list_c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_wifi_list_rssi, 270, 0);
    lv_obj_set_width(s_wifi_list_rssi, 54);

    s_wifi_list_ch = mk_label(list_c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(s_wifi_list_ch, 330, 0);
    lv_obj_set_width(s_wifi_list_ch, 36);

    s_wifi_list_auth = mk_label(list_c, &lv_font_montserrat_14, COL_WIFI, "");
    lv_obj_set_pos(s_wifi_list_auth, 370, 0);
    lv_obj_set_width(s_wifi_list_auth, 80);

    /* Tab 1: Channels Chart */
    lv_obj_t *ch_c = mk_card(s_wifi_tabs[1], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    mk_label(ch_c, &lv_font_montserrat_14, COL_MUTED, "2.4 GHz SPECTRUM OCCUPANCY (CHANNELS 1..14)");
    s_wifi_chart = lv_chart_create(ch_c);
    lv_obj_remove_style_all(s_wifi_chart);
    lv_obj_set_size(s_wifi_chart, BSP_LCD_H_RES - 36, BODY_H - 74);
    lv_obj_set_pos(s_wifi_chart, 0, 24);
    lv_chart_set_type(s_wifi_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_wifi_chart, 14);
    lv_obj_set_style_pad_column(s_wifi_chart, 6, 0);
    s_wifi_chart_ser = lv_chart_add_series(s_wifi_chart, COL_WIFI, LV_CHART_AXIS_PRIMARY_Y);

    /* Tab 2: Signal Monitor */
    lv_obj_t *mon_c = mk_card(s_wifi_tabs[2], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_WIFI);
    mk_label(mon_c, &lv_font_montserrat_14, COL_MUTED, "ACTIVE AP SIGNAL MONITOR");
    s_wifi_mon_ssid = mk_label(mon_c, &lv_font_montserrat_20, COL_TEXT, "Searching...");
    lv_obj_align(s_wifi_mon_ssid, LV_ALIGN_TOP_LEFT, 0, 24);

    s_wifi_mon_detail = mk_label(mon_c, &lv_font_montserrat_16, COL_MUTED, "RSSI: -- dBm");
    lv_obj_align(s_wifi_mon_detail, LV_ALIGN_LEFT_MID, 0, 0);

    s_wifi_mon_bar = lv_bar_create(mon_c);
    lv_obj_set_size(s_wifi_mon_bar, BSP_LCD_H_RES - 36, 26);
    lv_obj_align(s_wifi_mon_bar, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    lv_bar_set_range(s_wifi_mon_bar, -100, -20);
    lv_bar_set_value(s_wifi_mon_bar, -100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_wifi_mon_bar, COL_CARD_SUB, 0);
    lv_obj_set_style_bg_color(s_wifi_mon_bar, COL_WIFI, LV_PART_INDICATOR);

    for (int i = 1; i < 3; i++) {
        lv_obj_add_flag(s_wifi_tabs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_wifi_app(void)
{
    if (s_wifi_active_tab == 0) {
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

        lv_label_set_text(s_wifi_list_ssid, cssid);
        lv_label_set_text(s_wifi_list_rssi, crssi);
        lv_label_set_text(s_wifi_list_ch, cch);
        lv_label_set_text(s_wifi_list_auth, cauth);

    } else if (s_wifi_active_tab == 1) {
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

    } else if (s_wifi_active_tab == 2) {
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
}

/* ------------------------------------------------------------------ *
 *  Matter App (BLE Commissioning, mDNS, Node Details)                 *
 * ------------------------------------------------------------------ */

static void matter_tab_click_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_matter_active_tab = idx;
    for (int i = 0; i < 3; i++) {
        if (i == idx) {
            lv_obj_clear_flag(s_matter_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_matter_tab_btns[i], COL_MATTER, 0);
        } else {
            lv_obj_add_flag(s_matter_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_matter_tab_btns[i], COL_BORDER, 0);
        }
    }
}

static void build_matter_app(lv_obj_t *cont)
{
    static const char *tab_names[] = { "BLE COMM", "mDNS OPER", "DETAILS" };
    const lv_coord_t btn_w = (BSP_LCD_H_RES - 12 - 8) / 3;

    for (int i = 0; i < 3; i++) {
        s_matter_tab_btns[i] = lv_btn_create(cont);
        lv_obj_set_size(s_matter_tab_btns[i], btn_w, 26);
        lv_obj_set_pos(s_matter_tab_btns[i], 6 + i * (btn_w + 4), 4);
        lv_obj_set_style_bg_color(s_matter_tab_btns[i], COL_CARD, 0);
        lv_obj_set_style_border_color(s_matter_tab_btns[i], i == 0 ? COL_MATTER : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_matter_tab_btns[i], 1, 0);
        lv_obj_set_style_radius(s_matter_tab_btns[i], 4, 0);
        lv_obj_add_event_cb(s_matter_tab_btns[i], matter_tab_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *l = mk_label(s_matter_tab_btns[i], &lv_font_montserrat_14, COL_TEXT, tab_names[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);

        s_matter_tabs[i] = lv_obj_create(cont);
        lv_obj_remove_style_all(s_matter_tabs[i]);
        lv_obj_set_pos(s_matter_tabs[i], 6, 34);
        lv_obj_set_size(s_matter_tabs[i], BSP_LCD_H_RES - 12, BODY_H - 38);
        lv_obj_set_scrollable(s_matter_tabs[i], false);
    }

    /* Tab 0: BLE Commissioning */
    lv_obj_t *ble_c = mk_card(s_matter_tabs[0], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_matter_ble_list = mk_label(ble_c, &lv_font_montserrat_14, COL_TEXT, "Waiting for Matter BLE adverts...");
    lv_obj_set_width(s_matter_ble_list, BSP_LCD_H_RES - 36);

    /* Tab 1: mDNS Discovery */
    lv_obj_t *mdns_c = mk_card(s_matter_tabs[1], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_matter_mdns_list = mk_label(mdns_c, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(s_matter_mdns_list, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_matter_mdns_list, LV_LABEL_LONG_WRAP);

    /* Tab 2: Node Details */
    lv_obj_t *det_c = mk_card(s_matter_tabs[2], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_MATTER);
    s_matter_detail = mk_label(det_c, &lv_font_montserrat_14, COL_TEXT, "Select a Matter node for details.");
    lv_obj_set_width(s_matter_detail, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_matter_detail, LV_LABEL_LONG_WRAP);

    for (int i = 1; i < 3; i++) {
        lv_obj_add_flag(s_matter_tabs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_matter_app(void)
{
    static detection_t rows[LIST_ROWS];
    size_t n = store_snapshot(DET_MATTER, rows, LIST_ROWS, false);

    if (s_matter_active_tab == 0) {
        char buf[512] = "";
        size_t off = 0;
        size_t ble_count = 0;
        for (size_t i = 0; i < n; i++) {
            if (rows[i].x.matter.via == 1) { /* BLE */
                ble_count++;
                off += snprintf(buf + off, sizeof(buf) - off,
                                "Node: %02X:%02X:%02X:%02X:%02X:%02X  Disc: %u  VID: 0x%04X\n",
                                rows[i].mac[0], rows[i].mac[1], rows[i].mac[2],
                                rows[i].mac[3], rows[i].mac[4], rows[i].mac[5],
                                (unsigned)rows[i].x.matter.discriminator,
                                (unsigned)rows[i].x.matter.vendor_id);
            }
        }
        if (ble_count == 0) {
            snprintf(buf, sizeof(buf), "No Commissionable BLE nodes detected nearby.");
        }
        lv_label_set_text(s_matter_ble_list, buf);

    } else if (s_matter_active_tab == 1) {
        net_link_status_t ln;
        net_link_status(&ln);
        if (!ln.wifi_up) {
            lv_label_set_text(s_matter_mdns_list,
                              "mDNS Operational Discovery: UNAVAILABLE\n\n"
                              "Reason: No local Wi-Fi network association.\n"
                              "mDNS scanning requires the host to join a Wi-Fi LAN\n"
                              "in order to discover operational Matter nodes over IP multicast.");
            lv_obj_set_style_text_color(s_matter_mdns_list, COL_WARN, 0);
        } else {
            char buf[512] = "";
            size_t off = 0;
            size_t mdns_count = 0;
            for (size_t i = 0; i < n; i++) {
                if (rows[i].x.matter.via > 1) { /* mDNS */
                    mdns_count++;
                    off += snprintf(buf + off, sizeof(buf) - off,
                                    "Node: %s  VID: 0x%04X  PID: 0x%04X\n",
                                    rows[i].name[0] ? rows[i].name : "Matter Device",
                                    (unsigned)rows[i].x.matter.vendor_id,
                                    (unsigned)rows[i].x.matter.product_id);
                }
            }
            if (mdns_count == 0) {
                snprintf(buf, sizeof(buf), "Wi-Fi link active. Listening for _matter._tcp records...");
            }
            lv_label_set_text(s_matter_mdns_list, buf);
            lv_obj_set_style_text_color(s_matter_mdns_list, COL_TEXT, 0);
        }

    } else if (s_matter_active_tab == 2) {
        if (n > 0) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "Latest Observed Matter Device:\n\n"
                     "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n"
                     "Vendor ID: 0x%04X  Product ID: 0x%04X\n"
                     "Discriminator: %u\n"
                     "Transport: %s\n"
                     "Total Sightings: %lu",
                     rows[0].mac[0], rows[0].mac[1], rows[0].mac[2],
                     rows[0].mac[3], rows[0].mac[4], rows[0].mac[5],
                     (unsigned)rows[0].x.matter.vendor_id,
                     (unsigned)rows[0].x.matter.product_id,
                     (unsigned)rows[0].x.matter.discriminator,
                     rows[0].x.matter.via == 1 ? "BLE Advertisement" : "mDNS Operational",
                     (unsigned long)rows[0].hits);
            lv_label_set_text(s_matter_detail, buf);
        } else {
            lv_label_set_text(s_matter_detail, "No Matter devices observed yet.");
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Zigbee App (802.15.4 Observations, Channels, Link Health)          *
 * ------------------------------------------------------------------ */

static void zb_tab_click_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_zb_active_tab = idx;
    for (int i = 0; i < 3; i++) {
        if (i == idx) {
            lv_obj_clear_flag(s_zb_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_zb_tab_btns[i], COL_ZIGBEE, 0);
        } else {
            lv_obj_add_flag(s_zb_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(s_zb_tab_btns[i], COL_BORDER, 0);
        }
    }
}

static void build_zigbee_app(lv_obj_t *cont)
{
    static const char *tab_names[] = { "802.15.4 FRAMES", "CHANNELS", "LINK HEALTH" };
    const lv_coord_t btn_w = (BSP_LCD_H_RES - 12 - 8) / 3;

    for (int i = 0; i < 3; i++) {
        s_zb_tab_btns[i] = lv_btn_create(cont);
        lv_obj_set_size(s_zb_tab_btns[i], btn_w, 26);
        lv_obj_set_pos(s_zb_tab_btns[i], 6 + i * (btn_w + 4), 4);
        lv_obj_set_style_bg_color(s_zb_tab_btns[i], COL_CARD, 0);
        lv_obj_set_style_border_color(s_zb_tab_btns[i], i == 0 ? COL_ZIGBEE : COL_BORDER, 0);
        lv_obj_set_style_border_width(s_zb_tab_btns[i], 1, 0);
        lv_obj_set_style_radius(s_zb_tab_btns[i], 4, 0);
        lv_obj_add_event_cb(s_zb_tab_btns[i], zb_tab_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *l = mk_label(s_zb_tab_btns[i], &lv_font_montserrat_14, COL_TEXT, tab_names[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);

        s_zb_tabs[i] = lv_obj_create(cont);
        lv_obj_remove_style_all(s_zb_tabs[i]);
        lv_obj_set_pos(s_zb_tabs[i], 6, 34);
        lv_obj_set_size(s_zb_tabs[i], BSP_LCD_H_RES - 12, BODY_H - 38);
        lv_obj_set_scrollable(s_zb_tabs[i], false);
    }

    /* Tab 0: Frames List */
    lv_obj_t *f_card = mk_card(s_zb_tabs[0], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_zb_frames_list = mk_label(f_card, &lv_font_montserrat_14, COL_TEXT, "Waiting for 802.15.4 frames...");
    lv_obj_set_width(s_zb_frames_list, BSP_LCD_H_RES - 36);

    /* Tab 1: Channels & PANs */
    lv_obj_t *pan_card = mk_card(s_zb_tabs[1], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_BORDER);
    s_zb_pan_list = mk_label(pan_card, &lv_font_montserrat_14, COL_TEXT, "Scanning channels 11..26...");
    lv_obj_set_width(s_zb_pan_list, BSP_LCD_H_RES - 36);

    /* Tab 2: Link Diagnostics */
    lv_obj_t *diag_card = mk_card(s_zb_tabs[2], 0, 0, BSP_LCD_H_RES - 12, BODY_H - 38, COL_ZIGBEE);
    s_zb_diag_info = mk_label(diag_card, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_width(s_zb_diag_info, BSP_LCD_H_RES - 36);
    lv_label_set_long_mode(s_zb_diag_info, LV_LABEL_LONG_WRAP);

    for (int i = 1; i < 3; i++) {
        lv_obj_add_flag(s_zb_tabs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_zigbee_app(void)
{
    if (s_zb_active_tab == 0) {
        static detection_t rows[LIST_ROWS];
        size_t n = store_snapshot(DET_ZIGBEE, rows, LIST_ROWS, true);
        char buf[512] = "";
        size_t off = 0;
        for (size_t i = 0; i < n; i++) {
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

    } else if (s_zb_active_tab == 1) {
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

    } else if (s_zb_active_tab == 2) {
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
}

/* ------------------------------------------------------------------ *
 *  Main Timer Refresh Callback                                        *
 * ------------------------------------------------------------------ */

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
        /* Launcher is static, nothing to poll */
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
            ui_dashboard_show((s_dash_screen + 1) % UI_DASH_COUNT);
            s_last_touch_ms = now_ms();
        }
        break;

    case UI_VIEW_WARDRIVE:
        update_wardrive_app();
        break;

    case UI_VIEW_BLE:
        update_ble_app();
        break;

    case UI_VIEW_WIFI:
        update_wifi_app();
        break;

    case UI_VIEW_MATTER:
        update_matter_app();
        break;

    case UI_VIEW_ZIGBEE:
        update_zigbee_app();
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

    build_launcher(s_view_cont[UI_VIEW_LAUNCHER]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after Launcher: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    build_dashboard_app(s_view_cont[UI_VIEW_DASHBOARD]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after Dashboard: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    build_wardrive_app(s_view_cont[UI_VIEW_WARDRIVE]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after Wardrive: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    build_ble_app(s_view_cont[UI_VIEW_BLE]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after BLE: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    build_wifi_app(s_view_cont[UI_VIEW_WIFI]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after Wi-Fi: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    build_matter_app(s_view_cont[UI_VIEW_MATTER]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after Matter: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

    build_zigbee_app(s_view_cont[UI_VIEW_ZIGBEE]);
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "Heap after Zigbee: %u free (used %u%%)", (unsigned)mon.free_size, (unsigned)mon.used_pct);

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
