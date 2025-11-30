idf_component_register(
    SRCS 
        "src/usb_audio.c" 
        "src/adc_audio.c"
        "src/es8388_audio.c"
        "src/i2s_audio.c"
        "src/tone_gen.c"
        "src/ring_buffer.c"
        "src/opus_codec.c"
        "src/adf_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc esp-opus
)
