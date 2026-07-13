#pragma once

/* Private to web_portal - not part of the component's public include/. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts a UDP:53 task that answers every DNS query with the AP's own
 * IP (192.168.4.1) - the standard captive-portal trick that makes
 * phones/laptops pop the login page automatically. Harmless to run for
 * the device's whole lifetime: with no AP clients associated, no
 * queries ever reach it. */
esp_err_t dns_hijack_start(void);

#ifdef __cplusplus
}
#endif
