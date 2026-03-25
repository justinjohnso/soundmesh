cmake_minimum_required(VERSION 3.16.0)

if(DEFINED ENV{PIOENV})
    set(_env $ENV{PIOENV})
else()
    set(_env "src")
endif()

set(SDKCONFIG_DEFAULTS
    "${CMAKE_SOURCE_DIR}/sdkconfig.shared.defaults"
    "${CMAKE_SOURCE_DIR}/sdkconfig.${_env}.defaults"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshnet-audio)
