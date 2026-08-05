#include "time_sync.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "time_sync";

static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "system time synchronized via SNTP");
}

esp_err_t time_sync_init(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.wait_for_sync = false; /* never block boot on network time */
    config.sync_cb = on_time_synced;

    return esp_netif_sntp_init(&config);
}

bool time_sync_is_synced(void)
{
    return time(NULL) >= TIME_SYNC_EPOCH_THRESHOLD;
}

void time_sync_set_from_epoch(int64_t epoch_unix, const char *source)
{
    if (epoch_unix < TIME_SYNC_EPOCH_THRESHOLD) {
        return;
    }
    struct timeval tv = { .tv_sec = (time_t)epoch_unix, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system time synchronized via %s", source);
}
