#include "net_manager.h"

#include <stdio.h>
#include <string.h>

#include "ap_policy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "net_manager";

static void apply_wifi_mode_for_ap_state(ap_state_t old_state, ap_state_t new_state)
{
    (void)old_state;

    /* STA-mode handling (WiFi as data transport) is layered on top of
     * this in a later build stage; for now the radio only ever needs to
     * be AP or fully off. */
    wifi_mode_t desired = (new_state == AP_STATE_OFF) ? WIFI_MODE_NULL : WIFI_MODE_AP;

    wifi_mode_t current;
    if (esp_wifi_get_mode(&current) == ESP_OK && current == desired) {
        return;
    }

    esp_err_t err = esp_wifi_set_mode(desired);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(%d) failed: %s", (int)desired, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "wifi mode -> %d (ap_state=%d)", (int)desired, (int)new_state);
}

esp_err_t net_manager_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    wifi_config_t ap_cfg = { 0 };
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "WalterSensor-%02X%02X", mac[4], mac[5]);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.ssid_hidden = 0;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ap_policy_set_callback(apply_wifi_mode_for_ap_state);
    ESP_ERROR_CHECK(ap_policy_init());

    ESP_LOGI(TAG, "SoftAP '%s' started (open network, portal login required)", ap_cfg.ap.ssid);
    return ESP_OK;
}

void net_manager_force_ap_on(void)
{
    ap_policy_force_on();
}

bool net_manager_ap_is_active(void)
{
    return ap_policy_get_state() != AP_STATE_OFF;
}

uint8_t net_manager_ap_client_count(void)
{
    return ap_policy_client_count();
}
