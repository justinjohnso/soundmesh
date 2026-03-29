idf_component_register(
    SRCS 
        "src/usb_audio.c" 
        "src/adc_audio.c"
        "src/es8388_audio.c"
        "src/i2s_audio.c"
        "src/tone_gen.c"
        "src/ring_buffer.c"
        "src/rx_underrun_concealment.c"
        "src/sequence_tracker.c"
        "src/adf_pipeline.c"
        "src/adf_pipeline_core.c"
        "src/adf_pipeline_tx.c"
        "src/adf_pipeline_rx.c"
        "src/adf_pipeline_fft.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc esp-opus
)
