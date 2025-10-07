cmake_minimum_required(VERSION 3.16)

set(ADF_PATH "${CMAKE_CURRENT_LIST_DIR}/../../esp-adf")
list(APPEND EXTRA_COMPONENT_DIRS
    ${ADF_PATH}/components
    ${CMAKE_CURRENT_LIST_DIR}/../../components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(PROJECT_KCONFIG ${CMAKE_CURRENT_LIST_DIR}/../../Kconfig APPEND)

set(PROJECT_NAME node)
project(${PROJECT_NAME})
