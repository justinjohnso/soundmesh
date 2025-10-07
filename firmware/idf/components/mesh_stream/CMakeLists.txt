idf_component_register(SRCS "mesh_stream.c"
                      INCLUDE_DIRS "include"
                      REQUIRES esp_wifi esp_netif esp_event esp_mesh audio_pipeline audio_element audio_board esp_timer)
