#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the captive-portal DNS hijack (UDP:53) and the HTTP server
 * (REST API under /api/*, embedded SPA for everything else). Requires
 * net_manager_init() to have already brought up the AP netif. Safe to
 * call regardless of current AP on/off state - the DNS/HTTP servers
 * simply won't receive traffic while the AP radio is off. */
esp_err_t web_portal_init(void);

#ifdef __cplusplus
}
#endif
