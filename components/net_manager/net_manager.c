#include "net_manager.h"

#include <stdio.h>
#include <string.h>

#include "ap_policy.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "net_manager";

#define STA_RECONNECT_DELAY_US (5 * 1000 * 1000)

static bool s_sta_connected;
static esp_timer_handle_t s_sta_reconnect_timer;

/* Tracks whether esp_wifi_start() is currently in effect, independent of
 * esp_wifi_get_mode()'s own return (that reflects the last configured
 * mode even while stopped, which isn't what apply_wifi_mode() below
 * needs to know). Set true once at the end of net_manager_init(); see
 * apply_wifi_mode()'s WIFI_MODE_NULL branch for how it goes false. */
static bool s_wifi_started;

static wifi_mode_t compute_desired_mode(void)
{
    net_settings_t net;
    config_store_get_net_settings(&net);

    bool ap_on = ap_policy_get_state() != AP_STATE_OFF;
    bool sta_on = (net.transport == TRANSPORT_WIFI);

    if (ap_on && sta_on) {
        return WIFI_MODE_APSTA;
    }
    if (ap_on) {
        return WIFI_MODE_AP;
    }
    if (sta_on) {
        return WIFI_MODE_STA;
    }
    return WIFI_MODE_NULL;
}

static void apply_wifi_mode(void)
{
    wifi_mode_t desired = compute_desired_mode();

    if (desired == WIFI_MODE_NULL) {
        /* Nothing needs AP or STA right now (past the boot-grace window,
         * no portal client connected, and no WiFi station transport
         * configured) - fully stop the WiFi driver rather than just
         * clearing its mode, so the radio (RF/PHY) actually powers down
         * instead of staying initialized-but-idle. Restarted on demand
         * below the moment AP or STA is needed again (a client shows up
         * during a forced-on grace window, etc). */
        if (s_wifi_started) {
            esp_err_t err = esp_wifi_stop();
            if (err == ESP_OK) {
                s_wifi_started = false;
                ESP_LOGI(TAG, "wifi stopped - nothing needs AP or STA right now");
            } else {
                ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    if (s_wifi_started) {
        wifi_mode_t current;
        if (esp_wifi_get_mode(&current) == ESP_OK && current == desired) {
            return;
        }
    }

    esp_err_t err = esp_wifi_set_mode(desired);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(%d) failed: %s", (int)desired, esp_err_to_name(err));
        return;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return;
        }
        s_wifi_started = true;
        /* Modem power-save isn't persisted across stop/start - see the
         * matching call (and its own comment) in net_manager_init(). */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    ESP_LOGI(TAG, "wifi mode -> %d", (int)desired);
}

static void apply_wifi_mode_for_ap_state(ap_state_t old_state, ap_state_t new_state)
{
    (void)old_state;
    (void)new_state;
    apply_wifi_mode();
}

static void sta_reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        ESP_LOGW(TAG, "STA disconnected, retrying in %d s", STA_RECONNECT_DELAY_US / 1000000);
        esp_timer_stop(s_sta_reconnect_timer); /* no-op if not running */
        esp_timer_start_once(s_sta_reconnect_timer, STA_RECONNECT_DELAY_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA connected, got IP");
    }
}

esp_err_t net_manager_init(bool suppress_ap_boot_grace)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, sta_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sta_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event_handler, NULL));

    const esp_timer_create_args_t sta_targs = { .callback = sta_reconnect_timer_cb, .name = "sta_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&sta_targs, &s_sta_reconnect_timer));

    net_settings_t net;
    config_store_get_net_settings(&net);
    bool sta_wanted = (net.transport == TRANSPORT_WIFI);

    /* esp_wifi_set_config() requires its target interface to be enabled
     * in the current mode, so configure both AP and STA while in APSTA
     * mode regardless of what's actually wanted at boot - simpler than
     * conditionally ordering set_mode/set_config against each other
     * below. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

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

    if (sta_wanted) {
        wifi_config_t sta_cfg = { 0 };
        /* wifi_config_t's ssid[32] has no room for a trailing NUL if the
         * SSID is a full 32 characters, so copy by length rather than via
         * snprintf (which GCC correctly flags as truncating in that case). */
        size_t ssid_len = strnlen(net.wifi_sta_ssid, sizeof(sta_cfg.sta.ssid));
        memcpy(sta_cfg.sta.ssid, net.wifi_sta_ssid, ssid_len);
        snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", net.wifi_sta_password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    /* A normal boot always starts in the AP boot-grace window (AP forced
     * on). A power_manager sleep-wake boot (suppress_ap_boot_grace)
     * skips that - the portal doesn't need to be reachable on every
     * brief wake, only on a genuine boot or an explicit stay-awake
     * request (see power_manager_request_stay_awake()) - so it starts
     * with only whatever the data-plane transport itself needs. */
    bool ap_wanted_at_boot = !suppress_ap_boot_grace;
    wifi_mode_t initial_mode = ap_wanted_at_boot ? (sta_wanted ? WIFI_MODE_APSTA : WIFI_MODE_AP)
                                                  : (sta_wanted ? WIFI_MODE_STA : WIFI_MODE_NULL);
    if (initial_mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(initial_mode));
    }

    if (initial_mode != WIFI_MODE_NULL) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;

        /* Modem power-save: let the radio doze between beacon/DTIM
         * intervals instead of staying fully powered all the time -
         * cooperates with the system-wide automatic light sleep enabled
         * in app_main.c. Mainly benefits STA mode (idle time between the
         * broker/AP's beacons); the SoftAP side still has to keep
         * beaconing on schedule regardless; harmless to set
         * unconditionally. WIFI_PS_MIN_MODEM is also ESP-IDF's own
         * default for STA, set explicitly here so the intent isn't
         * silently dependent on that default staying unchanged. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    } else {
        /* Nothing wants AP or STA right now (sleep-wake boot, no WiFi
         * transport configured) - leave the radio stopped entirely
         * rather than starting it in NULL mode; apply_wifi_mode() brings
         * it up on demand the moment AP or STA is actually needed. */
        s_wifi_started = false;
    }

    ap_policy_set_callback(apply_wifi_mode_for_ap_state);
    ESP_ERROR_CHECK(ap_policy_init(suppress_ap_boot_grace));

    ESP_LOGI(TAG, "SoftAP '%s' %s (open network, portal login required)%s", ap_cfg.ap.ssid,
             ap_wanted_at_boot ? "started" : "not started (sleep-wake boot)",
             sta_wanted ? "; connecting to configured WiFi STA" : "");
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

bool net_manager_sta_is_connected(void)
{
    return s_sta_connected;
}
