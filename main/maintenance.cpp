#include "maintenance.h"

#include <cstdint>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace maintenance {

static const char* TAG = "MAINT";
static constexpr uint32_t MAGIC = 0x52414452;  // "RADR"

// Survives esp_restart(); garbage after power-on, hence the magic.
RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint32_t s_scope;

static void EraseNamespace(const char* ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

void RequestReset(ResetScope scope)
{
    s_scope = static_cast<uint32_t>(scope);
    s_magic = MAGIC;
    static esp_timer_handle_t timer;
    if (!timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.name = "reset_restart";
        esp_timer_create(&args, &timer);
    }
    esp_timer_start_once(timer, 1000 * 1000);
}

bool ApplyPendingReset()
{
    if (s_magic != MAGIC) return false;
    const auto scope = static_cast<ResetScope>(s_scope);
    s_magic = 0;
    switch (scope) {
    case ResetScope::Settings:
        ESP_LOGW(TAG, "resetting radar settings");
        EraseNamespace("config");
        return false;
    case ResetScope::Wifi:
        ESP_LOGW(TAG, "forgetting all Wi-Fi networks");
        EraseNamespace("wifi");          // our saved-network list
        EraseNamespace("nvs.net80211");  // the Wi-Fi driver's own copy
        return false;
    case ResetScope::All:
        ESP_LOGW(TAG, "factory reset: erasing all settings and Wi-Fi");
        nvs_flash_deinit();
        nvs_flash_erase();
        return true;
    default:
        return false;
    }
}

}  // namespace maintenance
