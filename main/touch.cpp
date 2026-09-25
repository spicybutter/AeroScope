#include "touch.h"

#include <utility>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

namespace touch {

static const char* TAG = "TOUCH";

static constexpr i2c_port_t PORT = I2C_NUM_1;
static constexpr gpio_num_t PIN_SDA = GPIO_NUM_11;
static constexpr gpio_num_t PIN_SCL = GPIO_NUM_7;
static constexpr gpio_num_t PIN_RST = GPIO_NUM_6;
static constexpr gpio_num_t PIN_INT = GPIO_NUM_12;  // input with pull-up, never driven
static constexpr uint8_t ADDR = 0x15;
static constexpr int SIZE = 240;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

bool Init()
{
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = PORT;
    bus.sda_io_num = PIN_SDA;
    bus.scl_io_num = PIN_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }

    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << PIN_RST;
    rst.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    gpio_config_t irq = {};
    irq.pin_bit_mask = 1ULL << PIN_INT;
    irq.mode = GPIO_MODE_INPUT;
    irq.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&irq);

    // Reset pulse (Xiaozhi sequence)
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = ADDR;
    dev.scl_speed_hz = 400 * 1000;
    uint8_t reg = 0xA3, id = 0;
    if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK ||
        i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 100) != ESP_OK) {
        ESP_LOGW(TAG, "CST816D not responding - touch disabled");
        if (s_dev) i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        s_dev = nullptr;
        s_bus = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "CST816D ready (0x%02X, id reg 0x%02X)", ADDR, id);
    return true;
}

bool Available() { return s_dev != nullptr; }

bool Read(int& x, int& y)
{
    if (!s_dev) return false;
    uint8_t reg = 0x02, buf[5];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 20) != ESP_OK) return false;
    if (buf[0] == 0xFF || (buf[0] & 0x0F) == 0) return false;  // no finger
    int rx = ((buf[1] & 0x0F) << 8) | buf[2];
    int ry = ((buf[3] & 0x0F) << 8) | buf[4];

    // Calibration (#29): make touches land where the display draws.
    const auto s = settings::Current();
    if (s->touchSwapXY) std::swap(rx, ry);
    if (s->touchMirrorX) rx = SIZE - 1 - rx;
    if (s->touchMirrorY) ry = SIZE - 1 - ry;
    x = rx < 0 ? 0 : rx >= SIZE ? SIZE - 1 : rx;
    y = ry < 0 ? 0 : ry >= SIZE ? SIZE - 1 : ry;
    return true;
}

}  // namespace touch
