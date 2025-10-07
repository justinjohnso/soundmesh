idf_component_register(SRCS "mesh_stream.c"
                      INCLUDE_DIRS "include"
                      REQUIRES esp_wifi esp_netif esp_event audio_pipeline esp_timer)
