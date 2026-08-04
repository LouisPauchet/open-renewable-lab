cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(walter_sensor_node)

# dptechnics/walter-modem's own CMakeLists.txt REQUIRES the legacy "driver"
# umbrella component for driver/uart.h and driver/gpio.h - in this ESP-IDF
# version "driver" no longer provides either (both were split out into their
# own esp_driver_uart/esp_driver_gpio components), so the package's own
# source files fail to compile as fetched. This can't be fixed by editing
# the vendored copy under managed_components/ (gitignored - a fresh clone
# re-fetches the unpatched package), so patch its actual CMake target here
# instead, which runs on every configure for every clone regardless of
# whether managed_components/ was just fetched.
if(IDF_TARGET STREQUAL "esp32s3")
    idf_component_get_property(walter_modem_lib dptechnics__walter-modem COMPONENT_LIB)
    target_link_libraries(${walter_modem_lib} PRIVATE idf::esp_driver_uart idf::esp_driver_gpio)
endif()
