#include "board.h"

#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace board {

static const char* TAG = "board";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;

void PowerHold()
{
    // Xiaozhi InitializePowerSaveTimer() sequence. Never driven low here.
    rtc_gpio_init(POWER_HOLD);
    rtc_gpio_set_direction(POWER_HOLD, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(POWER_HOLD, 1);
}

esp_err_t BacklightInit()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = BL_PWM_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t ch = {};
    ch.gpio_num = BL_PIN;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;  // inverted output: duty 0 = backlight off
    ch.flags.output_invert = 1;
    return ledc_channel_config(&ch);
}

void BacklightSet(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static bool OnColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

esp_err_t LcdInit()
{
    s_flush_done = xSemaphoreCreateBinary();

    // Same values as GC9A01_PANEL_BUS_SPI_CONFIG / GC9A01_PANEL_IO_SPI_CONFIG
    // (those C macros don't compile as C++ with -Werror=missing-field-initializers).
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = LCD_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.data4_io_num = -1;
    buscfg.data5_io_num = -1;
    buscfg.data6_io_num = -1;
    buscfg.data7_io_num = -1;
    buscfg.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = LCD_CS;
    io_config.dc_gpio_num = LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = LCD_PCLK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = OnColorTransDone;
    io_config.user_ctx = nullptr;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &s_io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(s_io, &panel_config, &s_panel), TAG, "panel");

    // Exact Xiaozhi/Spotpear sequence (verified by the diagnostic firmware):
    // one horizontal flip via MADCTL MX (0x48) - esp_lcd never sends 0xB6.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");

    static const uint8_t d62[] = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70};
    static const uint8_t d63[] = {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70};
    static const uint8_t d36[] = {0x48};
    static const uint8_t dC3[] = {0x1F};
    static const uint8_t dC4[] = {0x1F};
    esp_lcd_panel_io_tx_param(s_io, 0x62, d62, sizeof(d62));
    esp_lcd_panel_io_tx_param(s_io, 0x63, d63, sizeof(d63));
    esp_lcd_panel_io_tx_param(s_io, 0x36, d36, sizeof(d36));
    esp_lcd_panel_io_tx_param(s_io, 0xC3, dC3, sizeof(dC3));
    esp_lcd_panel_io_tx_param(s_io, 0xC4, dC4, sizeof(dC4));
    return ESP_OK;
}

esp_lcd_panel_io_handle_t LcdIo() { return s_io; }
esp_lcd_panel_handle_t LcdPanel() { return s_panel; }

esp_err_t LcdPushRows(int y, int lines, const uint16_t* pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + lines, pixels);
    if (err != ESP_OK) return err;
    return xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

}  // namespace board
