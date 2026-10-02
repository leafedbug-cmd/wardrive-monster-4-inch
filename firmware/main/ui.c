#include "ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp_pins.h"
#include "c6_ota.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gps.h"
#include "net_link.h"
#include "scanners.h"
#include "sdlog.h"
#include "store.h"

static const char *TAG = "ui";

/* ------------------------------------------------------------------ *
 *  Palette -- dark and high contrast, for reading in a moving car.    *
 * ------------------------------------------------------------------ */
#define COL_BG        lv_color_hex(0x080B10)
#define COL_CARD      lv_color_hex(0x141A23)
#define COL_EDGE      lv_color_hex(0x263142)
#define COL_TEXT      lv_color_hex(0xE6EAF0)
#define COL_MUTED     lv_color_hex(0x7A8699)
#define COL_WIFI      lv_color_hex(0x22D3EE)
#define COL_BLE       lv_color_hex(0x60A5FA)
#define COL_MATTER    lv_color_hex(0xA78BFA)
#define COL_ZIGBEE    lv_color_hex(0xFBBF24)
#define COL_GPS       lv_color_hex(0x34D399)
#define COL_OK        lv_color_hex(0x4ADE80)
#define COL_WARN      lv_color_hex(0xFB923C)
#define COL_BAD       lv_color_hex(0xF87171)

#define HEADER_H   26
#define FOOTER_H   22
#define BODY_H     (BSP_LCD_V_RES - HEADER_H - FOOTER_H)

#define LIST_ROWS  10
#define REFRESH_MS 1000

static const char *SCREEN_NAME[UI_SCREEN_COUNT] = {
    "COMBINED", "WI-FI", "BLE", "MATTER", "ZIGBEE", "GPS"
};

static lv_color_t screen_color(ui_screen_t s)
{
    switch (s) {
    case UI_SCREEN_WIFI:   return COL_WIFI;
    case UI_SCREEN_BLE:    return COL_BLE;
    case UI_SCREEN_MATTER: return COL_MATTER;
    case UI_SCREEN_ZIGBEE: return COL_ZIGBEE;
    case UI_SCREEN_GPS:    return COL_GPS;
    default:               return COL_TEXT;
    }
}

/* ------------------------------------------------------------------ */

static lv_obj_t *s_tiles;
static lv_obj_t *s_tile[UI_SCREEN_COUNT];
static lv_obj_t *s_hdr_title, *s_hdr_link, *s_hdr_sd, *s_hdr_clock;
static lv_obj_t *s_dots[UI_SCREEN_COUNT];

/* combined */
static lv_obj_t *s_tot_value, *s_tot_rate, *s_tot_sub;
static lv_obj_t *s_card_val[DET_KIND_COUNT], *s_card_sub[DET_KIND_COUNT];

/* per-protocol screens */
static lv_obj_t *s_big[UI_SCREEN_COUNT];
static lv_obj_t *s_meta[UI_SCREEN_COUNT];
static lv_obj_t *s_list[UI_SCREEN_COUNT];      /* name column      */
static lv_obj_t *s_lc_rssi[UI_SCREEN_COUNT];   /* dBm, right-aligned */
static lv_obj_t *s_lc_ch[UI_SCREEN_COUNT];     /* channel            */
static lv_obj_t *s_lc_age[UI_SCREEN_COUNT];    /* age                */
static lv_obj_t *s_chart;

/* GPS screen */
static lv_obj_t *s_gps_sats, *s_gps_meta, *s_gps_lat, *s_gps_lon,
                *s_gps_state;
static lv_chart_series_t *s_chart_ser;

static uint16_t  s_autocycle_s;
static int64_t   s_last_touch_ms;
static ui_screen_t s_active;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------------ *
 *  Small helpers                                                      *
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
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_border_color(c, edge, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    /* Cards are decoration. lv_obj_create() makes objects CLICKABLE by
     * default, and a clickable child swallows the press that starts a
     * swipe instead of letting it reach the tileview -- so a screen whose
     * cards happen to cover where your thumb lands simply will not swipe.
     * Nothing here needs to receive input, so take the flag away. */
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

/* "-54" / "--" when the source carries no signal strength. */
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

    /* Clamped on both ends so the field is always exactly three columns --
     * and so the compiler can see that it is. */
    if (v < 0)  { v = 0;  }
    if (v > 99) { v = 99; }

    snprintf(buf, n, "%2d%c", (int)v, unit);
}

/* ------------------------------------------------------------------ *
 *  Header / footer                                                    *
 * ------------------------------------------------------------------ */

static void build_chrome(lv_obj_t *scr)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, BSP_LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(hdr, COL_CARD, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(hdr, false);

    s_hdr_title = mk_label(hdr, &lv_font_montserrat_16, COL_TEXT, "COMBINED");
    lv_obj_align(s_hdr_title, LV_ALIGN_LEFT_MID, 8, 0);

    s_hdr_clock = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "0m");
    lv_obj_align(s_hdr_clock, LV_ALIGN_CENTER, 0, 0);

    /* Both status labels get a FIXED width and right-aligned text.
     *
     * The previous version aligned the link label relative to the SD label
     * with lv_obj_align_to(). That resolves once, at build time, against
     * whatever width the SD label happened to have then -- so the moment
     * "SD --" grew into "SD 29512M" the two labels sat on top of each
     * other. Fixed boxes cannot drift no matter what the text does. */
    s_hdr_sd = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "SD --");
    lv_obj_set_width(s_hdr_sd, 76);
    lv_obj_set_style_text_align(s_hdr_sd, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_hdr_sd, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_hdr_sd, LV_ALIGN_RIGHT_MID, -8, 0);

    s_hdr_link = mk_label(hdr, &lv_font_montserrat_14, COL_MUTED, "C6 --");
    lv_obj_set_width(s_hdr_link, 74);
    lv_obj_set_style_text_align(s_hdr_link, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_hdr_link, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_hdr_link, LV_ALIGN_RIGHT_MID, -90, 0);

    /* Footer: one dot per screen, the active one filled. */
    lv_obj_t *ftr = lv_obj_create(scr);
    lv_obj_remove_style_all(ftr);
    lv_obj_set_pos(ftr, 0, BSP_LCD_V_RES - FOOTER_H);
    lv_obj_set_size(ftr, BSP_LCD_H_RES, FOOTER_H);
    lv_obj_set_style_bg_color(ftr, COL_CARD, 0);
    lv_obj_set_style_bg_opa(ftr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(ftr, false);

    const lv_coord_t gap = 18;
    const lv_coord_t x0 = BSP_LCD_H_RES / 2 - (UI_SCREEN_COUNT - 1) * gap / 2;
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(ftr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, COL_EDGE, 0);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, x0 + i * gap, 0);
        s_dots[i] = d;
    }
}

/* ------------------------------------------------------------------ *
 *  Screen 0 -- COMBINED                                               *
 * ------------------------------------------------------------------ */

static void build_combined(lv_obj_t *t)
{
    lv_obj_t *hero = mk_card(t, 6, 4, 190, BODY_H - 8, COL_EDGE);

    mk_label(hero, &lv_font_montserrat_14, COL_MUTED, "TOTAL UNIQUE");

    s_tot_value = mk_label(hero, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_tot_value, LV_ALIGN_LEFT_MID, 0, -14);

    s_tot_rate = mk_label(hero, &lv_font_montserrat_16, COL_OK, "0 / min");
    lv_obj_align(s_tot_rate, LV_ALIGN_LEFT_MID, 0, 18);

    s_tot_sub = mk_label(hero, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_tot_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Four protocol cards in a 2x2 grid on the right. */
    static const char *titles[DET_KIND_COUNT] = { "WI-FI", "BLE", "MATTER", "ZIGBEE" };
    const lv_color_t colors[DET_KIND_COUNT] = { COL_WIFI, COL_BLE, COL_MATTER, COL_ZIGBEE };

    const lv_coord_t cw = 134, ch = (BODY_H - 14) / 2;
    for (int i = 0; i < DET_KIND_COUNT; i++) {
        lv_coord_t x = 202 + (i % 2) * (cw + 6);
        lv_coord_t y = 4 + (i / 2) * (ch + 6);
        lv_obj_t *c = mk_card(t, x, y, cw, ch, colors[i]);

        lv_obj_t *ttl = mk_label(c, &lv_font_montserrat_14, colors[i], titles[i]);
        lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

        s_card_val[i] = mk_label(c, &lv_font_montserrat_28, COL_TEXT, "0");
        lv_obj_align(s_card_val[i], LV_ALIGN_LEFT_MID, 0, 2);

        s_card_sub[i] = mk_label(c, &lv_font_montserrat_14, COL_MUTED, "");
        lv_obj_align(s_card_sub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void update_combined(void)
{
    store_stats_t tot;
    store_stats_total(&tot);

    char buf[96];
    lv_label_set_text_fmt(s_tot_value, "%lu", (unsigned long)tot.unique);
    lv_label_set_text_fmt(s_tot_rate, "%lu / min", (unsigned long)tot.new_last_min);
    lv_obj_set_style_text_color(s_tot_rate,
                                tot.new_last_min ? COL_OK : COL_MUTED, 0);

    sdlog_status_t sd;
    sdlog_status(&sd);
    snprintf(buf, sizeof(buf), "%lu logged\n%lu dropped\ntable %u%% full",
             (unsigned long)sd.written, (unsigned long)sd.dropped,
             store_fill_pct());
    lv_label_set_text(s_tot_sub, buf);

    for (int i = 0; i < DET_KIND_COUNT; i++) {
        store_stats_t st;
        store_stats((det_kind_t)i, &st);
        lv_label_set_text_fmt(s_card_val[i], "%lu", (unsigned long)st.unique);

        scanner_status_t sc = {0};
        switch (i) {
        case DET_WIFI:   scan_wifi_status(&sc);   break;
        case DET_BLE:    scan_ble_status(&sc);    break;
        case DET_MATTER: scan_matter_status(&sc); break;
        default:         scan_zigbee_status(&sc); break;
        }

        if (sc.state == SCAN_UNAVAILABLE) {
            lv_label_set_text(s_card_sub[i], "unavailable");
            lv_obj_set_style_text_color(s_card_sub[i], COL_BAD, 0);
        } else {
            snprintf(buf, sizeof(buf), "+%lu/min", (unsigned long)st.new_last_min);
            lv_label_set_text(s_card_sub[i], buf);
            lv_obj_set_style_text_color(s_card_sub[i], COL_MUTED, 0);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Screens 1-3 -- one protocol each                                   *
 * ------------------------------------------------------------------ */

static const char *by_rssi_title(ui_screen_t which)
{
    /* Matter is ordered by recency because mDNS rows carry no RSSI. */
    return (which == UI_SCREEN_MATTER) ? "NEWEST" : "STRONGEST";
}

/* Layout, for a 480x320 landscape body of BODY_H px.
 *
 * The left column is three stacked cards rather than one. The old single
 * card put a 36 px number and four lines of meta inside 92 px of usable
 * height and let them collide -- which is what the Wi-Fi screen was doing,
 * printing the unique count straight through "+8/min".
 *
 *      x=6   w=152                    x=164  w=310
 *      +-------------+                +---------------------------+
 *      | UNIQUE  nnn |  y=4   h=86    |  list, 4 aligned columns  |
 *      +-------------+                |                           |
 *      | meta lines  |  y=94          |                           |
 *      +-------------+                |                           |
 *      | chart       |  y=172 h=96    |                           |
 *      +-------------+                +---------------------------+
 */
#define COL_L_X      6
#define COL_L_W      152
#define COL_R_X      164
#define COL_R_W      (BSP_LCD_H_RES - COL_R_X - 6)

#define STAT_Y       4
#define STAT_H       86
#define META_Y       94
#define META_H_CHART 74
#define META_H_PLAIN (BODY_H - META_Y - 4)
#define CHART_Y      172
#define CHART_H      (BODY_H - CHART_Y - 4)

/* Column offsets inside the list card, which has 6 px padding each side. */
#define LCOL_NAME_W  168
#define LCOL_RSSI_X  170
#define LCOL_RSSI_W  46
#define LCOL_CH_X    218
#define LCOL_CH_W    32
#define LCOL_AGE_X   252
#define LCOL_AGE_W   46
#define LCOL_ROW1_Y  22

static lv_obj_t *mk_col(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_text_align_t align, lv_color_t colour)
{
    lv_obj_t *l = mk_label(parent, &lv_font_montserrat_14, colour, "");
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

static void build_protocol(lv_obj_t *t, ui_screen_t which, bool with_chart)
{
    const lv_color_t col = screen_color(which);

    /* --- stat card: the headline number, and nothing else --- */
    lv_obj_t *stat = mk_card(t, COL_L_X, STAT_Y, COL_L_W, STAT_H, col);
    lv_obj_t *cap = mk_label(stat, &lv_font_montserrat_14, COL_MUTED, "UNIQUE");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    s_big[which] = mk_label(stat, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_big[which], LV_ALIGN_BOTTOM_LEFT, 0, 2);

    /* --- meta card --- *
     * Wraps rather than clips: these strings genuinely do not fit on one
     * line, which is why "mDNS 0 commissionable" was rendering as
     * "mDNS 0 commissio". */
    lv_obj_t *metac = mk_card(t, COL_L_X, META_Y, COL_L_W,
                              with_chart ? META_H_CHART : META_H_PLAIN,
                              COL_EDGE);
    s_meta[which] = mk_label(metac, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(s_meta[which], COL_L_W - 14);
    lv_label_set_long_mode(s_meta[which], LV_LABEL_LONG_WRAP);
    lv_obj_align(s_meta[which], LV_ALIGN_TOP_LEFT, 0, 0);

    /* --- chart card, Wi-Fi only --- */
    if (with_chart) {
        lv_obj_t *cc = mk_card(t, COL_L_X, CHART_Y, COL_L_W, CHART_H, col);
        lv_obj_t *lab = mk_label(cc, &lv_font_montserrat_14, COL_MUTED,
                                 "CHANNELS 1-14");
        lv_obj_align(lab, LV_ALIGN_TOP_LEFT, 0, 0);

        s_chart = lv_chart_create(cc);
        lv_obj_set_size(s_chart, COL_L_W - 14, CHART_H - 36);
        lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(s_chart, 14);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
        lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_chart, 0, 0);
        lv_obj_set_style_pad_all(s_chart, 0, 0);
        /* Without a column gap the 14 bars touch and read as one block. */
        lv_obj_set_style_pad_column(s_chart, 2, LV_PART_MAIN);
        lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
        lv_chart_set_div_line_count(s_chart, 0, 0);
        lv_obj_set_scrollbar_mode(s_chart, LV_SCROLLBAR_MODE_OFF);
        s_chart_ser = lv_chart_add_series(s_chart, col, LV_CHART_AXIS_PRIMARY_Y);
    }

    /* --- list card: four independently aligned columns --- *
     * Not one space-padded block. Montserrat is proportional, so padding
     * with spaces lines nothing up: a row of "1"s is far narrower than a
     * row of "W"s, and the dBm/CH/AGE columns wander across the card.
     * Four fixed-width labels cannot wander. */
    lv_obj_t *lc = mk_card(t, COL_R_X, 4, COL_R_W, BODY_H - 8, COL_EDGE);

    lv_obj_t *h_name = mk_col(lc, 0, 0, LCOL_NAME_W, LV_TEXT_ALIGN_LEFT, COL_MUTED);
    lv_obj_t *h_rssi = mk_col(lc, LCOL_RSSI_X, 0, LCOL_RSSI_W, LV_TEXT_ALIGN_RIGHT, COL_MUTED);
    lv_obj_t *h_ch   = mk_col(lc, LCOL_CH_X,   0, LCOL_CH_W,   LV_TEXT_ALIGN_RIGHT, COL_MUTED);
    lv_obj_t *h_age  = mk_col(lc, LCOL_AGE_X,  0, LCOL_AGE_W,  LV_TEXT_ALIGN_RIGHT, COL_MUTED);
    lv_label_set_text(h_name, by_rssi_title(which));
    lv_label_set_text(h_rssi, "dBm");
    lv_label_set_text(h_ch,   "CH");
    lv_label_set_text(h_age,  "AGE");

    s_list[which]    = mk_col(lc, 0,           LCOL_ROW1_Y, LCOL_NAME_W, LV_TEXT_ALIGN_LEFT,  COL_TEXT);
    s_lc_rssi[which] = mk_col(lc, LCOL_RSSI_X, LCOL_ROW1_Y, LCOL_RSSI_W, LV_TEXT_ALIGN_RIGHT, COL_TEXT);
    s_lc_ch[which]   = mk_col(lc, LCOL_CH_X,   LCOL_ROW1_Y, LCOL_CH_W,   LV_TEXT_ALIGN_RIGHT, COL_TEXT);
    s_lc_age[which]  = mk_col(lc, LCOL_AGE_X,  LCOL_ROW1_Y, LCOL_AGE_W,  LV_TEXT_ALIGN_RIGHT, COL_TEXT);
}

static void update_protocol(ui_screen_t which, det_kind_t kind, bool by_rssi)
{
    store_stats_t st;
    store_stats(kind, &st);
    lv_label_set_text_fmt(s_big[which], "%lu", (unsigned long)st.unique);

    scanner_status_t sc = {0};
    switch (kind) {
    case DET_WIFI:   scan_wifi_status(&sc);   break;
    case DET_BLE:    scan_ble_status(&sc);    break;
    case DET_MATTER: scan_matter_status(&sc); break;
    default:         scan_zigbee_status(&sc); break;
    }

    char meta[160];
    char best[8];
    fmt_rssi(best, sizeof(best), st.best_rssi == -128 ? DET_RSSI_NA : st.best_rssi);
    snprintf(meta, sizeof(meta), "+%lu/min  %lu hits\nbest %s dBm\n%s",
             (unsigned long)st.new_last_min, (unsigned long)st.hits,
             best, sc.detail);
    lv_label_set_text(s_meta[which], meta);
    lv_obj_set_style_text_color(s_meta[which],
                                sc.state == SCAN_UNAVAILABLE ? COL_BAD : COL_MUTED, 0);

    /* One buffer per column, newline separated, so every row shares a
     * baseline across all four labels. */
    static detection_t rows[LIST_ROWS];
    size_t n = store_snapshot(kind, rows, LIST_ROWS, by_rssi);

    char cname[LIST_ROWS * 24 + 16];
    char crssi[LIST_ROWS * 8 + 16];
    char cch[LIST_ROWS * 6 + 16];
    char cage[LIST_ROWS * 8 + 16];
    size_t on = 0, orssi = 0, och = 0, oage = 0;

    cname[0] = crssi[0] = cch[0] = cage[0] = '\0';

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
            och += snprintf(cch + och, sizeof(cch) - och, "%u\n",
                            (unsigned)rows[i].channel);
        } else {
            och += snprintf(cch + och, sizeof(cch) - och, "-\n");
        }

        oage += snprintf(cage + oage, sizeof(cage) - oage, "%s\n", age);
    }

    if (n == 0) {
        snprintf(cname, sizeof(cname), "nothing yet");
    }

    lv_label_set_text(s_list[which],    cname);
    lv_label_set_text(s_lc_rssi[which], crssi);
    lv_label_set_text(s_lc_ch[which],   cch);
    lv_label_set_text(s_lc_age[which],  cage);
}

static void update_wifi(void)
{
    update_protocol(UI_SCREEN_WIFI, DET_WIFI, true);

    uint16_t hist[14];
    store_wifi_channel_hist(hist);
    uint16_t peak = 1;
    for (int i = 0; i < 14; i++) {
        if (hist[i] > peak) {
            peak = hist[i];
        }
    }
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, peak);
    for (int i = 0; i < 14; i++) {
        lv_chart_set_value_by_id(s_chart, s_chart_ser, i, hist[i]);
    }
}

/* ------------------------------------------------------------------ *
 *  Screen 4 -- ZIGBEE                                                 *
 * ------------------------------------------------------------------ */

static void build_zigbee(lv_obj_t *t)
{
    /* Same layout as the other protocol screens. It used to be a wall of
     * text explaining why it could never have data; the external C6 on the
     * header is that data source, so this is a real list now. When none is
     * fitted the list is empty and the meta block says why -- which is the
     * same way every other screen reports a dead source. */
    build_protocol(t, UI_SCREEN_ZIGBEE, false);
}

static void update_zigbee(void)
{
    update_protocol(UI_SCREEN_ZIGBEE, DET_ZIGBEE, true);
}


/* ------------------------------------------------------------------ *
 *  Screen 5 -- GPS                                                    *
 * ------------------------------------------------------------------ */

static void build_gps(lv_obj_t *t)
{
    /* Satellite count, which is the number that tells you whether a fix is
     * coming. Position itself goes on the right, big enough to read at a
     * glance while driving. */
    lv_obj_t *stat = mk_card(t, COL_L_X, STAT_Y, COL_L_W, STAT_H, COL_GPS);
    lv_obj_t *cap = mk_label(stat, &lv_font_montserrat_14, COL_MUTED, "SATELLITES");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    s_gps_sats = mk_label(stat, &lv_font_montserrat_36, COL_TEXT, "0");
    lv_obj_align(s_gps_sats, LV_ALIGN_BOTTOM_LEFT, 0, 2);

    lv_obj_t *metac = mk_card(t, COL_L_X, META_Y, COL_L_W, META_H_PLAIN, COL_EDGE);
    s_gps_meta = mk_label(metac, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(s_gps_meta, COL_L_W - 14);
    lv_label_set_long_mode(s_gps_meta, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_gps_meta, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *pos = mk_card(t, COL_R_X, 4, COL_R_W, BODY_H - 8, COL_GPS);

    lv_obj_t *latcap = mk_label(pos, &lv_font_montserrat_14, COL_MUTED, "LATITUDE");
    lv_obj_align(latcap, LV_ALIGN_TOP_LEFT, 0, 4);
    s_gps_lat = mk_label(pos, &lv_font_montserrat_28, COL_TEXT, "--");
    lv_obj_align(s_gps_lat, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t *loncap = mk_label(pos, &lv_font_montserrat_14, COL_MUTED, "LONGITUDE");
    lv_obj_align(loncap, LV_ALIGN_TOP_LEFT, 0, 74);
    s_gps_lon = mk_label(pos, &lv_font_montserrat_28, COL_TEXT, "--");
    lv_obj_align(s_gps_lon, LV_ALIGN_TOP_LEFT, 0, 94);

    s_gps_state = mk_label(pos, &lv_font_montserrat_16, COL_MUTED, "");
    lv_obj_set_width(s_gps_state, COL_R_W - 14);
    lv_label_set_long_mode(s_gps_state, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_gps_state, LV_ALIGN_TOP_LEFT, 0, 150);
}

static void update_gps(void)
{
    gps_fix_t f;
    gps_get(&f);

    scanner_status_t sc;
    gps_status(&sc);

    lv_label_set_text_fmt(s_gps_sats, "%u", (unsigned)f.sats);

    /* Only print coordinates we actually have. A stale or absent fix shows
     * dashes rather than the last known position dressed up as current. */
    if (gps_has_fix()) {
        lv_label_set_text_fmt(s_gps_lat, "%.5f", f.lat);
        lv_label_set_text_fmt(s_gps_lon, "%.5f", f.lon);
        lv_obj_set_style_text_color(s_gps_lat, COL_TEXT, 0);
        lv_obj_set_style_text_color(s_gps_lon, COL_TEXT, 0);
    } else {
        lv_label_set_text(s_gps_lat, "--");
        lv_label_set_text(s_gps_lon, "--");
        lv_obj_set_style_text_color(s_gps_lat, COL_MUTED, 0);
        lv_obj_set_style_text_color(s_gps_lon, COL_MUTED, 0);
    }

    static const char *QUALITY[] = { "no fix", "GPS", "DGPS" };
    const char *q = (f.quality < 3) ? QUALITY[f.quality] : "fix";

    char meta[160];
    snprintf(meta, sizeof(meta), "%s\n%.0f m alt\n%.1f kt\n%lu sentences",
             q, (double)f.alt_m, (double)f.speed_kts,
             (unsigned long)sc.reports);
    lv_label_set_text(s_gps_meta, meta);

    if (sc.state == SCAN_UNAVAILABLE) {
        lv_label_set_text(s_gps_state,
                          "No NMEA on the GPS pins.\n"
                          "Check 3V3, GND, and that the module TX reaches RX.");
        lv_obj_set_style_text_color(s_gps_state, COL_BAD, 0);
    } else if (!gps_has_fix()) {
        lv_label_set_text(s_gps_state,
                          "Receiver talking, waiting for satellites.\n"
                          "A cold start takes minutes, and needs sky.");
        lv_obj_set_style_text_color(s_gps_state, COL_WARN, 0);
    } else {
        lv_label_set_text(s_gps_state, "Fix good -- positions are being logged.");
        lv_obj_set_style_text_color(s_gps_state, COL_OK, 0);
    }
}

/* ------------------------------------------------------------------ *
 *  Refresh                                                            *
 * ------------------------------------------------------------------ */

static void update_chrome(void)
{
    lv_label_set_text(s_hdr_title, SCREEN_NAME[s_active]);
    lv_obj_set_style_text_color(s_hdr_title, screen_color(s_active), 0);

    int64_t up_s = esp_timer_get_time() / 1000000;
    if (up_s < 3600) {
        lv_label_set_text_fmt(s_hdr_clock, "%lldm%02llds",
                              (long long)(up_s / 60), (long long)(up_s % 60));
    } else {
        lv_label_set_text_fmt(s_hdr_clock, "%lldh%02lldm",
                              (long long)(up_s / 3600),
                              (long long)((up_s % 3600) / 60));
    }

    net_link_status_t ln;
    net_link_status(&ln);
    c6_ota_status_t ota;
    c6_ota_status(&ota);

    if (ota.state == C6_OTA_WRITING || ota.state == C6_OTA_ERASING ||
        ota.state == C6_OTA_FINALISING) {
        lv_label_set_text_fmt(s_hdr_link, "OTA %u%%", ota.percent);
        lv_obj_set_style_text_color(s_hdr_link, COL_WARN, 0);
    } else if (ln.hosted_up) {
        lv_label_set_text_fmt(s_hdr_link, "C6 %s%s",
                              ln.wifi_up ? "W" : "-", ln.ble_up ? "B" : "-");
        lv_obj_set_style_text_color(s_hdr_link,
                                    (ln.wifi_up && ln.ble_up) ? COL_OK : COL_WARN, 0);
    } else {
        lv_label_set_text(s_hdr_link, "C6 DOWN");
        lv_obj_set_style_text_color(s_hdr_link, COL_BAD, 0);
    }

    sdlog_status_t sd;
    sdlog_status(&sd);
    if (!sd.mounted) {
        lv_label_set_text(s_hdr_sd, "NO SD");
        lv_obj_set_style_text_color(s_hdr_sd, COL_BAD, 0);
    } else {
        lv_label_set_text_fmt(s_hdr_sd, "SD %lluM", (unsigned long long)sd.free_mb);
        lv_obj_set_style_text_color(s_hdr_sd,
                                    sd.free_mb < 64 ? COL_WARN : COL_MUTED, 0);
    }

    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        lv_obj_set_style_bg_color(s_dots[i],
                                  i == s_active ? screen_color(s_active) : COL_EDGE, 0);
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    /* s_active is maintained by tile_changed_cb as the user swipes. */
    update_chrome();

    switch (s_active) {
    case UI_SCREEN_COMBINED: update_combined(); break;
    case UI_SCREEN_WIFI:     update_wifi();     break;
    case UI_SCREEN_BLE:      update_protocol(UI_SCREEN_BLE, DET_BLE, true); break;
    /* Matter rows come partly from mDNS, which has no RSSI, so order by
     * recency instead of signal. */
    case UI_SCREEN_MATTER:   update_protocol(UI_SCREEN_MATTER, DET_MATTER, false); break;
    case UI_SCREEN_ZIGBEE:   update_zigbee();   break;
    case UI_SCREEN_GPS:      update_gps();      break;
    default: break;
    }

    if (s_autocycle_s &&
        now_ms() - s_last_touch_ms > (int64_t)s_autocycle_s * 1000) {
        ui_show((s_active + 1) % UI_SCREEN_COUNT);
        s_last_touch_ms = now_ms();
    }
}

static void tile_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *cur = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        if (s_tile[i] == cur) {
            s_active = (ui_screen_t)i;
            break;
        }
    }
    s_last_touch_ms = now_ms();
    update_chrome();
}

/* ------------------------------------------------------------------ */

esp_err_t ui_init(void)
{
    if (!display_lock(0)) {
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_chrome(scr);

    s_tiles = lv_tileview_create(scr);
    lv_obj_set_pos(s_tiles, 0, HEADER_H);
    lv_obj_set_size(s_tiles, BSP_LCD_H_RES, BODY_H);
    lv_obj_set_style_bg_color(s_tiles, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_tiles, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tiles, 0, 0);
    lv_obj_add_event_cb(s_tiles, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        lv_dir_t dir = LV_DIR_HOR;
        s_tile[i] = lv_tileview_add_tile(s_tiles, i, 0, dir);
        lv_obj_set_style_pad_all(s_tile[i], 0, 0);
        /* Do NOT set the tile non-scrollable: the tileview navigates by
         * scrolling its tiles, so clearing that flag fights the widget.
         * Hiding the scrollbar is all that was actually wanted. */
        lv_obj_set_scrollbar_mode(s_tile[i], LV_SCROLLBAR_MODE_OFF);
    }

    build_combined(s_tile[UI_SCREEN_COMBINED]);
    build_protocol(s_tile[UI_SCREEN_WIFI],   UI_SCREEN_WIFI,   true);
    build_protocol(s_tile[UI_SCREEN_BLE],    UI_SCREEN_BLE,    false);
    build_protocol(s_tile[UI_SCREEN_MATTER], UI_SCREEN_MATTER, false);
    build_zigbee(s_tile[UI_SCREEN_ZIGBEE]);
    build_gps(s_tile[UI_SCREEN_GPS]);

    s_active = UI_SCREEN_COMBINED;
    s_last_touch_ms = now_ms();
    update_chrome();

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    display_unlock();
    ESP_LOGI(TAG, "dashboard built: %d screens", UI_SCREEN_COUNT);
    return ESP_OK;
}

void ui_show(ui_screen_t screen)
{
    if (screen >= UI_SCREEN_COUNT || !s_tiles) {
        return;
    }
    lv_tileview_set_tile_by_index(s_tiles, screen, 0, LV_ANIM_ON);
    s_active = screen;
}

void ui_set_autocycle(uint16_t seconds)
{
    s_autocycle_s = seconds;
    s_last_touch_ms = now_ms();
}
