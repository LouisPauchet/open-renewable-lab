/* Only meaningful on esp32s3 (the Walter module's chip); on any other
 * target (e.g. the ESP32 DevKit V1 test rig - see board_pins.h) every
 * function here is a no-op stub, since dptechnics/walter-modem isn't
 * even fetched for other targets (main/idf_component.yml).
 *
 * WalterModem API usage below was verified against the actual fetched
 * managed_components/dptechnics__walter-modem/src/WalterModem.h and
 * the SDK's own examples/positioning/main/positioning.cpp (the
 * reference this file's connect/GNSS sequencing mirrors). */

#include "cellular_transport.h"

#include <cinttypes>
#include <cstring>
#include <ctime>

#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cellular_transport";

static volatile bool s_registered = false;
static volatile bool s_pdp_active = false;
static SemaphoreHandle_t s_gnss_mutex;
static gnss_fix_t s_last_fix;

#if CONFIG_IDF_TARGET_ESP32S3

#include "cellular_transport_cpp.h"

/* Function-local static: thread-safe lazy init (C++11+), and guarantees
 * exactly one WalterModem instance shared between this file and
 * mqtt_client's backend_walter_mqtt.cpp - there is exactly one physical
 * modem/AT-command UART, see cellular_transport_cpp.h. */
WalterModem &cellular_transport_get_modem()
{
    static WalterModem modem;
    return modem;
}

/* Given by gnss_event_handler() when a WALTER_MODEM_GNSS_EVENT_FIX
 * event arrives - the SDK's GNSS API is asynchronous (gnssPerformAction()
 * only requests a fix; the result shows up later via this callback), so
 * cellular_transport_acquire_gnss_fix()'s blocking-with-timeout contract
 * is built on top of it rather than being a direct SDK call. */
static SemaphoreHandle_t s_gnss_fix_ready;
static WMGNSSFixEvent s_pending_walter_fix;

static void gnss_event_handler(WMGNSSEventType type, const WMGNSSEventData *data, void *args)
{
    (void)args;
    if (type == WALTER_MODEM_GNSS_EVENT_FIX && data) {
        s_pending_walter_fix = data->gnssfix;
        xSemaphoreGive(s_gnss_fix_ready);
    }
}

/* setOpState(FULL) + automatic network selection - the sequence the
 * SDK's own examples use to (re)join the LTE network. Doesn't block for
 * registration to complete; cellular_task() polls that separately. */
static bool start_lte_connect(WalterModem &modem)
{
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        ESP_LOGE(TAG, "setOpState(FULL) failed");
        return false;
    }
    if (!modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        ESP_LOGE(TAG, "setNetworkSelectionMode failed");
        return false;
    }
    return true;
}

static void cellular_task(void *pvParams)
{
    (void)pvParams;
    WalterModem &modem = cellular_transport_get_modem();
    for (;;) {
        WalterModemNetworkRegState reg_state = modem.getNetworkRegState();
        s_registered = (reg_state == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
                        reg_state == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t cellular_transport_init(void)
{
    net_settings_t net;
    config_store_get_net_settings(&net);

    WalterModem &modem = cellular_transport_get_modem();

    if (!modem.begin(UART_NUM_1)) {
        ESP_LOGE(TAG, "modem.begin() failed");
        return ESP_FAIL;
    }

    /* The SDK's own WalterModem.cpp has hidden ESP_LOGD("WalterModem",
     * "TX: ...") / ("RX: ...") calls tracing every raw AT command and
     * the modem's raw response - normally compiled out at this
     * project's default INFO log level. Enabling just this one tag
     * gives the exact AT command and modem error response (e.g. a
     * +CME ERROR code) behind a failure like definePDPContext(),
     * without this project's own INFO-level logging getting noisier. */
    esp_log_level_set("WalterModem", ESP_LOG_DEBUG);

    /* AT+CPIN? isn't called anywhere else in this codebase - added here
     * purely as a diagnostic (visible via the TX:/RX: trace above) for
     * a real "definePDPContext failed (+CME ERROR: 4)" that persisted
     * across a full flash erase and a hardware modem reset (which
     * begin() already performs every boot), pointing at the SIM/network
     * side rather than device-side state. */
    modem.getSIMState();

    /* PDP context/SIM PIN must be set up before the modem goes FULL -
     * NO_RF is the state the SDK's examples use for this setup phase. */
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGE(TAG, "setOpState(NO_RF) failed");
        return ESP_FAIL;
    }

    if (strlen(net.cellular_pin) > 0) {
        /* Non-fatal: many IoT/M2M SIMs ship with PIN lock disabled
         * entirely, and unlockSIM() on one of those errors out with
         * nothing actually wrong - continuing to PDP/registration
         * still works fine in that case. Aborting the whole cellular
         * stack here would only make sense for a SIM that's genuinely
         * PIN-locked with the wrong PIN configured, which looks
         * identical from this return value alone. Do NOT retry this
         * automatically - repeated wrong-PIN attempts risk exhausting
         * the SIM's (usually 3-try) retry counter toward a PUK lock. */
        if (!modem.unlockSIM(NULL, NULL, NULL, net.cellular_pin)) {
            ESP_LOGW(TAG, "unlockSIM failed (SIM may simply not be PIN-locked) - "
                          "continuing; if registration fails next, check the configured SIM PIN");
        }
    }

    /* An empty APN (the portal's "Auto-detect" clears the field) must
     * be passed as NULL, not "" - definePDPContext()'s documented
     * default (letting the network provide the APN) is keyed off the
     * pointer being NULL, and an explicitly empty string isn't
     * guaranteed to behave the same over AT+CGDCONT on every modem
     * firmware. */
    const char *apn = strlen(net.cellular_apn) > 0 ? net.cellular_apn : NULL;
    /* WALTER_MODEM_PDP_TYPE_IP (IPv4-only) got a hard "+CME ERROR: 4"
     * (operation not supported) from AT+CGDCONT on real hardware, with
     * an empty APN, no preceding SIM-unlock attempt, and even on a
     * freshly-erased device - so it's a real rejection of the PDP type
     * itself, not leftover state. Many IoT SIM subscriptions are
     * provisioned as IPv4v6-only and reject a plain IPv4-only context
     * request the same way. */
    if (!modem.definePDPContext(1, apn, NULL, NULL, NULL, WALTER_MODEM_PDP_TYPE_IPV4V6)) {
        ESP_LOGE(TAG, "definePDPContext failed");
        return ESP_FAIL;
    }
    s_pdp_active = true; /* context is defined; it activates once registered, not tracked separately here */

    if (!start_lte_connect(modem)) {
        return ESP_FAIL;
    }

    if (!modem.gnssConfig()) {
        ESP_LOGW(TAG, "gnssConfig failed - GNSS fixes will not be available");
    }
    s_gnss_fix_ready = xSemaphoreCreateBinary();
    modem.setGNSSEventHandler(gnss_event_handler, NULL);

    BaseType_t ok = xTaskCreate(cellular_task, "cellular", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t cellular_transport_acquire_gnss_fix(gnss_fix_t *out_fix, uint32_t timeout_ms)
{
    if (!out_fix) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_gnss_mutex) {
        s_gnss_mutex = xSemaphoreCreateMutex();
        if (!s_gnss_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_gnss_fix_ready) {
        return ESP_ERR_INVALID_STATE; /* cellular_transport_init() hasn't run yet */
    }

    WalterModem &modem = cellular_transport_get_modem();

    /* GNSS and LTE share the modem's RF front-end - the SDK's own
     * positioning example disconnects from the network before every
     * fix attempt ("Required for GNSS") and reconnects afterward.
     * Best-effort: a slow/failed disconnect or reconnect doesn't abort
     * the fix attempt itself, since cellular_task() will keep retrying
     * registration regardless. */
    bool was_registered = s_registered;
    if (was_registered) {
        modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF);
        WalterModemNetworkRegState reg_state = modem.getNetworkRegState();
        int waited_ms = 0;
        while (reg_state != WALTER_MODEM_NETWORK_REG_NOT_SEARCHING && waited_ms < 10000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited_ms += 100;
            reg_state = modem.getNetworkRegState();
        }
    }

    xSemaphoreTake(s_gnss_fix_ready, 0); /* drain any stale/unrelated pending signal */
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (!modem.gnssPerformAction()) {
        ESP_LOGW(TAG, "gnssPerformAction failed to start");
        result = ESP_FAIL;
    } else if (xSemaphoreTake(s_gnss_fix_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "GNSS fix timed out after %" PRIu32 " ms", timeout_ms);
    } else if (s_pending_walter_fix.status != WALTER_MODEM_GNSS_FIX_STATUS_READY) {
        ESP_LOGW(TAG, "GNSS fix not ready (status=%d)", (int)s_pending_walter_fix.status);
        result = ESP_FAIL;
    } else {
        gnss_fix_t fix = {};
        fix.latitude = s_pending_walter_fix.latitude;
        fix.longitude = s_pending_walter_fix.longitude;
        fix.altitude_m = (float)s_pending_walter_fix.height;
        fix.timestamp_unix = s_pending_walter_fix.timestamp;
        fix.valid = true;

        xSemaphoreTake(s_gnss_mutex, portMAX_DELAY);
        s_last_fix = fix;
        xSemaphoreGive(s_gnss_mutex);

        *out_fix = fix;
        result = ESP_OK;
    }

    if (was_registered) {
        start_lte_connect(modem);
    }

    return result;
}

#else /* !CONFIG_IDF_TARGET_ESP32S3 */

esp_err_t cellular_transport_init(void)
{
    ESP_LOGE(TAG, "cellular_transport is only supported on esp32s3 (the Walter module) - "
                  "this build target has no walter-modem dependency available");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cellular_transport_acquire_gnss_fix(gnss_fix_t *out_fix, uint32_t timeout_ms)
{
    (void)out_fix;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */

bool cellular_transport_is_registered(void)
{
    return s_registered;
}

bool cellular_transport_is_pdp_active(void)
{
    return s_pdp_active;
}

void cellular_transport_get_last_fix(gnss_fix_t *out_fix)
{
    if (!out_fix) {
        return;
    }
    if (!s_gnss_mutex) {
        *out_fix = gnss_fix_t{};
        return;
    }
    xSemaphoreTake(s_gnss_mutex, portMAX_DELAY);
    *out_fix = s_last_fix;
    xSemaphoreGive(s_gnss_mutex);
}
