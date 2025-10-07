idf_component_register(SRCS "main.c"
                      INCLUDE_DIRS "."
                      REQUIRES audio_pipeline audio_element audio_stream i2s_stream usb_stream encoder_opus decoder_opus mesh_stream ctrl_plane esp_wifi esp_netif esp_event esp_mesh driver)
