/*
 * ESP32-C6 co-processor firmware for the Wardrive Monster.
 *
 * This is the "slave" half of ESP-Hosted. It owns the 2.4 GHz radio and
 * answers RPC from the ESP32-P4 over SDIO. There is deliberately almost
 * nothing here: the esp_hosted co-processor component registers its own
 * feature tasks (transport, RPC, Wi-Fi, BT HCI, system/OTA) and starts
 * them automatically, so app_main only has to prepare NVS and the event
 * loop and get out of the way.
 *
 * Built with dual OTA slots so the P4 can replace this image over SDIO --
 * which matters a lot on this board, because the C6's UART is on bare
 * pads. See docs/C6-OTA.md.
 */
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "c6_slave";

void app_main(void)
{
    ESP_LOGI(TAG, "Wardrive Monster C6 co-processor starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "handing off to esp_hosted co-processor");
}
