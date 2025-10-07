cmake_minimum_required(VERSION 3.16)

set(ADF_PATH "${CMAKE_CURRENT_LIST_DIR}/../../esp-adf")
list(APPEND EXTRA_COMPONENT_DIRS
    ${ADF_PATH}/components
    ${CMAKE_CURRENT_LIST_DIR}/../../components
)

# ESP-ADF compatibility fixes for ESP-IDF v5.5+
cmake_minimum_required(VERSION 3.16)

set(ADF_PATH "${CMAKE_CURRENT_LIST_DIR}/../../esp-adf")
list(APPEND EXTRA_COMPONENT_DIRS
    ${ADF_PATH}/components
    ${CMAKE_CURRENT_LIST_DIR}/../../components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(PROJECT_KCONFIG ${CMAKE_CURRENT_LIST_DIR}/../../Kconfig APPEND)

set(PROJECT_NAME rx)
project(${PROJECT_NAME})

# ESP-ADF compatibility fixes for ESP-IDF v5.5+
add_compile_definitions(
    portTICK_RATE_MS=portTICK_PERIOD_MS
    xSemaphoreHandle=SemaphoreHandle_t
    xTimerHandle=TimerHandle_t
    xTaskHandle=TaskHandle_t
)
add_compile_options(
    -Wno-error=int-conversion
    -Wno-error=return-type
    -Wno-int-conversion
    -Wno-return-type
    -Wno-calloc-transposed-args
)
add_compile_options(
    -Wno-error=int-conversion
    -Wno-error=return-type
    -Wno-int-conversion
    -Wno-return-type
    -Wno-calloc-transposed-args
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(PROJECT_KCONFIG ${CMAKE_CURRENT_LIST_DIR}/../../Kconfig APPEND)

set(PROJECT_NAME rx)
project(${PROJECT_NAME})
