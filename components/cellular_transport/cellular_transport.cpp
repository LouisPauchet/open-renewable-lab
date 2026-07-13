/* See the big warning in include/cellular_transport.h - every
 * WalterModem call below is a best-effort guess from a research
 * summary, not verified against the actual library header (which
 * isn't fetched into this checkout). Treat this file as a skeleton to
 * correct once managed_components/dptechnics__walter-modem is
 * present, not as tested code. */

#include "cellular_transport.h"

#include <ctime>
#include <cstring>

#include "cellular_transport_cpp.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cellular_transport";

WalterModem &cellular_transport_get_modem()
{
    static WalterModem modem; /* VERIFY: default-constructible, per walter-esp-idf examples */
    return modem;
}

static volatile bool s_registered = false;
static volatile bool s_pdp_active = false;

static void cellular_task(void *pvParams)
{
    (void)pvParams;
    WalterModem &modem = cellular_transport_get_modem();
    for (;;) {
        /* VERIFY: exact registration-status query API/enum names. */
        WalterModemNetworkRegState reg_state = modem.getNetworkRegState();
        s_registered = (reg_state == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
                        reg_state == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t cellular_transport_init(void)
{
    ESP_LOGW(TAG, "cellular_transport is unverified against the real walter-modem SDK - see header comment");

    net_settings_t net;
    config_store_get_net_settings(&net);

    WalterModem &modem = cellular_transport_get_modem();

    /* VERIFY: begin() signature. UART0 is hardwired to the modem on the
     * Walter module, so this likely needs no arguments, but confirm. */
    if (!modem.begin()) {
        ESP_LOGE(TAG, "modem.begin() failed");
        return ESP_FAIL;
    }

    if (strlen(net.cellular_pin) > 0) {
        /* VERIFY: SIM PIN unlock method name/signature. */
        ESP_LOGW(TAG, "SIM PIN configured but unlockSIM()-equivalent call is not yet wired up - VERIFY and implement");
    }

    /* VERIFY: exact PDP context API name/parameter order/APN auth type,
     * and whether activation is automatic after begin() or needs an
     * explicit call. */
    if (!modem.definePDPContext(1, net.cellular_apn)) {
        ESP_LOGE(TAG, "definePDPContext failed");
        return ESP_FAIL;
    }
    s_pdp_active = true; /* optimistic placeholder - confirm the real success signal */

    BaseType_t ok = xTaskCreate(cellular_task, "cellular", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool cellular_transport_is_registered(void)
{
    return s_registered;
}

bool cellular_transport_is_pdp_active(void)
{
    return s_pdp_active;
}

static SemaphoreHandle_t s_gnss_mutex;
static gnss_fix_t s_last_fix;

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

    WalterModem &modem = cellular_transport_get_modem();

    /* VERIFY: exact GNSS trigger/result API. Walter's GNSS is part of
     * the Sequans modem chip; the walter-esp-idf examples reportedly
     * include a "positioning" example this should be checked against.
     * Guessed shape: a blocking "perform a GNSS fix" call returning a
     * result struct/status, since a cold GNSS fix can take tens of
     * seconds - hence the caller-supplied timeout_ms. */
    WalterModemGNSSFix walter_fix;
    if (!modem.gnssPerformFix(&walter_fix, timeout_ms)) {
        ESP_LOGW(TAG, "GNSS fix failed or timed out");
        return ESP_ERR_TIMEOUT;
    }

    gnss_fix_t fix = {
        .latitude = walter_fix.latitude,   /* VERIFY: field name */
        .longitude = walter_fix.longitude, /* VERIFY: field name */
        .altitude_m = walter_fix.altitude, /* VERIFY: field name */
        .timestamp_unix = (int64_t)time(NULL),
        .valid = true,
    };

    xSemaphoreTake(s_gnss_mutex, portMAX_DELAY);
    s_last_fix = fix;
    xSemaphoreGive(s_gnss_mutex);

    *out_fix = fix;
    return ESP_OK;
}

void cellular_transport_get_last_fix(gnss_fix_t *out_fix)
{
    if (!out_fix) {
        return;
    }
    if (!s_gnss_mutex) {
        *out_fix = (gnss_fix_t){ 0 };
        return;
    }
    xSemaphoreTake(s_gnss_mutex, portMAX_DELAY);
    *out_fix = s_last_fix;
    xSemaphoreGive(s_gnss_mutex);
}
