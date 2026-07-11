idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "." "../.."
                       REQUIRES nvs_flash esp_wifi esp_event esp_netif driver esp_driver_i2s esp_driver_i2c)
