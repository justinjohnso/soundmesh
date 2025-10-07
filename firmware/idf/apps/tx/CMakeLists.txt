cmake_minimum_required(VERSION 3.16)

# Minimal ESP-IDF only build for MVP
# TODO: Add ESP-ADF back once we have working basic mesh audio

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PROJECT_NAME tx)
project(${PROJECT_NAME})
