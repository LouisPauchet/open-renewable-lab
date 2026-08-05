#include "i2c_bus.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

/* If a downstream device was left holding SDA low mid-transaction (a
 * brief power glitch on the switched I2C rail during a reboot
 * interrupting it mid-byte is the leading suspect - GPIO0 doubles as
 * both the 3V3_SW rail enable and the boot-strap pin, so its state
 * isn't guaranteed for the first few instructions of a reset), the
 * shared, open-drain I2C bus stays wedged for every device on it -
 * i2c_master_probe()/transmit() calls all time out forever, for every
 * single address, confirmed on real hardware (a full bus scan timing
 * out address-by-address after a reboot, reproduced even on firmware
 * predating any of this session's own I2C-adjacent changes). Nothing
 * short of forcing the stuck device to finish its transaction and
 * release the bus recovers from this - clock SCL manually (up to 9
 * pulses, enough for a full byte + ack) before the I2C peripheral
 * driver claims these pins, standard I2C bus-recovery practice, then
 * issue a manual STOP condition so every device's own bus-protocol
 * state machine resets cleanly, not just the one that was stuck. */
static void recover_stuck_bus(gpio_num_t sda, gpio_num_t scl)
{
    gpio_config_t od_conf = {
        .pin_bit_mask = board_pin_bit_mask(sda) | board_pin_bit_mask(scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&od_conf);
    gpio_set_level(sda, 1); /* release - let the pull-up show the real state */
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);

    if (gpio_get_level(sda)) {
        return; /* not stuck - nothing to recover */
    }

    ESP_LOGW(TAG, "I2C bus stuck (SDA held low) - attempting recovery");
    for (int i = 0; i < 9 && !gpio_get_level(sda); i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }

    /* Manual STOP condition: SDA low-to-high while SCL is high. */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);

    ESP_LOGW(TAG, "I2C bus recovery %s", gpio_get_level(sda) ? "succeeded" : "failed - SDA still held low");
}

static esp_err_t create_master_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

/* Caller must hold s_mutex. A device getting wedged mid-transaction
 * during normal runtime (not just at boot, see recover_stuck_bus()
 * above) is a real, repeatable failure mode on real hardware -
 * confirmed via a live multimeter reading showing SDA held at 0V
 * while SCL sat at its normal idle-high level, minutes into uptime,
 * well after boot's one-shot recovery already ran and found nothing
 * wrong yet. A plain retry never recovers from this since the bus
 * stays wedged until something forces the stuck device to release it
 * - previously required a full power cycle. Called whenever a
 * transaction actually times out (as opposed to a clean NACK, which
 * just means "no device answered" and doesn't indicate a wedged bus). */
static void reset_bus_locked(void)
{
    ESP_LOGW(TAG, "I2C transaction timeout - resetting bus");

    for (int i = 0; i < MAX_CACHED_DEVICES; i++) {
        if (s_devices[i].in_use) {
            i2c_master_bus_rm_device(s_devices[i].handle);
        }
    }
    memset(s_devices, 0, sizeof(s_devices));

    i2c_del_master_bus(s_bus);
    s_bus = NULL;

    recover_stuck_bus(BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);

    esp_err_t err = create_master_bus();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus reset failed: %s - I2C disabled until next reboot", esp_err_to_name(err));
        s_initialized = false;
    }
}

esp_err_t i2c_bus_init(void)
{
    if (!board_pin_is_set(BOARD_PIN_I2C_SDA) || !board_pin_is_set(BOARD_PIN_I2C_SCL)) {
        ESP_LOGE(TAG, "I2C pins not configured in board_pins.h, I2C bus disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (board_pin_is_set(BOARD_PIN_I2C_BUS_POWER)) {
        gpio_config_t pwr_conf = {
            .pin_bit_mask = board_pin_bit_mask(BOARD_PIN_I2C_BUS_POWER),
            .mode = GPIO_MODE_OUTPUT,
        };
        esp_err_t pwr_err = gpio_config(&pwr_conf);
        if (pwr_err != ESP_OK) {
            return pwr_err;
        }
        gpio_set_level(BOARD_PIN_I2C_BUS_POWER, 1); /* power external I2C sensors on; TODO verify active level */
        vTaskDelay(pdMS_TO_TICKS(10)); /* let downstream sensors actually power up before touching the bus */
    }

    recover_stuck_bus(BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);

    esp_err_t err = create_master_bus();
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
        if (err == ESP_ERR_TIMEOUT) {
            reset_bus_locked();
        }
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
        if (err == ESP_ERR_TIMEOUT) {
            reset_bus_locked();
        }
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
        if (!s_initialized) {
            break; /* reset_bus_locked() gave up - see its own log line for why */
        }
        esp_err_t probe_err = i2c_master_probe(s_bus, addr, 50);
        if (probe_err == ESP_OK) {
            found[n++] = addr;
        } else if (probe_err == ESP_ERR_TIMEOUT) {
            reset_bus_locked();
        }
    }
    xSemaphoreGive(s_mutex);

    *out_count = n;
    return ESP_OK;
}
