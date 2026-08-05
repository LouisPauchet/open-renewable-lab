# dptechnics/walter-modem only supports the esp32s3 target (see
# main/idf_component.yml) - only pull it in when actually building for
# that target, so the ESP32 DevKit V1 test-rig target (plain esp32)
# doesn't hit a dependency-resolution error for a component it never
# uses. cellular_transport.cpp/cellular_transport_cpp.h mirror this
# with matching `#if CONFIG_IDF_TARGET_ESP32S3` guards.
#
# Confirmed via an actual esp32s3 build (component manager resolves the
# managed dependency to "dptechnics__walter-modem", the
# <namespace>__<name> convention - not the bare "walter-modem" name
# from idf_component.yml).
#
# Must use IDF_TARGET here, not CONFIG_IDF_TARGET_ESP32S3: REQUIRES is
# captured by an early "component requirements expansion" CMake pass
# that runs before sdkconfig/Kconfig is available, so any CONFIG_*
# variable reads back empty at that point even though it's set
# correctly by the time the component's own body executes (confirmed
# by a temporary debug print - IDF_TARGET is set in both passes,
# CONFIG_IDF_TARGET_ESP32S3 only in the second/later one). Getting this
# wrong doesn't error - it just silently drops the REQUIRES entry, so
# WalterModem.h fails to resolve with no obvious link back to this file.
set(walter_modem_req "")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND walter_modem_req dptechnics__walter-modem)
endif()

# walter-modem's own CMakeLists.txt REQUIRES the legacy "driver" umbrella
# component for driver/uart.h and driver/gpio.h - in this ESP-IDF version
# "driver" is a thin compat shim that no longer provides either (uart/gpio
# were split into their own esp_driver_* components), so we pull those in
# directly ourselves rather than editing the vendored package.
set(walter_modem_transitive_req "")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND walter_modem_transitive_req esp_driver_uart esp_driver_gpio)
endif()

idf_component_register(
    SRCS "cellular_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES config_store freertos time_sync ${walter_modem_req} ${walter_modem_transitive_req}
)
