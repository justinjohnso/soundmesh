cmake_minimum_required(VERSION 3.16)

# Top-level multi-app workspace: select app via IDF_PROJECT_EXECUTABLE or set default.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Export compile_commands.json for IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Default to building the dual-mode node app; override with `-DPROJECT=tx` or `-DPROJECT=rx`.
if(NOT DEFINED PROJECT)
    set(PROJECT node)
endif()

# Add ADF path if provided via environment; otherwise assume submodule at esp-adf
if(NOT DEFINED ADF_PATH)
    set(ADF_PATH "${CMAKE_SOURCE_DIR}/esp-adf")
endif()
set(ENV{ADF_PATH} ${ADF_PATH})

# Add ADF CMake helpers
list(APPEND CMAKE_MODULE_PATH ${ADF_PATH}/cmake)
include(adf_project)

# Use adf_project to set up project with common components and apps
set(EXTRA_COMPONENT_DIRS
    ${ADF_PATH}/components
    ${CMAKE_SOURCE_DIR}/components
)

# Add project Kconfig
idf_build_set_property(PROJECT_KCONFIG ${CMAKE_SOURCE_DIR}/Kconfig APPEND)

# Map app names to directories
if(${PROJECT} STREQUAL "tx")
    adf_project(tx)
elseif(${PROJECT} STREQUAL "rx")
    adf_project(rx)
else()
    adf_project(node)
endif()