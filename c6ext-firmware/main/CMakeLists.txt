idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "." "../../shared"
                       REQUIRES esp_driver_uart esp_driver_gpio
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif
                                     esp_event ieee802154)
