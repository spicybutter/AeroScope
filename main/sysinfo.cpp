#include "sysinfo.h"

#include <cstdio>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"

namespace sysinfo {

static const char* TAG = "SYS";
static std::string s_reset;
static std::string s_crash;
static bool s_crashed = false;

static const char* ReasonName(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
    }
}

void Init()
{
    const esp_reset_reason_t r = esp_reset_reason();
    s_reset = ReasonName(r);
    s_crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                r == ESP_RST_BROWNOUT;

    // The core dump partition keeps the last crash until it is overwritten by the next one.
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t* summary = new esp_core_dump_summary_t();
        if (esp_core_dump_get_summary(summary) == ESP_OK) {
            char buf[160];
            int n = snprintf(buf, sizeof(buf), "task '%s' PC 0x%08lx backtrace", summary->exc_task,
                             static_cast<unsigned long>(summary->exc_pc));
            for (uint32_t i = 0; i < summary->exc_bt_info.depth && i < 6 && n < (int)sizeof(buf) - 12; i++)
                n += snprintf(buf + n, sizeof(buf) - n, " 0x%08lx", static_cast<unsigned long>(summary->exc_bt_info.bt[i]));
            s_crash = buf;
        }
        delete summary;
    }

    ESP_LOGI(TAG, "firmware %s, ESP-IDF %s", FirmwareVersion().c_str(), IdfVersion().c_str());
    if (s_crashed) {
        ESP_LOGW(TAG, "last reset: %s", s_reset.c_str());
    } else {
        ESP_LOGI(TAG, "last reset: %s", s_reset.c_str());
    }
    if (!s_crash.empty()) ESP_LOGW(TAG, "stored crash: %s", s_crash.c_str());
}

std::string ResetReason() { return s_reset; }
bool LastBootWasCrash() { return s_crashed; }
std::string CrashSummary() { return s_crash; }

std::string FirmwareVersion()
{
    const esp_app_desc_t* d = esp_app_get_description();
    return std::string(d->version) + " (" + d->date + " " + d->time + ")";
}

std::string IdfVersion() { return esp_get_idf_version(); }

void ApplyLogLevel(int level)
{
    if (level < ESP_LOG_ERROR) level = ESP_LOG_ERROR;
    if (level > ESP_LOG_DEBUG) level = ESP_LOG_DEBUG;
    esp_log_level_set("*", static_cast<esp_log_level_t>(level));
    // Keep the Wi-Fi driver quiet below warnings unless debugging.
    esp_log_level_set("wifi", level >= ESP_LOG_DEBUG ? ESP_LOG_INFO : ESP_LOG_WARN);
}

}  // namespace sysinfo
