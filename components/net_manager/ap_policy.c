#include "ap_policy.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "ap_policy";

#define AP_BOOT_GRACE_MS (5 * 60 * 1000)

static ap_state_t s_state = AP_STATE_BOOT_GRACE;
static bool s_grace_active = true;
static uint8_t s_client_count;
static esp_timer_handle_t s_grace_timer;
static ap_policy_state_change_cb_t s_cb;

static void set_state(ap_state_t new_state)
{
    if (new_state == s_state) {
        return;
    }
    ap_state_t old = s_state;
    s_state = new_state;
    ESP_LOGI(TAG, "state %d -> %d (clients=%u)", (int)old, (int)new_state, (unsigned)s_client_count);
    if (s_cb) {
        s_cb(old, new_state);
    }
}

static void grace_timer_cb(void *arg)
{
    (void)arg;
    s_grace_active = false;
    set_state(s_client_count > 0 ? AP_STATE_ACTIVE_BY_CLIENT : AP_STATE_OFF);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_client_count++;
        if (!s_grace_active) {
            set_state(AP_STATE_ACTIVE_BY_CLIENT);
        }
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_client_count > 0) {
            s_client_count--;
        }
        if (!s_grace_active && s_client_count == 0) {
            set_state(AP_STATE_OFF);
        }
    }
}

esp_err_t ap_policy_init(void)
{
    s_state = AP_STATE_BOOT_GRACE;
    s_grace_active = true;
    s_client_count = 0;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler, NULL));

    const esp_timer_create_args_t targs = {
        .callback = grace_timer_cb,
        .name = "ap_boot_grace",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_grace_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_grace_timer, (uint64_t)AP_BOOT_GRACE_MS * 1000));

    ESP_LOGI(TAG, "boot grace window started (%d ms)", AP_BOOT_GRACE_MS);
    return ESP_OK;
}

void ap_policy_set_callback(ap_policy_state_change_cb_t cb)
{
    s_cb = cb;
}

void ap_policy_force_on(void)
{
    s_grace_active = true;
    esp_timer_stop(s_grace_timer); /* no-op if not running */
    esp_timer_start_once(s_grace_timer, (uint64_t)AP_BOOT_GRACE_MS * 1000);
    set_state(AP_STATE_BOOT_GRACE);
}

ap_state_t ap_policy_get_state(void)
{
    return s_state;
}

uint8_t ap_policy_client_count(void)
{
    return s_client_count;
}
