#include "display.h"

#include "bsp_pins.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t     s_panel;
static esp_lcd_panel_io_handle_t  s_io;
static esp_lcd_touch_handle_t     s_touch;
static lv_display_t              *s_disp;
static uint8_t                    s_brightness = BSP_LCD_BL_DEFAULT_PCT;

/* ------------------------------------------------------------------ *
 *  Backlight                                                          *
 * ------------------------------------------------------------------ */

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BSP_LCD_BL_LEDC_MODE,
        .timer_num       = BSP_LCD_BL_LEDC_TIMER,
        .duty_resolution = BSP_LCD_BL_DUTY_RES,
        .freq_hz         = BSP_LCD_BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    const ledc_channel_config_t chan = {
        .gpio_num   = BSP_LCD_PIN_BL,
        .speed_mode = BSP_LCD_BL_LEDC_MODE,
        .channel    = BSP_LCD_BL_LEDC_CHAN,
        .timer_sel  = BSP_LCD_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "ledc channel");
    return ESP_OK;
}

void display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;

    const uint32_t max_duty = (1u << BSP_LCD_BL_DUTY_RES) - 1;
    uint32_t duty = (max_duty * percent) / 100;
#if BSP_LCD_BL_ON_LEVEL == 0
    duty = max_duty - duty;
#endif
    ledc_set_duty(BSP_LCD_BL_LEDC_MODE, BSP_LCD_BL_LEDC_CHAN, duty);
    ledc_update_duty(BSP_LCD_BL_LEDC_MODE, BSP_LCD_BL_LEDC_CHAN);
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

/* ------------------------------------------------------------------ *
 *  Panel                                                              *
 * ------------------------------------------------------------------ */

static esp_err_t panel_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num     = BSP_LCD_PIN_SCLK,
        .mosi_io_num     = BSP_LCD_PIN_MOSI,
        .miso_io_num     = BSP_LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* One full-width band of RGB565 per transfer. */
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BSP_LCD_PIN_DC,
        .cs_gpio_num       = BSP_LCD_PIN_CS,
        .pclk_hz           = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = BSP_LCD_CMD_BITS,
        .lcd_param_bits    = BSP_LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                 &io_cfg, &s_io),
        TAG, "panel io");

    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io, &dev, &s_panel),
                        TAG, "st7796");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* Orientation is deliberately NOT set here.
     *
     * esp_lvgl_port re-applies swap_xy/mirror from its own rotation config
     * when the display is registered (esp_lvgl_port_disp.c), so anything we
     * set at this level is silently overwritten. Setting it in both places
     * is how you end up with a portrait-addressed panel being fed landscape
     * rows: mirrored text and a 160 px strip of never-written panel RAM.
     *
     * The single source of truth is lvgl_init() below. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Touch                                                              *
 * ------------------------------------------------------------------ */

static esp_err_t touch_init(void)
{
    /* The board already has an I2C bus here (ES8311 at 0x18, camera SCCB).
     * The FT6336U lives at 0x38, so it simply joins. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = BSP_I2C_PORT,
        .sda_io_num                   = BSP_I2C_PIN_SDA,
        .scl_io_num                   = BSP_I2C_PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Someone already owns the port -- reuse it. */
        ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(BSP_I2C_PORT, &bus),
                            TAG, "i2c bus handle");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "i2c bus");
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = BSP_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io),
                        TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max         = BSP_LCD_H_RES,
        .y_max         = BSP_LCD_V_RES,
        .rst_gpio_num  = BSP_TOUCH_PIN_RST,
        .int_gpio_num  = BSP_TOUCH_PIN_INT,
        .levels = {
            .reset     = 0,   /* CTP_RST is active low */
            .interrupt = 0,   /* CTP_INT pulls low on touch */
        },
        /* Match the panel's swap/mirror so touch lines up with pixels. */
        .flags = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
    };

    esp_err_t terr = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch);
    if (terr != ESP_OK) {
        /* A module ordered without touch (MSP4030) lands here. Not fatal --
         * the dashboard still cycles screens on a timer. */
        ESP_LOGW(TAG, "no FT6336U at 0x%02X (%s) -- touch disabled",
                 BSP_TOUCH_I2C_ADDR, esp_err_to_name(terr));
        s_touch = NULL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  LVGL                                                               *
 * ------------------------------------------------------------------ */

static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t cfg = {
        .task_priority = 4,
        .task_stack    = 8192,
        .task_affinity = -1,
        .timer_period_ms = 5,
        .task_max_sleep_ms = 500,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&cfg), TAG, "lvgl port");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_io,
        .panel_handle  = s_panel,
        /* Two 40-line bands, double buffered, out of PSRAM. */
        .buffer_size   = BSP_LCD_H_RES * 40,
        .double_buffer = true,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        /* Native panel is 320x480 portrait; the dashboard runs landscape.
         *
         * swap_xy transposes it to 480x320. A transpose is a reflection, so
         * it normally takes an ODD number of mirrors to turn back into a
         * true rotation -- but THIS module's native scan direction is itself
         * reversed (text reads mirrored with no transforms applied at all),
         * which adds one more reflection. Hence both mirrors here: the
         * panel's built-in one plus these two is an even number, and the
         * image comes out the right way round.
         *
         * If it reads upside down, set BOTH to false rather than one of
         * each -- one of each puts the mirroring back. */
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            .swap_bytes  = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return ESP_FAIL;
    }

    if (s_touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = s_disp,
            .handle = s_touch,
        };
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGW(TAG, "touch not registered with LVGL");
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(panel_init(),     TAG, "panel");
    ESP_RETURN_ON_ERROR(touch_init(),     TAG, "touch");
    ESP_RETURN_ON_ERROR(lvgl_init(),      TAG, "lvgl");

    display_set_brightness(BSP_LCD_BL_DEFAULT_PCT);
    ESP_LOGI(TAG, "display up: %dx%d, touch %s",
             BSP_LCD_H_RES, BSP_LCD_V_RES, s_touch ? "yes" : "no");
    return ESP_OK;
}

bool display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
