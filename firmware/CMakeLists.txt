# Arbor ESP32 Firmware
# ESP-IDF 5.x project — CMake build system
#
# Target: ESP32-WROOM-32 (Waveshare Servo Driver)
# Task: F01

cmake_minimum_required(VERSION 3.16)

# Read version from version.h (single source of truth)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/main/version.h" _ver_h)
string(REGEX MATCH "SB_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_ver_h}")
set(PROJECT_VER "${CMAKE_MATCH_1}")
message(STATUS "Arbor ESP firmware version: ${PROJECT_VER}")

# Pull in ESP-IDF build system before project()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(arbor-esp VERSION ${PROJECT_VER})
