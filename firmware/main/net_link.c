#include "net_link.h"

#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "net_link";

static net_link_status_t s_st;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void probe_coprocessor(void)
{
    /* esp_hosted_get_cp_info() is a 3.x API. On 2.x there is no runtime chip
     * query, and this board only ever has one co-processor anyway. */
    strlcpy(s_st.cp_target, "esp32c6", sizeof(s_st.cp_target));

    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        snprintf(s_st.cp_fw, sizeof(s_st.cp_fw), "%lu.%lu.%lu",
                 (unsigned long)ver.major1,
                 (unsigned long)ver.minor1,
                 (unsigned long)ver.patch1);
        ESP_LOGI(TAG, "co-processor esp_hosted v%s", s_st.cp_fw);
    } else {
        strlcpy(s_st.cp_fw, "?", sizeof(s_st.cp_fw));
    }
}

esp_err_t net_link_start(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Bring up the SDIO link to the C6 before anything touches esp_wifi_*.
     * On this board the transport pins are the esp_hosted defaults for the
     * ESP32-P4 (CLK18 CMD19 D0..D3=14..17, reset GPIO54), so there is
     * nothing to configure here. */
    ESP_LOGI(TAG, "starting esp_hosted link to C6...");
    int rc = esp_hosted_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_hosted_init failed (%d) -- is the C6 flashed?", rc);
        return ESP_FAIL;
    }
    s_st.hosted_up = true;
    probe_coprocessor();

    /* STA mode purely as a scan vantage point; we never join anything.
     * A netif is still required for mDNS on the Matter screen. */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Keep the radio awake -- power save would gap the scan. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_st.wifi_up = true;
    ESP_LOGI(TAG, "radio link up");
    return ESP_OK;
}

void net_link_status(net_link_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}

void net_link_set_ble_up(bool up)
{
    s_st.ble_up = up;
}
