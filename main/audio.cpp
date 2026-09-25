#include "audio.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

namespace audio {

static const char* TAG = "AUDIO";

static constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
static constexpr gpio_num_t PIN_SDA = GPIO_NUM_15;
static constexpr gpio_num_t PIN_SCL = GPIO_NUM_14;
static constexpr gpio_num_t PIN_MCLK = GPIO_NUM_16;
static constexpr gpio_num_t PIN_BCLK = GPIO_NUM_9;
static constexpr gpio_num_t PIN_WS = GPIO_NUM_45;
static constexpr gpio_num_t PIN_DOUT = GPIO_NUM_8;
static constexpr gpio_num_t PIN_DIN = GPIO_NUM_10;
static constexpr gpio_num_t PIN_PA = GPIO_NUM_46;
static constexpr uint8_t ES8311_ADDR_7BIT = 0x18;
static constexpr int RATE = 24000;
static constexpr int MIC_GAIN_DB = 30;
static constexpr int FRAME = 480;  // 20 ms

static i2c_master_bus_handle_t s_bus;
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_dev;
static QueueHandle_t s_queue;
static std::mutex s_codec_mutex;  // playback task vs self-test

struct Request {
    Sound sound;
    bool force;
};

// ---------------------------------------------------------------------------
// Bring-up (diagnostic firmware sequence)

bool ReadChipId(uint8_t& id1, uint8_t& id2)
{
    if (!s_bus) return false;
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = ES8311_ADDR_7BIT;
    cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(s_bus, &cfg, &dev) != ESP_OK) return false;
    uint8_t r1 = 0xFD, r2 = 0xFE;
    const bool ok = i2c_master_transmit_receive(dev, &r1, 1, &id1, 1, 100) == ESP_OK &&
                    i2c_master_transmit_receive(dev, &r2, 1, &id2, 1, 100) == ESP_OK;
    i2c_master_bus_rm_device(dev);
    return ok;
}

static esp_err_t CreateDuplex()
{
    i2s_chan_config_t chan = {};
    chan.id = I2S_NUM_0;
    chan.role = I2S_ROLE_MASTER;
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear_after_cb = true;
    chan.auto_clear_before_cb = false;
    chan.intr_priority = 0;
    esp_err_t err = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t std = {};
    std.clk_cfg.sample_rate_hz = RATE;
    std.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std.slot_cfg.ws_pol = false;
    std.slot_cfg.bit_shift = true;
    std.slot_cfg.left_align = true;
    std.slot_cfg.big_endian = false;
    std.slot_cfg.bit_order_lsb = false;
    std.gpio_cfg.mclk = PIN_MCLK;
    std.gpio_cfg.bclk = PIN_BCLK;
    std.gpio_cfg.ws = PIN_WS;
    std.gpio_cfg.dout = PIN_DOUT;
    std.gpio_cfg.din = PIN_DIN;
    if ((err = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) return err;
    if ((err = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) return err;
    if ((err = i2s_channel_enable(s_tx)) != ESP_OK) return err;
    return i2s_channel_enable(s_rx);
}

static void SetPa(bool on) { gpio_set_level(PIN_PA, on ? 1 : 0); }

static void AudioTask(void*);

bool Init()
{
    // Codec I2C: no internal pull-ups (board has external ones), like Xiaozhi.
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_PORT;
    bus.sda_io_num = PIN_SDA;
    bus.scl_io_num = PIN_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "codec I2C bus init failed");
        return false;
    }
    uint8_t id1 = 0, id2 = 0;
    if (!ReadChipId(id1, id2) || id1 != 0x83 || id2 != 0x11) {
        ESP_LOGW(TAG, "ES8311 not found (id %02X %02X) - sound disabled", id1, id2);
        return false;
    }
    if (CreateDuplex() != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = s_rx;
    i2s_cfg.tx_handle = s_tx;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_PORT;
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = s_bus;
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!data_if || !ctrl_if) return false;

    uint8_t reset = 0x1F;  // Xiaozhi ResetCodec()
    ctrl_if->write_reg(ctrl_if, 0x00, 1, &reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    es8311_codec_cfg_t es = {};
    es.ctrl_if = ctrl_if;
    es.gpio_if = audio_codec_new_gpio();
    es.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es.pa_pin = PIN_PA;
    es.pa_reverted = false;
    es.use_mclk = true;
    es.hw_gain.pa_voltage = 5.0;
    es.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t* codec_if = es8311_codec_new(&es);
    if (!codec_if) return false;

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = data_if;
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) return false;

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = RATE;
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_in_gain(s_dev, MIC_GAIN_DB);
    SetPa(false);  // output idle -> PA off (Xiaozhi UpdateDeviceState)

    s_queue = xQueueCreate(4, sizeof(Request));
    xTaskCreatePinnedToCore(AudioTask, "audio", 4096, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "ES8311 ready");
    return true;
}

bool Available() { return s_dev != nullptr; }

// ---------------------------------------------------------------------------
// Sound synthesis

struct Note {
    int hz;      // 0 = silence
    int ms;
};

static void Render(const Note* notes, int count, int16_t* out, int total)
{
    int pos = 0;
    for (int n = 0; n < count && pos < total; n++) {
        const int len = std::min(RATE * notes[n].ms / 1000, total - pos);
        const int fade = std::min(RATE / 200, len / 2);  // 5 ms edges, no clicks
        for (int i = 0; i < len; i++) {
            float env = 1.0f;
            if (i < fade) env = static_cast<float>(i) / fade;
            else if (i > len - fade) env = static_cast<float>(len - i) / fade;
            const float v = notes[n].hz ? std::sin(2.0f * static_cast<float>(M_PI) * notes[n].hz * i / RATE) : 0.0f;
            out[pos + i] = static_cast<int16_t>(22000 * env * v);  // ~ -3.5 dBFS
        }
        pos += len;
    }
    while (pos < total) out[pos++] = 0;
}

static void PlayNotes(const Note* notes, int count, int volume)
{
    int ms = 0;
    for (int i = 0; i < count; i++) ms += notes[i].ms;
    const int total = RATE * ms / 1000;
    int16_t* buf = static_cast<int16_t*>(heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buf) return;
    Render(notes, count, buf, total);
    std::lock_guard<std::mutex> lock(s_codec_mutex);
    esp_codec_dev_set_out_vol(s_dev, volume);
    SetPa(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_codec_dev_write(s_dev, buf, total * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(100));  // let the DMA drain before cutting the PA
    SetPa(false);
    heap_caps_free(buf);
}

static void AudioTask(void*)
{
    static const Note chime[] = {{880, 160}, {0, 40}, {660, 260}};
    static const Note watch[] = {{1000, 110}, {0, 80}, {1000, 110}, {0, 80}, {1320, 180}};
    static const Note emergency[] = {{700, 220}, {1000, 220}, {700, 220}, {1000, 220}, {700, 220}, {1000, 300}};
    static const Note test[] = {{1000, 500}};
    Request r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) != pdTRUE) continue;
        const auto s = settings::Current();
        if (!r.force && (!s->sound || s->volume <= 0)) continue;
        // Previews use the configured volume (or 70 if sound is muted/zero, so they are audible).
        const int vol = r.force && s->volume <= 0 ? 70 : s->volume;
        switch (r.sound) {
        case Sound::Chime: PlayNotes(chime, 3, vol); break;
        case Sound::Watch: PlayNotes(watch, 5, vol); break;
        case Sound::Emergency: PlayNotes(emergency, 6, vol); break;
        case Sound::Test: PlayNotes(test, 1, vol); break;
        }
    }
}

void Play(Sound s, bool force)
{
    if (!s_queue) return;
    Request r{s, force};
    xQueueSend(s_queue, &r, 0);  // drop if already busy with several sounds
}

// ---------------------------------------------------------------------------
// Self-test helper: tone out, microphone in

static float MicPeakDb(int ms)
{
    int16_t buf[FRAME];
    float best = -90.0f;
    const int frames = std::max(1, ms / 20);
    for (int f = 0; f < frames; f++) {
        if (esp_codec_dev_read(s_dev, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) break;
        double sum = 0;
        for (int i = 0; i < FRAME; i++) sum += static_cast<double>(buf[i]) * buf[i];
        const float rms = std::sqrt(sum / FRAME);
        best = std::max(best, rms > 0.5f ? 20.0f * std::log10(rms / 32768.0f) : -90.0f);
    }
    return best;
}

float MeasureToneLoopback(int hz, int ms, float& baselineDb)
{
    baselineDb = -90.0f;
    if (!s_dev) return -90.0f;
    const int total = RATE * ms / 1000;
    int16_t* buf = static_cast<int16_t*>(heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buf) return -90.0f;
    const Note n[] = {{hz, ms}};
    Render(n, 1, buf, total);

    std::lock_guard<std::mutex> lock(s_codec_mutex);
    MicPeakDb(100);                   // flush stale samples
    baselineDb = MicPeakDb(500);      // quiet room
    esp_codec_dev_set_out_vol(s_dev, std::max(settings::Current()->volume, 70));
    SetPa(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    // The I2S write blocks for the tone's duration, so it runs in a helper task
    // while this task listens (the diagnostic firmware's proven arrangement).
    struct Job {
        int16_t* buf;
        int total;
        SemaphoreHandle_t done;
    } job{buf, total, xSemaphoreCreateBinary()};
    xTaskCreate(
        [](void* arg) {
            auto* j = static_cast<Job*>(arg);
            esp_codec_dev_write(s_dev, j->buf, j->total * sizeof(int16_t));
            xSemaphoreGive(j->done);
            vTaskDelete(nullptr);
        },
        "tone_out", 3072, &job, 4, nullptr);
    const float during = MicPeakDb(ms);
    xSemaphoreTake(job.done, pdMS_TO_TICKS(ms + 2000));
    vSemaphoreDelete(job.done);
    vTaskDelay(pdMS_TO_TICKS(100));  // let the DMA drain before cutting the PA
    SetPa(false);
    heap_caps_free(buf);
    return during;
}

}  // namespace audio
