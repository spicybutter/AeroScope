#include "selftest.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "aircraft.h"
#include "audio.h"
#include "board.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "timekeeping.h"
#include "touch.h"

namespace selftest {

static const char* TAG = "SELFTEST";

static AircraftManager* s_aircraft;
static std::mutex s_mutex;
static std::vector<Result> s_results;
static std::atomic<bool> s_running{false};

const char* StatusName(Status s)
{
    switch (s) {
    case Status::Pass: return "PASS";
    case Status::Warn: return "WARN";
    case Status::Fail: return "FAIL";
    default: return "SKIP";
    }
}

static void Add(const std::string& name, Status st, const std::string& detail)
{
    ESP_LOGI(TAG, "[%s] %s: %s", StatusName(st), name.c_str(), detail.c_str());
    std::lock_guard<std::mutex> lock(s_mutex);
    s_results.push_back({name, st, detail});
}

static std::string F(const char* fmt, double a, double b = 0, double c = 0)
{
    char buf[96];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    return buf;
}

// Diagnostic-firmware style PSRAM check, limited to 1 MiB so it is harmless at runtime.
static void TestPsram()
{
    if (!esp_psram_is_initialized()) {
        Add("PSRAM", Status::Fail, "not initialised");
        return;
    }
    const size_t size = esp_psram_get_size();
    constexpr size_t BYTES = 1024 * 1024;
    auto* p = static_cast<volatile uint32_t*>(heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM));
    if (!p) {
        Add("PSRAM", Status::Warn, F("%.0f MiB present, no 1 MiB block free", size / 1048576.0));
        return;
    }
    const size_t words = BYTES / 4;
    bool ok = true;
    for (uint32_t pattern : {0x55555555u, 0xAAAAAAAAu}) {
        for (size_t i = 0; i < words; i++) p[i] = pattern ^ static_cast<uint32_t>(i);
        for (size_t i = 0; i < words && ok; i++) ok = p[i] == (pattern ^ static_cast<uint32_t>(i));
    }
    heap_caps_free(const_cast<uint32_t*>(p));
    Add("PSRAM", ok && size == 8 * 1048576 ? Status::Pass : ok ? Status::Warn : Status::Fail,
        F("%.0f MiB, 1 MiB pattern test %s", size / 1048576.0) + (ok ? "OK" : "FAILED"));
}

static void TestMemory()
{
    const size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t minInt = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    Add("Internal RAM", minInt > 20 * 1024 ? Status::Pass : Status::Warn,
        F("%.0f KB free, lowest %.0f KB", freeInt / 1024.0, minInt / 1024.0));
    uint32_t flash = 0;
    esp_flash_get_physical_size(nullptr, &flash);
    Add("Flash", flash == 16 * 1048576 ? Status::Pass : Status::Warn, F("%.0f MiB", flash / 1048576.0));
}

static void TestTouch()
{
    if (!touch::Available()) {
        Add("Touch (CST816D)", Status::Fail, "controller not responding");
        return;
    }
    Add("Touch (CST816D)", Status::Pass, "controller responding at 0x15");
}

static void TestBacklight()
{
    const int keep = settings::Current()->brightness;
    for (int b : {10, 100, keep}) {
        board::BacklightSet(b);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    Add("Backlight", Status::Pass, "dimmed, full, restored (visual check)");
}

static void TestAudio()
{
    uint8_t a = 0, b = 0;
    if (!audio::ReadChipId(a, b)) {
        Add("Audio codec (ES8311)", Status::Fail, "no I2C answer at 0x18");
        return;
    }
    char id[32];
    snprintf(id, sizeof(id), "chip ID %02X %02X", a, b);
    Add("Audio codec (ES8311)", a == 0x83 && b == 0x11 ? Status::Pass : Status::Fail, id);
    if (!audio::Available()) {
        Add("Speaker + microphone", Status::Skip, "codec not initialised");
        return;
    }
    float baseline = 0;
    const float during = audio::MeasureToneLoopback(1000, 600, baseline);
    const bool heard = during > baseline + 10.0f;
    Add("Speaker + microphone", heard ? Status::Pass : Status::Warn,
        F("mic %.1f dBFS during 1 kHz tone vs %.1f dBFS quiet", during, baseline) +
            (heard ? "" : " - check the speaker is audible"));
}

static void TestNetwork()
{
    if (net::Connected())
        Add("Wi-Fi", net::Rssi() > -80 ? Status::Pass : Status::Warn,
            net::Ssid() + F(", %.0f dBm", net::Rssi()));
    else
        Add("Wi-Fi", Status::Fail, "not connected");
    Add("Time (NTP)", timekeeping::Synced() ? Status::Pass : Status::Warn,
        timekeeping::Synced() ? timekeeping::LocalTimestamp() : "not synced yet");
    if (s_aircraft) {
        const FetchStatus st = s_aircraft->Status();
        const auto s = settings::Current();
        if (!s->locationSet)
            Add("Flight data", Status::Skip, "no location set");
        else if (st.haveData && st.lastError.empty())
            Add("Flight data", Status::Pass, F("%.0f aircraft, last update %.0f s ago", st.total,
                                               (Millis() - st.lastSuccessMs) / 1000.0));
        else
            Add("Flight data", Status::Warn, st.lastError.empty() ? "waiting for first update" : st.lastError);
    }
}

static void Run(void*)
{
    ESP_LOGI(TAG, "self-test started");
    TestPsram();
    TestMemory();
    TestTouch();
    TestBacklight();
    TestAudio();
    TestNetwork();
    ESP_LOGI(TAG, "self-test finished");
    s_running = false;
    vTaskDelete(nullptr);
}

void Init(AircraftManager& aircraft) { s_aircraft = &aircraft; }

bool Start()
{
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) return false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_results.clear();
    }
    if (xTaskCreatePinnedToCore(Run, "selftest", 6144, nullptr, 2, nullptr, 0) != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

bool Running() { return s_running; }

std::vector<Result> Results()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_results;
}

}  // namespace selftest
