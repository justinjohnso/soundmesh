idf_component_register(SRCS "ctrl_plane.c"
                      INCLUDE_DIRS "include"
                      REQUIRES esp_wifi esp_netif esp_event esp_mesh json)
