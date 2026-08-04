#include "sdi12_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdi12_internal.h"

static const char *TAG = "sdi12_bus";

/* 1/1200s = 833.33us; SDI-12 tolerates a few % timing error. */
#define SDI12_BIT_PERIOD_US 833
/* Spec minimum break is 12ms, marking gap 8.33ms - both padded for margin. */
#define SDI12_BREAK_US 15000
#define SDI12_MARKING_US 9000
#define SDI12_INTERCHAR_GUARD_US 200

static bool s_initialized;
static SemaphoreHandle_t s_bus_mutex;
static SemaphoreHandle_t s_edge_sem;
static volatile uint32_t s_debug_edge_count; /* diagnostic only - see sdi12_bus_debug_tx_toggle_test() */

static void IRAM_ATTR sdi12_gpio_isr(void *arg)
{
    (void)arg;
    s_debug_edge_count++;
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_edge_sem, &higher_prio_woken);
    if (higher_prio_woken) {
        portYIELD_FROM_ISR();
    }
}

/* ---- bit-level TX/RX (SDI-12 polarity: mark/idle=LOW=1, space=HIGH=0) ----
 * TXD and RXD are separate, fixed-direction pins (see board_pins.h) -
 * no gpio_set_direction() toggling needed; only the TX_EN/RX_EN
 * transceiver-enable pins are switched. */

static inline void tx_bit(int bit_value)
{
    gpio_set_level(BOARD_PIN_SDI12_TXD, bit_value ? 0 : 1); /* SDI-12 polarity: value 1 -> LOW */
    esp_rom_delay_us(SDI12_BIT_PERIOD_US);
}

static void tx_char(char c)
{
    uint8_t data = (uint8_t)c & 0x7F;
    int ones = 0;
    for (int i = 0; i < 7; i++) {
        if (data & (1 << i)) {
            ones++;
        }
    }
    /* SDI-12's character framing is 7E1: 7 data bits, EVEN parity, 1
     * stop bit (confirmed against the spec and multiple independent
     * implementations, e.g. EnviroDIY/Arduino-SDI-12 and
     * ESPSoftwareSerial-based SDI-12 drivers using SWSERIAL_7E1). A
     * previous change to this line briefly (and incorrectly) claimed
     * SDI-12 needs odd parity - it does not; that was a mistake, not a
     * verified fact, and has been reverted. The real cause of the
     * total non-response on real hardware was the RX signal path
     * (RX_EN/RXD/U6), not parity - see sdi12_bus_debug_tx_toggle_test(). */
    int parity = (ones % 2 == 0) ? 0 : 1; /* even parity: total 1-bits (data+parity) must be even */

    tx_bit(0); /* start bit (space) */
    for (int i = 0; i < 7; i++) {
        tx_bit((data >> i) & 1); /* LSB first */
    }
    tx_bit(parity);
    tx_bit(1); /* stop bit (mark) */
}

static void send_break_and_marking(void)
{
    gpio_set_level(BOARD_PIN_SDI12_TXD, 1); /* SDI-12 polarity: break = space = HIGH */
    esp_rom_delay_us(SDI12_BREAK_US);
    gpio_set_level(BOARD_PIN_SDI12_TXD, 0); /* marking = LOW */
    esp_rom_delay_us(SDI12_MARKING_US);
}

/* Waits for one character's start-bit edge, then busy-samples its 7
 * data bits + parity at the bit centers. Returns ESP_ERR_TIMEOUT if no
 * edge arrives within timeout_ms. */
static esp_err_t rx_char(uint32_t timeout_ms, char *out_char)
{
    xSemaphoreTake(s_edge_sem, 0); /* drain any stale signal */
    gpio_set_intr_type(BOARD_PIN_SDI12_RXD, GPIO_INTR_POSEDGE);
    gpio_intr_enable(BOARD_PIN_SDI12_RXD);

    BaseType_t got = xSemaphoreTake(s_edge_sem, pdMS_TO_TICKS(timeout_ms));
    gpio_intr_disable(BOARD_PIN_SDI12_RXD);
    if (got != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Edge = start of the start bit. Sample each subsequent bit at its
     * center: 1.5 bit periods from the edge lands mid first-data-bit. */
    esp_rom_delay_us(SDI12_BIT_PERIOD_US + SDI12_BIT_PERIOD_US / 2);

    uint8_t data = 0;
    for (int i = 0; i < 7; i++) {
        int level = gpio_get_level(BOARD_PIN_SDI12_RXD);
        if (!level) {
            data |= (1 << i); /* SDI-12 polarity: LOW -> bit value 1 */
        }
        esp_rom_delay_us(SDI12_BIT_PERIOD_US);
    }
    /* Parity bit center reached now; not strictly enforced (log-only
     * would require the caller context) - malformed characters simply
     * surface as a parse failure upstream. */
    esp_rom_delay_us(SDI12_BIT_PERIOD_US + SDI12_INTERCHAR_GUARD_US);

    *out_char = (char)(data & 0x7F);
    return ESP_OK;
}

static esp_err_t rx_response(char *buf, size_t buf_size, uint32_t first_char_timeout_ms,
                              uint32_t interchar_timeout_ms)
{
    size_t len = 0;
    bool first = true;

    for (;;) {
        char c;
        esp_err_t err = rx_char(first ? first_char_timeout_ms : interchar_timeout_ms, &c);
        if (err != ESP_OK) {
            if (!first) {
                break; /* sensor stopped sending - treat as end of response */
            }
            return err; /* no response at all */
        }
        first = false;

        if (len < buf_size - 1) {
            buf[len++] = c;
        }
        if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') {
            break;
        }
        if (len >= buf_size - 1) {
            break;
        }
    }

    buf[len] = '\0';
    return ESP_OK;
}

/* ---- public API ---- */

/* Configures `pin` as a plain output and drives it to `level`, if the
 * pin is actually set in board_pins.h (a handful of these - the
 * RS485/RS232 disable pins - are optional depending on board
 * revision). */
static esp_err_t init_output_pin(gpio_num_t pin, int level)
{
    if (!board_pin_is_set(pin)) {
        return ESP_OK;
    }
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&conf);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(pin, level);
}

esp_err_t sdi12_bus_init(void)
{
    if (!board_pin_is_set(BOARD_PIN_SDI12_TXD) || !board_pin_is_set(BOARD_PIN_SDI12_RXD) ||
        !board_pin_is_set(BOARD_PIN_SDI12_TX_EN) || !board_pin_is_set(BOARD_PIN_SDI12_RX_EN)) {
        ESP_LOGE(TAG, "SDI-12 pins not configured in board_pins.h, SDI-12 bus disabled");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = init_output_pin(BOARD_PIN_SDI12_TXD, 0); /* idle = marking = LOW */
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t rx_conf = {
        .pin_bit_mask = board_pin_bit_mask(BOARD_PIN_SDI12_RXD),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* defined idle=LOW if nothing drives the line */
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&rx_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = init_output_pin(BOARD_PIN_SDI12_TX_EN, 0); /* start disabled; asserted only while sending */
    if (err != ESP_OK) {
        return err;
    }
    err = init_output_pin(BOARD_PIN_SDI12_RX_EN, 1); /* left enabled permanently - always listening */
    if (err != ESP_OK) {
        return err;
    }

    /* SER_TX/SER_RX are shared with RS485/RS232 transceivers on this
     * board - confirmed with DPTechnics support (Daan) that *_TX_EN
     * must be LOW and *_RX_EN must be HIGH to fully disable each
     * transceiver. This was previously (incorrectly) assumed to be
     * LOW for both - RS485/RS232's RX_EN pins are evidently active-low
     * receiver-enables (a common RS-485 transceiver DE/~RE pattern),
     * not simple active-high output-enables like SDI-12's own RX_EN.
     * Leaving them LOW left both other transceivers' receivers
     * actively driving the shared SER_RXD line, contending with
     * SDI-12's own U6 output - the real cause of RXD never seeing a
     * coherent signal despite every other link in the SDI-12 receive
     * chain checking out individually. */
    err = init_output_pin(BOARD_PIN_RS485_TX_EN, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = init_output_pin(BOARD_PIN_RS485_RX_EN, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = init_output_pin(BOARD_PIN_RS232_TX_EN, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = init_output_pin(BOARD_PIN_RS232_RX_EN, 1);
    if (err != ESP_OK) {
        return err;
    }

    if (board_pin_is_set(BOARD_PIN_SDI12_BUS_POWER)) {
        err = init_output_pin(BOARD_PIN_SDI12_BUS_POWER, 1); /* power sensors on; TODO verify active level */
        if (err != ESP_OK) {
            return err;
        }
    }

    s_bus_mutex = xSemaphoreCreateMutex();
    s_edge_sem = xSemaphoreCreateBinary();
    if (!s_bus_mutex || !s_edge_sem) {
        return ESP_ERR_NO_MEM;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = already installed elsewhere */
        return err;
    }
    err = gpio_isr_handler_add(BOARD_PIN_SDI12_RXD, sdi12_gpio_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    gpio_intr_disable(BOARD_PIN_SDI12_RXD);

    s_initialized = true;
    ESP_LOGI(TAG, "SDI-12 bus initialized (TXD=GPIO%d, RXD=GPIO%d)", BOARD_PIN_SDI12_TXD, BOARD_PIN_SDI12_RXD);
    return ESP_OK;
}

esp_err_t sdi12_send_command(char addr, const char *cmd_body, char *resp_buf, size_t resp_buf_len,
                              uint32_t response_timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!resp_buf || resp_buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);

    char cmd[16];
    int n = snprintf(cmd, sizeof(cmd), "%c%s!", addr, cmd_body ? cmd_body : "");
    if (n < 0 || n >= (int)sizeof(cmd)) {
        xSemaphoreGive(s_bus_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_set_level(BOARD_PIN_SDI12_TX_EN, 1); /* enable the TX buffer onto the bus; TODO verify active level */

    send_break_and_marking();
    for (const char *p = cmd; *p; p++) {
        tx_char(*p);
    }

    gpio_set_level(BOARD_PIN_SDI12_TX_EN, 0); /* back to high-Z; RX_EN stays enabled permanently */

    esp_err_t err = rx_response(resp_buf, resp_buf_len, response_timeout_ms, 100);

    xSemaphoreGive(s_bus_mutex);
    return err;
}

esp_err_t sdi12_scan_addresses(char *found, size_t max_found, size_t *out_count)
{
    if (!found || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char ADDR_SET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    size_t n = 0;
    for (const char *p = ADDR_SET; *p; p++) {
        char resp[16];
        if (sdi12_send_command(*p, "", resp, sizeof(resp), 100) == ESP_OK && n < max_found) {
            found[n++] = *p;
        }
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t sdi12_change_address(char old_addr, char new_addr)
{
    char body[4];
    snprintf(body, sizeof(body), "A%c", new_addr);
    char resp[16];
    return sdi12_send_command(old_addr, body, resp, sizeof(resp), 200);
}

esp_err_t sdi12_measure(char addr, uint8_t *num_values, uint32_t *wait_s)
{
    char resp[16];
    esp_err_t err = sdi12_send_command(addr, "M", resp, sizeof(resp), 200);
    if (err != ESP_OK) {
        return err;
    }
    /* response = "atttn\r\n" */
    if (strlen(resp) < 5 || resp[0] != addr) {
        return ESP_FAIL;
    }
    char ttt[4] = { resp[1], resp[2], resp[3], '\0' };
    *wait_s = (uint32_t)atoi(ttt);
    *num_values = (uint8_t)(resp[4] - '0');
    return ESP_OK;
}

esp_err_t sdi12_read_data(char addr, uint8_t data_index, char *resp_buf, size_t resp_buf_len)
{
    char body[4];
    snprintf(body, sizeof(body), "D%u", (unsigned)(data_index % 10));
    return sdi12_send_command(addr, body, resp_buf, resp_buf_len, 200);
}

void sdi12_bus_debug_tx_toggle_test(int cycles, uint32_t half_period_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "debug TX toggle test: bus not initialized");
        return;
    }
    ESP_LOGW(TAG, "debug TX toggle test starting: %d cycles, %" PRIu32 " ms/half-cycle - "
                  "watch the SDI-12 DATA line with a meter now", cycles, half_period_ms);
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);

    /* RX_EN is already permanently enabled (sdi12_bus_init()) - if the
     * receive chain (U6/RX_EN/RXD wiring+GPIO config) works at all, our
     * own TXD transitions on the shared bus should be visible as edges
     * on RXD too (a loopback self-test). GPIO_INTR_ANYEDGE (not the
     * normal rx_char()'s POSEDGE-only) to catch both directions of this
     * slow toggle. */
    s_debug_edge_count = 0;
    gpio_set_intr_type(BOARD_PIN_SDI12_RXD, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(BOARD_PIN_SDI12_RXD);

    gpio_set_level(BOARD_PIN_SDI12_TX_EN, 1); /* enable the TX buffer onto the bus */
    for (int i = 0; i < cycles; i++) {
        gpio_set_level(BOARD_PIN_SDI12_TXD, 1);
        vTaskDelay(pdMS_TO_TICKS(half_period_ms));
        gpio_set_level(BOARD_PIN_SDI12_TXD, 0);
        vTaskDelay(pdMS_TO_TICKS(half_period_ms));
    }
    gpio_set_level(BOARD_PIN_SDI12_TXD, 0); /* idle = marking = LOW */
    gpio_set_level(BOARD_PIN_SDI12_TX_EN, 0); /* back to high-Z */

    gpio_intr_disable(BOARD_PIN_SDI12_RXD);
    xSemaphoreGive(s_bus_mutex);
    ESP_LOGW(TAG, "debug TX toggle test done - RXD loopback saw %" PRIu32 " edge(s) (expect up to %d if the "
                  "receive chain works; 0 means RX_EN/RXD/U6 isn't passing the bus through to the MCU)",
             s_debug_edge_count, cycles * 2);
}

void sdi12_bus_debug_rx_monitor(uint32_t duration_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "debug RX monitor: bus not initialized");
        return;
    }
    /* Unlike sdi12_bus_debug_tx_toggle_test(), this never drives TXD/TX_EN
     * at all - it only listens. Meant for manually driving the SDI-12
     * DATA line from an external source (e.g. briefly touching it to
     * 5V and back) to test the RX_EN/RXD/U6 chain in complete isolation
     * from our own TX circuit (U5), which is already independently
     * confirmed working. Logs each edge live as it's detected so it can
     * be correlated with a manual action in real time, rather than only
     * reporting a final count after the fact. */
    ESP_LOGW(TAG, "debug RX monitor starting for %" PRIu32 " ms - manually drive the SDI-12 DATA "
                  "line now (e.g. briefly connect it to 5V and back) and watch for "
                  "'edge detected' lines below",
             duration_ms);
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);

    s_debug_edge_count = 0;
    gpio_set_intr_type(BOARD_PIN_SDI12_RXD, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(BOARD_PIN_SDI12_RXD);

    uint32_t last_count = 0;
    int64_t start_us = esp_timer_get_time();
    while ((esp_timer_get_time() - start_us) / 1000 < duration_ms) {
        uint32_t count = s_debug_edge_count;
        if (count != last_count) {
            ESP_LOGW(TAG, "debug RX monitor: edge detected! (total=%" PRIu32 ")", count);
            last_count = count;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    gpio_intr_disable(BOARD_PIN_SDI12_RXD);
    xSemaphoreGive(s_bus_mutex);
    ESP_LOGW(TAG, "debug RX monitor done - saw %" PRIu32 " edge(s) total", s_debug_edge_count);
}
