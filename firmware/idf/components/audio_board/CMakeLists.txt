idf_component_register(
    SRCS "audio_board_stub.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_driver_i2c
)