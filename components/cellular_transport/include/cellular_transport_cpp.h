#pragma once

/* C++-only header - do NOT include from .c files (WalterModem is a C++
 * class, this header has no extern "C" wrapping). Shares the single
 * WalterModem instance between cellular_transport.cpp and
 * mqtt_client's backend_walter_mqtt.cpp, since there is exactly one
 * physical modem/AT-command UART.
 *
 * Only meaningful on esp32s3 (the Walter module's chip - see
 * main/idf_component.yml, which only resolves dptechnics/walter-modem
 * for that target). On any other target this header declares nothing;
 * callers must guard their own use with the same
 * `#if CONFIG_IDF_TARGET_ESP32S3`. */

#if CONFIG_IDF_TARGET_ESP32S3

#include "WalterModem.h" /* VERIFY: header name/path from the fetched managed component */

WalterModem &cellular_transport_get_modem();

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
