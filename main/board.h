// Board support for the Spotpear SP-ESP32-S3-1.28-BOX (Xiaozhi).
// Pins and init sequence were verified on this board with a hardware
// diagnostic firmware.
#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "hal/spi_types.h"

namespace board {

// GC9A01 240x240 round IPS on SPI3 (A: factory binary + diag)
constexpr spi_host_device_t LCD_SPI_HOST = SPI3_HOST;
constexpr gpio_num_t LCD_SCLK = GPIO_NUM_4;
constexpr gpio_num_t LCD_MOSI = GPIO_NUM_2;
constexpr gpio_num_t LCD_CS = GPIO_NUM_5;
constexpr gpio_num_t LCD_DC = GPIO_NUM_47;
constexpr gpio_num_t LCD_RST = GPIO_NUM_38;
constexpr int LCD_PCLK_HZ = 40 * 1000 * 1000;
constexpr int LCD_WIDTH = 240;
constexpr int LCD_HEIGHT = 240;

// Backlight: LEDC PWM, active-low, 25 kHz
constexpr gpio_num_t BL_PIN = GPIO_NUM_42;
constexpr int BL_PWM_HZ = 25000;

// Power hold latch (RTC GPIO), driven HIGH only
constexpr gpio_num_t POWER_HOLD = GPIO_NUM_3;

// Not used by the radar and left unconfigured: touch 11/7/6/12, ES8311 15/14 +
// I2S 16/9/45/8/10 + PA 46, battery 1/41, TF card 13/17/18/21, boot 0, LED 48.

void PowerHold();
esp_err_t BacklightInit();
void BacklightSet(int percent);
esp_err_t LcdInit();

// Push `lines` rows starting at row `y` of big-endian (byte-swapped) RGB565
// pixels. Blocks until the DMA transfer has completed. Only valid before LVGL
// takes over the panel (it replaces the transfer-done callback).
esp_err_t LcdPushRows(int y, int lines, const uint16_t* pixels);

// Handles for LVGL (esp_lvgl_port).
esp_lcd_panel_io_handle_t LcdIo();
esp_lcd_panel_handle_t LcdPanel();

}  // namespace board
