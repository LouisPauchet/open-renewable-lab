#include "i2c_bus.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";

#define MAX_CACHED_DEVICES 16

typedef struct {
    uint8_t addr7;
    i2c_master_dev_handle_t handle;
    bool in_use;
} device_entry_t;

static bool s_initialized;
static i2c_master_bus_handle_t s_bus;
static device_entry_t s_devices[MAX_CACHED_DEVICES];
static SemaphoreHandle_t s_mutex;

esp_err_t i2c_bus_init(void)
{
    if (!board_pin_is_set(BOARD_PIN_I2C_SDA) || !board_pin_is_set(BOARD_PIN_I2C_SCL)) {
        ESP_LOGE(TAG, "I2C pins not configured in board_pins.h, I2C bus disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (board_pin_is_set(BOARD_PIN_I2C_BUS_POWER)) {
        gpio_config_t pwr_conf = {
            .pin_bit_mask = 1ULL << BOARD_PIN_I2C_BUS_POWER,
            .mode = GPIO_MODE_OUTPUT,
        };
        esp_err_t pwr_err = gpio_config(&pwr_conf);
        if (pwr_err != ESP_OK) {
            return pwr_err;
        }
        gpio_set_level(BOARD_PIN_I2C_BUS_POWER, 1); /* power external I2C sensors on; TODO verify active level */
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_devices, 0, sizeof(s_devices));
    s_initialized = true;
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
    return ESP_OK;
}

/* Caller must hold s_mutex. */
static esp_err_t get_or_add_device(uint8_t addr7, i2c_master_dev_handle_t *out_handle)
{
    for (int i = 0; i < MAX_CACHED_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].addr7 == addr7) {
            *out_handle = s_devices[i].handle;
            return ESP_OK;
        }
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_CACHED_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = BOARD_I2C_CLOCK_HZ,
    };

    i2c_master_dev_handle_t handle;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &handle);
    if (err != ESP_OK) {
        return err;
    }

    s_devices[free_slot].addr7 = addr7;
    s_devices[free_slot].handle = handle;
    s_devices[free_slot].in_use = true;
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t i2c_bus_write(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t handle;
    esp_err_t err = get_or_add_device(addr7, &handle);
    if (err == ESP_OK) {
        err = i2c_master_transmit(handle, wbuf, wlen, (int)timeout_ms);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_write_read(uint8_t addr7, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf, size_t rlen,
                              uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t handle;
    esp_err_t err = get_or_add_device(addr7, &handle);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(handle, wbuf, wlen, rbuf, rlen, (int)timeout_ms);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_scan(uint8_t *found, size_t max_found, size_t *out_count)
{
    if (!s_initialized || !found || !out_count) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = 0;
    for (uint8_t addr = 0x03; addr <= 0x77 && n < max_found; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            found[n++] = addr;
        }
    }
    xSemaphoreGive(s_mutex);

    *out_count = n;
    return ESP_OK;
}
