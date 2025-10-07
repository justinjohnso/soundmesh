idf_component_register(SRCS "main.c"
                      INCLUDE_DIRS "."
                      REQUIRES audio_pipeline audio_stream mesh_stream ctrl_plane esp_wifi esp_netif esp_event driver)
