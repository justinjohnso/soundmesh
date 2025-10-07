# ESP-ADF compatibility patches for ESP-IDF v5.5+
# Suppress specific warnings that prevent ESP-ADF from building with newer GCC/IDF versions

# This file is included by component CMakeLists to add compatibility flags
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Wno-error=int-conversion
    -Wno-error=return-type
    -Wno-int-conversion
    -Wno-return-type
    -Wno-calloc-transposed-args
)

# Define compatibility macros for deprecated FreeRTOS types
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    xSemaphoreHandle=SemaphoreHandle_t
    portTICK_RATE_MS=portTICK_PERIOD_MS
)