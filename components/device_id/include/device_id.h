#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a stable, unique-per-chip device identifier derived from the
 * ESP32's factory-programmed MAC address (12 uppercase hex chars, e.g.
 * "AABBCCDDEEFF") - no configuration required, always available. Used
 * to tag CSV file headers, MQTT topics (so multiple nodes can share
 * one broker/topic_prefix), and shown read-only in the web portal.
 * Computed once and cached; safe to call from any task. */
const char *device_id_get(void);

#ifdef __cplusplus
}
#endif
