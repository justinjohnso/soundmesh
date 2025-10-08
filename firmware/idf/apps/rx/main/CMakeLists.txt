idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash
    PRIV_REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_ledc
)
