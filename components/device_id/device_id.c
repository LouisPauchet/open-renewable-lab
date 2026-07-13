#include "device_id.h"

#include <stdio.h>

#include "esp_mac.h"

static char s_device_id[13]; /* 12 hex chars + NUL */
static bool s_computed;

const char *device_id_get(void)
{
    if (!s_computed) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        s_computed = true;
    }
    return s_device_id;
}
