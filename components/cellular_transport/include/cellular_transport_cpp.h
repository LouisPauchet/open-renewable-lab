#pragma once

/* C++-only header - do NOT include from .c files (WalterModem is a C++
 * class, this header has no extern "C" wrapping). Shares the single
 * WalterModem instance between cellular_transport.cpp and
 * mqtt_client's backend_walter_mqtt.cpp, since there is exactly one
 * physical modem/AT-command UART. */

#include "WalterModem.h" /* VERIFY: header name/path from the fetched managed component */

WalterModem &cellular_transport_get_modem();
