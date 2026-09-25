#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"

namespace net {

static const char* TAG = "NET";

static constexpr EventBits_t BIT_CONNECTED = BIT0;
static constexpr EventBits_t BIT_FAILED = BIT1;
static constexpr int ATTEMPTS_PER_NETWORK = 2;
static constexpr int NETWORK_TIMEOUT_MS = 15000;
static constexpr uint64_t RECONNECT_DELAY_US = 5 * 1000 * 1000;
static constexpr int RESELECT_AFTER_FAILURES = 6;   // then rescan and pick the best saved network

static const char* NVS_NS = "wifi";
static const char* NVS_KEY = "list";

static EventGroupHandle_t s_events;
static esp_netif_t* s_sta;
static esp_netif_t* s_ap;
static esp_timer_handle_t s_reconnect_timer;
static TaskHandle_t s_reselect_task;
static volatile bool s_selecting = true;  // a connect sequence is in progress (Start or reselect)
static volatile bool s_portal = false;    // setup hotspot active: stop reconnecting
static int s_attempts = 0;
static int s_runtime_failures = 0;
static volatile bool s_connected = false;

// ---------------------------------------------------------------------------
// Saved networks (NVS "wifi"/"list" = JSON [{"s":..,"p":..}])

std::vector<SavedNetwork> SavedNetworks()
{
    std::vector<SavedNetwork> out;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return out;
    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY, nullptr, &len) == ESP_OK && len > 1) {
        std::string json(len, '\0');
        if (nvs_get_str(h, NVS_KEY, json.data(), &len) == ESP_OK) {
            cJSON* arr = cJSON_Parse(json.c_str());
            const cJSON* it = nullptr;
            cJSON_ArrayForEach(it, arr)
            {
                const cJSON* s = cJSON_GetObjectItem(it, "s");
                const cJSON* p = cJSON_GetObjectItem(it, "p");
                if (cJSON_IsString(s) && s->valuestring[0])
                    out.push_back({s->valuestring, cJSON_IsString(p) ? p->valuestring : ""});
            }
            cJSON_Delete(arr);
        }
    }
    nvs_close(h);
    return out;
}

bool SaveNetworks(const std::vector<SavedNetwork>& list)
{
    cJSON* arr = cJSON_CreateArray();
    for (size_t i = 0; i < list.size() && i < MAX_SAVED; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "s", list[i].ssid.c_str());
        cJSON_AddStringToObject(o, "p", list[i].password.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    char* text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY, text ? text : "[]");
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    cJSON_free(text);
    if (err != ESP_OK) ESP_LOGE(TAG, "saving network list failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool SaveCredentials(const std::string& ssid, const std::string& password)
{
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) return false;
    auto list = SavedNetworks();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedNetwork& n) { return n.ssid == ssid; }),
               list.end());
    list.insert(list.begin(), {ssid, password});
    if (list.size() > MAX_SAVED) list.resize(MAX_SAVED);
    ESP_LOGI(TAG, "saved network \"%s\" (%u remembered)", ssid.c_str(), static_cast<unsigned>(list.size()));
    return SaveNetworks(list);
}

bool ForgetNetwork(const std::string& ssid)
{
    auto list = SavedNetworks();
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedNetwork& n) { return n.ssid == ssid; }),
               list.end());
    if (list.size() == before) return false;
    ESP_LOGI(TAG, "forgot network \"%s\"", ssid.c_str());
    return SaveNetworks(list);
}

// The Arduino build and the first ESP-IDF build kept one network in the Wi-Fi
// driver's own storage; adopt it into the list once.
static void MigrateDriverCredentials()
{
    wifi_config_t stored = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &stored) != ESP_OK || stored.sta.ssid[0] == '\0') return;
    const std::string ssid = reinterpret_cast<const char*>(stored.sta.ssid);
    auto list = SavedNetworks();
    if (std::any_of(list.begin(), list.end(), [&](const SavedNetwork& n) { return n.ssid == ssid; })) return;
    list.push_back({ssid, reinterpret_cast<const char*>(stored.sta.password)});
    ESP_LOGI(TAG, "adopted previously saved network \"%s\"", ssid.c_str());
    SaveNetworks(list);
}

// ---------------------------------------------------------------------------
// Static IP (#75)

static void ApplyIpConfig()
{
    const auto s = settings::Current();
    if (!s->staticIp) {
        esp_netif_dhcpc_start(s_sta);  // harmless if already running
        return;
    }
    esp_netif_ip_info_t info = {};
    if (s->ip.empty() || s->gateway.empty() || esp_netif_str_to_ip4(s->ip.c_str(), &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(s->netmask.c_str(), &info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(s->gateway.c_str(), &info.gw) != ESP_OK) {
        ESP_LOGW(TAG, "static IP enabled but incomplete/invalid - using DHCP");
        esp_netif_dhcpc_start(s_sta);
        return;
    }
    esp_netif_dhcpc_stop(s_sta);
    esp_netif_set_ip_info(s_sta, &info);
    auto setDns = [](const std::string& addr, esp_netif_dns_type_t type) {
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(addr.c_str(), &dns.ip.u_addr.ip4) == ESP_OK) esp_netif_set_dns_info(s_sta, type, &dns);
    };
    setDns(s->dns1.empty() ? s->gateway : s->dns1, ESP_NETIF_DNS_MAIN);
    if (!s->dns2.empty()) setDns(s->dns2, ESP_NETIF_DNS_BACKUP);
    ESP_LOGI(TAG, "static IP %s / %s gw %s", s->ip.c_str(), s->netmask.c_str(), s->gateway.c_str());
}

// ---------------------------------------------------------------------------
// Events

static void Reconnect(void*)
{
    if (!s_portal && !s_selecting) esp_wifi_connect();
}

static void OnEvent(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        const bool was_connected = s_connected;
        s_connected = false;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (s_portal) return;
        if (s_selecting) {
            ESP_LOGW(TAG, "connect attempt %d failed (reason %d)", s_attempts + 1, ev->reason);
            if (++s_attempts >= ATTEMPTS_PER_NETWORK) {
                xEventGroupSetBits(s_events, BIT_FAILED);
            } else {
                esp_wifi_connect();
            }
            return;
        }
        if (was_connected) ESP_LOGW(TAG, "WiFi lost (reason %d), reconnecting...", ev->reason);
        if (++s_runtime_failures % RESELECT_AFTER_FAILURES == 0 && s_reselect_task) {
            xTaskNotifyGive(s_reselect_task);  // try the other saved networks
        } else {
            esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_attempts = 0;
        s_runtime_failures = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        if (!s_selecting) PrintInfo();
    }
}

// ---------------------------------------------------------------------------
// Network selection

std::vector<ScanResult> Scan()
{
    std::vector<ScanResult> out;
    wifi_scan_config_t cfg = {};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return out;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return out;
    std::vector<wifi_ap_record_t> recs(n);
    esp_wifi_scan_get_ap_records(&n, recs.data());
    std::sort(recs.begin(), recs.begin() + n,
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid = reinterpret_cast<const char*>(recs[i].ssid);
        if (ssid.empty()) continue;
        if (std::any_of(out.begin(), out.end(), [&](const ScanResult& r) { return r.ssid == ssid; })) continue;
        out.push_back({ssid, recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN});
    }
    return out;
}

std::vector<std::string> ScanSsids()
{
    std::vector<std::string> out;
    for (const auto& r : Scan()) out.push_back(r.ssid);
    return out;
}

static bool TryNetwork(const SavedNetwork& n)
{
    ESP_LOGI(TAG, "Connecting to \"%s\"", n.ssid.c_str());
    wifi_config_t sta = {};
    strncpy(reinterpret_cast<char*>(sta.sta.ssid), n.ssid.c_str(), sizeof(sta.sta.ssid));
    strncpy(reinterpret_cast<char*>(sta.sta.password), n.password.c_str(), sizeof(sta.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    ApplyIpConfig();
    s_attempts = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    esp_wifi_connect();
    const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(NETWORK_TIMEOUT_MS));
    if (bits & BIT_CONNECTED) return true;
    esp_wifi_disconnect();
    return false;
}

// Saved networks that are visible, strongest first; then the unseen ones (they
// may be hidden SSIDs) in saved order.
static std::vector<SavedNetwork> Candidates(const std::vector<SavedNetwork>& saved)
{
    const auto seen = Scan();
    std::vector<SavedNetwork> out;
    for (const auto& r : seen)
        for (const auto& n : saved)
            if (n.ssid == r.ssid) out.push_back(n);
    for (const auto& n : saved)
        if (std::none_of(out.begin(), out.end(), [&](const SavedNetwork& c) { return c.ssid == n.ssid; }))
            out.push_back(n);
    return out;
}

static bool ConnectToBest()
{
    s_selecting = true;
    bool ok = false;
    for (const auto& n : Candidates(SavedNetworks())) {
        if (TryNetwork(n)) {
            ok = true;
            break;
        }
    }
    s_selecting = false;
    return ok;
}

static void ReselectTask(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_portal || s_connected) continue;
        ESP_LOGW(TAG, "still offline, trying all saved networks");
        if (!ConnectToBest()) esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
    }
}

static void StartPortal()
{
    s_portal = true;
    esp_wifi_disconnect();
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {};
    strncpy(reinterpret_cast<char*>(ap.ap.ssid), AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;   // same as WiFiManager (no AP password)
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);  // STA side kept for scanning
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "Setup hotspot \"%s\" active, portal at http://%s", AP_SSID, ApIp().c_str());
}

Mode Start()
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, OnEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, OnEvent, nullptr));

    esp_timer_create_args_t targs = {};
    targs.callback = Reconnect;
    targs.name = "wifi_reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    MigrateDriverCredentials();
    ESP_ERROR_CHECK(esp_wifi_start());

    const auto saved = SavedNetworks();
    if (saved.empty()) {
        ESP_LOGI(TAG, "No WiFi saved");
    } else if (ConnectToBest()) {
        xTaskCreate(ReselectTask, "wifi_reselect", 4096, nullptr, 3, &s_reselect_task);
        return Mode::Station;
    } else {
        ESP_LOGW(TAG, "Could not connect to any of %u saved network(s)", static_cast<unsigned>(saved.size()));
    }
    s_selecting = false;
    StartPortal();
    return Mode::SetupPortal;
}

bool Connected() { return s_connected; }
bool PortalActive() { return s_portal; }

static std::string IpOf(esp_netif_t* netif)
{
    esp_netif_ip_info_t info = {};
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return "0.0.0.0";
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return buf;
}

std::string Ip() { return IpOf(s_sta); }
std::string ApIp() { return IpOf(s_ap); }

std::string Ssid()
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return "";
    return reinterpret_cast<const char*>(ap.ssid);
}

int Rssi()
{
    wifi_ap_record_t ap = {};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void PrintInfo()
{
    if (Connected()) {
        ESP_LOGI(TAG, "Connected to \"%s\"  IP: %s  RSSI: %d dBm", Ssid().c_str(), Ip().c_str(), Rssi());
        ESP_LOGI(TAG, "Configure at http://%s or http://%s.local", Ip().c_str(), HOSTNAME);
    } else if (s_portal) {
        ESP_LOGI(TAG, "Setup hotspot \"%s\" active, portal at http://%s", AP_SSID, ApIp().c_str());
    } else {
        ESP_LOGW(TAG, "WiFi not connected");
    }
}

}  // namespace net
