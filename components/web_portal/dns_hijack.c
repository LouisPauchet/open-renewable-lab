#include "dns_hijack.h"

#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT 53
#define AP_IP_BYTES 192, 168, 4, 1

static void build_and_send_reply(int sock, const uint8_t *req, int req_len, const struct sockaddr_in *from,
                                  socklen_t from_len)
{
    if (req_len < 12) {
        return; /* shorter than a DNS header, ignore */
    }
    uint16_t qdcount = ((uint16_t)req[4] << 8) | req[5];
    if (qdcount < 1) {
        return;
    }

    static uint8_t tx[512];
    if (req_len > (int)sizeof(tx)) {
        return;
    }
    memcpy(tx, req, req_len);

    tx[2] = req[2] | 0x80; /* QR=1 (response), keep RD as sent */
    tx[3] = 0x80;          /* RA=1, RCODE=0 */
    tx[6] = 0x00;
    tx[7] = 0x01; /* ANCOUNT=1 */
    tx[8] = 0x00;
    tx[9] = 0x00; /* NSCOUNT=0 */
    tx[10] = 0x00;
    tx[11] = 0x00; /* ARCOUNT=0 */

    int pos = req_len;
    if (pos + 16 > (int)sizeof(tx)) {
        return;
    }
    tx[pos++] = 0xC0;
    tx[pos++] = 0x0C; /* name: pointer to the question at offset 12 */
    tx[pos++] = 0x00;
    tx[pos++] = 0x01; /* TYPE A */
    tx[pos++] = 0x00;
    tx[pos++] = 0x01; /* CLASS IN */
    tx[pos++] = 0x00;
    tx[pos++] = 0x00;
    tx[pos++] = 0x00;
    tx[pos++] = 0x3C; /* TTL 60s */
    tx[pos++] = 0x00;
    tx[pos++] = 0x04; /* RDLENGTH 4 */
    const uint8_t ip[4] = { AP_IP_BYTES };
    memcpy(&tx[pos], ip, 4);
    pos += 4;

    sendto(sock, tx, pos, 0, (const struct sockaddr *)from, from_len);
}

static void dns_hijack_task(void *pvParams)
{
    (void)pvParams;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "failed to create UDP socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "failed to bind UDP:%d (already in use?)", DNS_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "captive-portal DNS hijack listening on UDP:%d", DNS_PORT);

    uint8_t rx[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (len > 0) {
            build_and_send_reply(sock, rx, len, &from, from_len);
        }
    }
}

esp_err_t dns_hijack_start(void)
{
    BaseType_t ok = xTaskCreate(dns_hijack_task, "dns_hijack", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
