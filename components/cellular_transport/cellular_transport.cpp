/* See the big warning in include/cellular_transport.h - every
 * WalterModem call below is a best-effort guess from a research
 * summary, not verified against the actual library header (which
 * isn't fetched into this checkout). Treat this file as a skeleton to
 * correct once managed_components/dptechnics__walter-modem is
 * present, not as tested code. */

#include "cellular_transport.h"

#include <cstring>

#include "cellular_transport_cpp.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
