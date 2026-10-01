cmake_minimum_required(VERSION 3.16)

# NOTE: esp_hosted_bt_host_stack (which binds NimBLE to the HCI pipe tunnelled
# over SDIO) needs no wiring here. Despite living under esp_hosted's
# examples/common_components, it is not a standalone IDF component: esp_hosted's
# own CMakeLists add_subdirectory's it and attaches it to the eh_host umbrella,
# gated on CONFIG_ESP_HOSTED_HOST_FEAT_BT. Setting that Kconfig is all it takes.

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
# The image is wrapped by tools/wrap_c6.py first: esptool checks every file it
# writes and rejects a C6 image aimed at a P4, even though this one is only
# ever stored as data. See that script for the container layout.
#
# Build the C6 image first:
#   cd ../c6-firmware && idf.py set-target esp32c6 && idf.py build
# Then re-run cmake here (the wrap happens at configure time):
#   idf.py fullclean && idf.py build
# ---------------------------------------------------------------------------
set(C6_IMAGE   "${CMAKE_CURRENT_LIST_DIR}/../c6-firmware/build/wardrive_c6_slave.bin")
set(C6_WRAPPED "${CMAKE_BINARY_DIR}/c6fw.img")

if(EXISTS "${C6_IMAGE}")
    execute_process(
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/tools/wrap_c6.py"
                "${C6_IMAGE}" "${C6_WRAPPED}"
        RESULT_VARIABLE C6_WRAP_RESULT
        OUTPUT_VARIABLE C6_WRAP_OUTPUT
        ERROR_VARIABLE  C6_WRAP_OUTPUT)

    if(C6_WRAP_RESULT EQUAL 0)
        esptool_py_flash_to_partition(flash "c6fw" "${C6_WRAPPED}")
        message(STATUS "C6 slave image will be flashed into the c6fw partition")
        message(STATUS "  ${C6_WRAP_OUTPUT}")
    else()
        message(WARNING "Could not wrap the C6 image: ${C6_WRAP_OUTPUT}")
    endif()
else()
    message(STATUS "No C6 slave image at ${C6_IMAGE}")
    message(STATUS "  c6fw partition will be left empty; the C6 keeps its current firmware.")
    message(STATUS "  To build it: cd ../c6-firmware && idf.py set-target esp32c6 && idf.py build")
endif()
