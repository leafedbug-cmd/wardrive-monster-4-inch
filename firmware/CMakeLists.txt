cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wardrive_monster)

# ---------------------------------------------------------------------------
# Carry the ESP32-C6 slave image inside the P4's own flash.
#
# The C6's UART is on unrouted pads on this board, so the only practical way
# to reflash it is for the P4 to push the image over the SDIO link they share.
# Embedding it here means `idf.py flash` delivers BOTH chips' firmware over
# one USB cable, with no SD card and nothing to wire up.
#
# Build it first:   cd ../c6-firmware && idf.py set-target esp32c6 && idf.py build
# ---------------------------------------------------------------------------
set(C6_IMAGE "${CMAKE_CURRENT_LIST_DIR}/../c6-firmware/build/wardrive_c6_slave.bin")

if(EXISTS "${C6_IMAGE}")
    esptool_py_flash_to_partition(flash "c6fw" "${C6_IMAGE}")
    message(STATUS "C6 slave image found -- will be flashed into the c6fw partition")
else()
    message(STATUS "No C6 slave image at ${C6_IMAGE}")
    message(STATUS "  c6fw partition will be left empty; the C6 keeps its current firmware.")
    message(STATUS "  To build it: cd ../c6-firmware && idf.py set-target esp32c6 && idf.py build")
endif()
