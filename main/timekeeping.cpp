#include "timekeeping.h"

#include <cstdlib>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

namespace timekeeping {

static const char* TAG = "TIME";
static volatile bool s_synced = false;
static std::string s_server;  // must outlive the SNTP client

static void OnSync(struct timeval*)
{
    const bool first = !s_synced;
    s_synced = true;
    if (first) ESP_LOGI(TAG, "time synced: %s", LocalTimestamp().c_str());
}

void Start(const std::string& ntpServer, const std::string& posixTz)
{
    setenv("TZ", posixTz.c_str(), 1);
    tzset();
    s_server = ntpServer;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server.c_str());
    cfg.sync_cb = OnSync;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        return;
    }
    ESP_LOGI(TAG, "syncing with %s, TZ=%s", s_server.c_str(), posixTz.c_str());
}

bool Synced() { return s_synced; }

std::string ClockText(bool h24)
{
    if (!s_synced) return "";
    const time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char buf[16];
    if (h24) {
        strftime(buf, sizeof(buf), "%H:%M", &local);
    } else {
        strftime(buf, sizeof(buf), "%I:%M%p", &local);
        if (buf[0] == '0') return buf + 1;  // "2:05PM"
    }
    return buf;
}

int64_t SecondsToUtcMidnight()
{
    if (!s_synced) return -1;
    const time_t now = time(nullptr);
    return 86400 - (now % 86400);
}

std::string LocalTimestamp()
{
    if (!s_synced) return "not synced";
    const time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

}  // namespace timekeeping
