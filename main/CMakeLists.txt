idf_component_register(SRCS "main.c" "wifi_sta.c" "ftp_server.c" "http_server.c" "oled.c" "wifi_cfg.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_i2s esp_driver_sdspi esp_driver_spi esp_driver_gpio
                                  esp_driver_i2c esp_http_server
                                  sdmmc fatfs esp_adc esp_wifi esp_netif esp_event nvs_flash)
