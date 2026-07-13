#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* System time before this (~2023-11-14) is treated as "not yet synced" -
 * the ESP32 boots at epoch 0 plus uptime until something sets the
 * clock. Shared threshold used by sampling_engine, web_portal's
 * /api/status, and this component so "synced" means the same thing
 * everywhere. */
#define TIME_SYNC_EPOCH_THRESHOLD 1700000000

/* Starts an opportunistic SNTP client (pool.ntp.org) - it syncs
 * whenever IP connectivity exists (WiFi STA) and is a harmless no-op
 * otherwise. Cellular time sync (modem NITZ) is wired up separately in
 * cellular_transport once that lands, since the modem's AT-command
 * socket layer isn't reachable through lwIP/SNTP. */
esp_err_t time_sync_init(void);

bool time_sync_is_synced(void);

#ifdef __cplusplus
}
#endif
