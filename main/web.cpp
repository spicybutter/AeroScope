#include "web.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "aircraft.h"
#include "alerts.h"
#include "audio.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "maintenance.h"
#include "net.h"
#include "selftest.h"
#include "settings.h"
#include "sysinfo.h"
#include "timekeeping.h"
#include "web_pages.h"

// main/web/app.html, embedded by CMake (EMBED_TXTFILES -> NUL-terminated)
extern const char app_html_start[] asm("_binary_app_html_start");

namespace web {

static const char* TAG = "WEB";
static constexpr size_t MAX_BODY = 8192;
static constexpr int MAX_SOCKETS = 10;

// ---------------------------------------------------------------------------
// Helpers

static std::string UrlDecode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            out += static_cast<char>(strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

static std::map<std::string, std::string> ParseForm(const std::string& body)
{
    std::map<std::string, std::string> form;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos) end = body.size();
        const std::string pair = body.substr(start, end - start);
        const size_t eq = pair.find('=');
        if (!pair.empty()) {
            form[UrlDecode(pair.substr(0, eq))] = eq == std::string::npos ? "" : UrlDecode(pair.substr(eq + 1));
        }
        start = end + 1;
    }
    return form;
}

static bool ReadBody(httpd_req_t* req, std::string& body)
{
    if (req->content_len > MAX_BODY) return false;
    body.resize(req->content_len);
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body.data() + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static cJSON* ReadJson(httpd_req_t* req)
{
    std::string body;
    return ReadBody(req, body) ? cJSON_Parse(body.c_str()) : nullptr;
}

static std::string HtmlEscape(const std::string& in)
{
    std::string out;
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\'': out += "&#39;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
        s.replace(pos, from.size(), to);
}

// Restart shortly after the response has gone out.
static void RestartSoon()
{
    static esp_timer_handle_t timer;
    if (!timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.name = "restart";
        esp_timer_create(&args, &timer);
    }
    esp_timer_start_once(timer, 1000 * 1000);
}

static esp_err_t SendJson(httpd_req_t* req, cJSON* root)
{
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t SendResult(httpd_req_t* req, bool ok, const std::string& error = "")
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (!ok) cJSON_AddStringToObject(o, "error", error.c_str());
    return SendJson(req, o);
}

static std::string JsonString(const cJSON* obj, const char* key)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static httpd_handle_t StartServer()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;  // captive-portal probes / idle tabs open many sockets
    cfg.max_open_sockets = MAX_SOCKETS;
    cfg.max_uri_handlers = 24;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return nullptr;
    }
    return server;
}

static void Register(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*),
                     bool websocket = false)
{
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.is_websocket = websocket;
    httpd_register_uri_handler(server, &u);
}

// ---------------------------------------------------------------------------
// JSON builders shared by the REST API and the WebSocket push

static AircraftManager* s_aircraft = nullptr;
static httpd_handle_t s_server = nullptr;

static cJSON* AircraftJson(const char* type)
{
    const auto s = settings::Current();
    cJSON* o = cJSON_CreateObject();
    if (type) cJSON_AddStringToObject(o, "t", type);
    cJSON* centre = cJSON_AddArrayToObject(o, "centre");
    cJSON_AddItemToArray(centre, cJSON_CreateNumber(s->lat));
    cJSON_AddItemToArray(centre, cJSON_CreateNumber(s->lon));
    cJSON_AddNumberToObject(o, "radius", s->radius);
    cJSON_AddBoolToObject(o, "location_set", s->locationSet);
    cJSON* list = cJSON_AddArrayToObject(o, "list");
    if (s_aircraft && s->locationSet) {
        for (const auto& a : s_aircraft->Snapshot(s->lat, s->lon)) {
            cJSON* j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "i", a.icao24.c_str());
            cJSON_AddStringToObject(j, "c", a.callsign.c_str());
            cJSON_AddStringToObject(j, "co", a.country.c_str());
            cJSON_AddStringToObject(j, "sq", a.squawk.c_str());
            cJSON_AddNumberToObject(j, "la", a.lat);
            cJSON_AddNumberToObject(j, "lo", a.lon);
            cJSON_AddNumberToObject(j, "a", a.altitude);
            cJSON_AddNumberToObject(j, "v", a.velocity);
            cJSON_AddNumberToObject(j, "h", a.track);
            cJSON_AddNumberToObject(j, "vr", a.verticalRate);
            cJSON_AddNumberToObject(j, "cat", a.category);
            cJSON_AddBoolToObject(j, "g", a.onGround);
            cJSON_AddNumberToObject(j, "d", a.distanceKm);
            cJSON_AddNumberToObject(j, "b", a.bearingDeg);
            cJSON_AddNumberToObject(j, "age", a.ageMs);
            cJSON_AddItemToArray(list, j);
        }
    }
    return o;
}

static int WsClientCount();

static cJSON* StatusJson()
{
    cJSON* o = cJSON_CreateObject();
    const uint32_t now = Millis();
    cJSON_AddStringToObject(o, "firmware", sysinfo::FirmwareVersion().c_str());
    cJSON_AddStringToObject(o, "idf", sysinfo::IdfVersion().c_str());
    cJSON_AddNumberToObject(o, "uptime_s", esp_timer_get_time() / 1e6);
    cJSON_AddStringToObject(o, "reset_reason", sysinfo::ResetReason().c_str());
    cJSON_AddBoolToObject(o, "crashed", sysinfo::LastBootWasCrash());
    cJSON_AddStringToObject(o, "crash", sysinfo::CrashSummary().c_str());
    cJSON_AddNumberToObject(o, "ws_clients", WsClientCount());

    cJSON* heap = cJSON_AddObjectToObject(o, "heap");
    cJSON_AddNumberToObject(heap, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    cJSON* wifi = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", net::Connected());
    cJSON_AddStringToObject(wifi, "ssid", net::Ssid().c_str());
    cJSON_AddStringToObject(wifi, "ip", net::Ip().c_str());
    cJSON_AddNumberToObject(wifi, "rssi", net::Rssi());

    cJSON* time = cJSON_AddObjectToObject(o, "time");
    cJSON_AddBoolToObject(time, "synced", timekeeping::Synced());
    cJSON_AddStringToObject(time, "local", timekeeping::LocalTimestamp().c_str());

    if (s_aircraft) {
        const FetchStatus st = s_aircraft->Status();
        cJSON* d = cJSON_AddObjectToObject(o, "data");
        cJSON_AddBoolToObject(d, "authenticated", st.authenticated);
        cJSON_AddBoolToObject(d, "have_data", st.haveData);
        cJSON_AddNumberToObject(d, "last_update_age_s", st.haveData ? (now - st.lastSuccessMs) / 1000 : -1);
        cJSON_AddStringToObject(d, "last_error", st.lastError.c_str());
        cJSON_AddNumberToObject(d, "http_status", st.httpStatus);
        cJSON_AddNumberToObject(d, "credits", st.creditsRemaining);
        cJSON_AddNumberToObject(d, "latency_ms", st.latencyMs);
        cJSON_AddNumberToObject(d, "interval_s", st.intervalMs / 1000.0);
        const int32_t nextIn = static_cast<int32_t>(st.nextFetchMs - now);
        cJSON_AddNumberToObject(d, "next_in_s", nextIn > 0 ? nextIn / 1000 : 0);
        cJSON_AddNumberToObject(d, "total", st.total);
        cJSON_AddNumberToObject(d, "airborne", st.airborne);
    }
    return o;
}

// ---------------------------------------------------------------------------
// REST handlers (station mode)

static esp_err_t AppGet(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, app_html_start, HTTPD_RESP_USE_STRLEN);
}

static const char* TypeName(settings::Type t)
{
    switch (t) {
    case settings::Type::Bool: return "bool";
    case settings::Type::Int: return "int";
    case settings::Type::Float: return "float";
    case settings::Type::Enum: return "enum";
    case settings::Type::Text: return "text";
    case settings::Type::Ip: return "ip";
    default: return "secret";
    }
}

// GET /api/settings -> {"items":[{key,group,label,type,value,min,max,options,restart,help}]}
static esp_err_t SettingsGet(httpd_req_t* req)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < settings::Count(); i++) {
        const settings::Def& d = settings::Defs()[i];
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "key", d.key);
        cJSON_AddStringToObject(it, "group", d.group);
        cJSON_AddStringToObject(it, "label", d.label);
        cJSON_AddStringToObject(it, "type", TypeName(d.type));
        std::string v = settings::Value(d.key);
        if (d.type == settings::Type::Secret) std::fill(v.begin(), v.end(), '*');  // never sent
        cJSON_AddStringToObject(it, "value", v.c_str());
        if (d.type == settings::Type::Int || d.type == settings::Type::Float) {
            cJSON_AddNumberToObject(it, "min", d.min);
            cJSON_AddNumberToObject(it, "max", d.max);
        }
        if (d.type == settings::Type::Enum) {
            cJSON* opts = cJSON_AddArrayToObject(it, "options");
            for (int o = 0; o < d.optionCount; o++) {
                cJSON* opt = cJSON_CreateObject();
                cJSON_AddStringToObject(opt, "value", d.options[o].value);
                cJSON_AddStringToObject(opt, "label", d.options[o].label);
                cJSON_AddItemToArray(opts, opt);
            }
        }
        cJSON_AddBoolToObject(it, "restart", d.restart);
        cJSON_AddStringToObject(it, "help", d.help);
        cJSON_AddItemToArray(items, it);
    }
    return SendJson(req, root);
}

static settings::Raw JsonToRaw(const cJSON* obj, bool knownOnly)
{
    settings::Raw changes;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, obj)
    {
        if (knownOnly && !settings::Find(item->string)) continue;
        if (cJSON_IsString(item)) {
            changes[item->string] = item->valuestring;
        } else if (cJSON_IsBool(item)) {
            changes[item->string] = cJSON_IsTrue(item) ? "true" : "false";
        } else if (cJSON_IsNumber(item)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.10g", item->valuedouble);
            changes[item->string] = buf;
        }
    }
    return changes;
}

// POST /api/settings {"key":"value",...} -> {"ok":bool,"restart":bool,"error":"..."}
static esp_err_t SettingsPost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    settings::UpdateResult r;
    if (!cJSON_IsObject(in)) {
        r.ok = false;
        r.error = "expected a JSON object";
    } else {
        r = settings::Update(JsonToRaw(in, false));
    }
    cJSON_Delete(in);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", r.ok);
    cJSON_AddBoolToObject(out, "restart", r.ok && r.restartNeeded);
    if (!r.ok) cJSON_AddStringToObject(out, "error", r.error.c_str());
    const esp_err_t err = SendJson(req, out);
    if (r.ok && r.restartNeeded) {
        ESP_LOGI(TAG, "settings saved, restarting");
        RestartSoon();
    }
    return err;
}

static esp_err_t StatusGet(httpd_req_t* req) { return SendJson(req, StatusJson()); }
static esp_err_t AircraftGet(httpd_req_t* req) { return SendJson(req, AircraftJson(nullptr)); }

static esp_err_t LogsGet(httpd_req_t* req)
{
    cJSON* o = cJSON_CreateObject();
    cJSON* lines = cJSON_AddArrayToObject(o, "lines");
    for (const auto& l : logbuf::History()) cJSON_AddItemToArray(lines, cJSON_CreateString(l.c_str()));
    return SendJson(req, o);
}

// Wi-Fi (#61, #74)
static esp_err_t WifiGet(httpd_req_t* req)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", net::Ssid().c_str());
    cJSON_AddStringToObject(o, "ip", net::Ip().c_str());
    cJSON_AddNumberToObject(o, "rssi", net::Rssi());
    cJSON_AddBoolToObject(o, "static", settings::Current()->staticIp);
    cJSON* saved = cJSON_AddArrayToObject(o, "saved");
    for (const auto& n : net::SavedNetworks()) cJSON_AddItemToArray(saved, cJSON_CreateString(n.ssid.c_str()));
    return SendJson(req, o);
}

static esp_err_t WifiScanGet(httpd_req_t* req)
{
    cJSON* arr = cJSON_CreateArray();
    for (const auto& r : net::Scan()) {
        cJSON* n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", r.ssid.c_str());
        cJSON_AddNumberToObject(n, "rssi", r.rssi);
        cJSON_AddBoolToObject(n, "secure", r.secure);
        cJSON_AddItemToArray(arr, n);
    }
    return SendJson(req, arr);
}

static esp_err_t WifiAddPost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    const std::string ssid = JsonString(in, "ssid"), pass = JsonString(in, "password");
    cJSON_Delete(in);
    if (ssid.empty()) return SendResult(req, false, "SSID is required");
    if (!pass.empty() && pass.size() < 8) return SendResult(req, false, "WPA passwords are at least 8 characters");
    return SendResult(req, net::SaveCredentials(ssid, pass), "could not save");
}

static esp_err_t WifiForgetPost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    const std::string ssid = JsonString(in, "ssid");
    cJSON_Delete(in);
    return SendResult(req, net::ForgetNetwork(ssid), "not a saved network");
}

// Maintenance (#64)
static esp_err_t RebootPost(httpd_req_t* req)
{
    ESP_LOGI(TAG, "reboot requested from the web page");
    const esp_err_t err = SendResult(req, true);
    RestartSoon();
    return err;
}

static esp_err_t ResetPost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    const std::string scope = JsonString(in, "scope");
    cJSON_Delete(in);
    maintenance::ResetScope s = scope == "settings" ? maintenance::ResetScope::Settings
                              : scope == "wifi"     ? maintenance::ResetScope::Wifi
                              : scope == "all"      ? maintenance::ResetScope::All
                                                    : maintenance::ResetScope::None;
    if (s == maintenance::ResetScope::None) return SendResult(req, false, "scope must be settings, wifi or all");
    ESP_LOGW(TAG, "reset (%s) requested from the web page", scope.c_str());
    const esp_err_t err = SendResult(req, true);
    maintenance::RequestReset(s);
    return err;
}

// Backup / restore (#65)
static esp_err_t BackupGet(httpd_req_t* req)
{
    char query[32] = {};
    char secrets[4] = {};
    const bool withSecrets = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                             httpd_query_key_value(query, "secrets", secrets, sizeof(secrets)) == ESP_OK &&
                             secrets[0] == '1';
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "aeroscope-backup");
    cJSON_AddNumberToObject(o, "version", 1);
    cJSON_AddStringToObject(o, "firmware", sysinfo::FirmwareVersion().c_str());
    cJSON_AddBoolToObject(o, "includes_secrets", withSecrets);
    cJSON* s = cJSON_AddObjectToObject(o, "settings");
    for (int i = 0; i < settings::Count(); i++) {
        const settings::Def& d = settings::Defs()[i];
        if (d.type == settings::Type::Secret && !withSecrets) continue;
        cJSON_AddStringToObject(s, d.key, settings::Value(d.key).c_str());
    }
    if (withSecrets) {
        cJSON* w = cJSON_AddArrayToObject(o, "wifi");
        for (const auto& n : net::SavedNetworks()) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "ssid", n.ssid.c_str());
            cJSON_AddStringToObject(e, "password", n.password.c_str());
            cJSON_AddItemToArray(w, e);
        }
    }
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"aeroscope-backup.json\"");
    return SendJson(req, o);
}

static esp_err_t RestorePost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    // Backups made before the rename ("micro-radar-backup") are still accepted.
    const std::string type = JsonString(in, "type");
    if (!cJSON_IsObject(in) || (type != "aeroscope-backup" && type != "micro-radar-backup")) {
        cJSON_Delete(in);
        return SendResult(req, false, "not an AeroScope backup file");
    }
    const cJSON* s = cJSON_GetObjectItemCaseSensitive(in, "settings");
    settings::UpdateResult r;
    if (cJSON_IsObject(s)) r = settings::Update(JsonToRaw(s, true));  // unknown keys (other versions) skipped
    if (r.ok) {
        const cJSON* w = cJSON_GetObjectItemCaseSensitive(in, "wifi");
        if (cJSON_IsArray(w) && cJSON_GetArraySize(w) > 0) {
            std::vector<net::SavedNetwork> list;
            const cJSON* e = nullptr;
            cJSON_ArrayForEach(e, w)
            {
                const std::string ssid = JsonString(e, "ssid");
                if (!ssid.empty()) list.push_back({ssid, JsonString(e, "password")});
            }
            if (!list.empty()) net::SaveNetworks(list);
        }
    }
    cJSON_Delete(in);
    if (!r.ok) return SendResult(req, false, r.error);
    ESP_LOGI(TAG, "backup restored, restarting");
    const esp_err_t err = SendResult(req, true);
    RestartSoon();
    return err;
}

// ---------------------------------------------------------------------------
// Alerts (#50, #55, #57) and self-test (#94)

static std::mutex s_pending_mutex;
static std::vector<std::string> s_pending;  // JSON messages for the next broadcast tick

static cJSON* AlertJson(const alerts::Alert& a)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", a.id);
    cJSON_AddStringToObject(o, "kind", a.kind == alerts::Kind::Emergency ? "emergency" : "watch");
    cJSON_AddStringToObject(o, "title", a.title.c_str());
    cJSON_AddStringToObject(o, "text", a.text.c_str());
    cJSON_AddStringToObject(o, "icao", a.icao24.c_str());
    cJSON_AddNumberToObject(o, "age_s", (Millis() - a.timeMs) / 1000);
    return o;
}

void PushAlert(const alerts::Alert& a)
{
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "alert");
    cJSON_AddItemToObject(m, "a", AlertJson(a));
    char* text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!text) return;
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    s_pending.emplace_back(text);
    cJSON_free(text);
}

static esp_err_t AlertsGet(httpd_req_t* req)
{
    cJSON* arr = cJSON_CreateArray();
    for (const auto& a : alerts::Active()) cJSON_AddItemToArray(arr, AlertJson(a));
    return SendJson(req, arr);
}

static esp_err_t AlertsAckPost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(in, "id");
    if (cJSON_IsNumber(id)) {
        alerts::Ack(static_cast<uint32_t>(id->valuedouble));
    } else {
        alerts::AckAll();
    }
    cJSON_Delete(in);
    return SendResult(req, true);
}

// POST /api/sound {"sound":"chime"|"watch"|"emergency"} - preview a sound at the set volume (#46)
static esp_err_t SoundPost(httpd_req_t* req)
{
    cJSON* in = ReadJson(req);
    const std::string name = JsonString(in, "sound");
    cJSON_Delete(in);
    if (!audio::Available()) return SendResult(req, false, "no audio codec");
    const audio::Sound snd = name == "watch" ? audio::Sound::Watch
                           : name == "emergency" ? audio::Sound::Emergency
                                                 : audio::Sound::Chime;
    audio::Play(snd, true);
    return SendResult(req, true);
}

static esp_err_t SelfTestPost(httpd_req_t* req)
{
    return SendResult(req, selftest::Start(), "a self-test is already running");
}

static esp_err_t SelfTestGet(httpd_req_t* req)
{
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "running", selftest::Running());
    cJSON* arr = cJSON_AddArrayToObject(o, "results");
    for (const auto& r : selftest::Results()) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", r.name.c_str());
        cJSON_AddStringToObject(e, "status", selftest::StatusName(r.status));
        cJSON_AddStringToObject(e, "detail", r.detail.c_str());
        cJSON_AddItemToArray(arr, e);
    }
    return SendJson(req, o);
}

// ---------------------------------------------------------------------------
// WebSocket push (#58, #63): aircraft 1 Hz, status every 5 s, log lines as they come

static esp_err_t WsHandler(httpd_req_t* req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "web client connected (%d live)", WsClientCount() + 1);
        return ESP_OK;  // handshake done
    }
    // Clients only listen; read and discard anything they send.
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    if (frame.len > 0 && frame.len < 512) {
        std::string buf(frame.len, '\0');
        frame.payload = reinterpret_cast<uint8_t*>(buf.data());
        httpd_ws_recv_frame(req, &frame, frame.len);
    }
    return ESP_OK;
}

static int WsClients(int* fds)
{
    if (!s_server) return 0;
    size_t n = MAX_SOCKETS;
    int all[MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &n, all) != ESP_OK) return 0;
    int count = 0;
    for (size_t i = 0; i < n; i++)
        if (httpd_ws_get_fd_info(s_server, all[i]) == HTTPD_WS_CLIENT_WEBSOCKET) fds[count++] = all[i];
    return count;
}

static int WsClientCount()
{
    int fds[MAX_SOCKETS];
    return WsClients(fds);
}

static void BroadcastText(const std::string& text)
{
    int fds[MAX_SOCKETS];
    const int n = WsClients(fds);
    if (n == 0) return;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(text.data()));
    frame.len = text.size();
    for (int i = 0; i < n; i++) httpd_ws_send_data(s_server, fds[i], &frame);  // failures: client gone, httpd cleans up
}

static void Broadcast(cJSON* msg)
{
    if (WsClientCount() == 0) {
        cJSON_Delete(msg);
        return;
    }
    char* text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text) return;
    BroadcastText(text);
    cJSON_free(text);
}

static void BroadcastTask(void*)
{
    uint32_t tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tick++;

        std::vector<std::string> lines;
        logbuf::Drain(lines, 128);
        if (!lines.empty()) {
            logbuf::AddToHistory(lines);
            cJSON* m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "t", "log");
            cJSON* arr = cJSON_AddArrayToObject(m, "lines");
            for (const auto& l : lines) cJSON_AddItemToArray(arr, cJSON_CreateString(l.c_str()));
            Broadcast(m);
        }
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(s_pending_mutex);
            pending.swap(s_pending);
        }
        for (const auto& text : pending) BroadcastText(text);

        if (WsClientCount() == 0) continue;
        Broadcast(AircraftJson("ac"));
        if (tick % 5 == 0) {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "t", "st");
            cJSON_AddItemToObject(m, "s", StatusJson());
            Broadcast(m);
        }
    }
}

void StartConfigServer(AircraftManager& aircraft)
{
    s_aircraft = &aircraft;
    s_server = StartServer();
    if (!s_server) return;
    Register(s_server, "/", HTTP_GET, AppGet);
    Register(s_server, "/api/settings", HTTP_GET, SettingsGet);
    Register(s_server, "/api/settings", HTTP_POST, SettingsPost);
    Register(s_server, "/api/status", HTTP_GET, StatusGet);
    Register(s_server, "/api/aircraft", HTTP_GET, AircraftGet);
    Register(s_server, "/api/logs", HTTP_GET, LogsGet);
    Register(s_server, "/api/wifi", HTTP_GET, WifiGet);
    Register(s_server, "/api/wifi/scan", HTTP_GET, WifiScanGet);
    Register(s_server, "/api/wifi/add", HTTP_POST, WifiAddPost);
    Register(s_server, "/api/wifi/forget", HTTP_POST, WifiForgetPost);
    Register(s_server, "/api/reboot", HTTP_POST, RebootPost);
    Register(s_server, "/api/reset", HTTP_POST, ResetPost);
    Register(s_server, "/api/backup", HTTP_GET, BackupGet);
    Register(s_server, "/api/restore", HTTP_POST, RestorePost);
    Register(s_server, "/api/alerts", HTTP_GET, AlertsGet);
    Register(s_server, "/api/alerts/ack", HTTP_POST, AlertsAckPost);
    Register(s_server, "/api/selftest", HTTP_GET, SelfTestGet);
    Register(s_server, "/api/selftest", HTTP_POST, SelfTestPost);
    Register(s_server, "/api/sound", HTTP_POST, SoundPost);
    Register(s_server, "/ws", HTTP_GET, WsHandler, true);
    xTaskCreatePinnedToCore(BroadcastTask, "web_push", 8192, nullptr, 3, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Wi-Fi setup portal (setup-hotspot mode)

static esp_err_t PortalGet(httpd_req_t* req)
{
    std::string options;
    for (const auto& ssid : net::ScanSsids()) {
        const std::string e = HtmlEscape(ssid);
        options += "<option value=\"" + e + "\">" + e + "</option>";
    }
    std::string page = PORTAL_HTML;
    ReplaceAll(page, "%SSID_OPTIONS%", options);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), page.size());
}

static esp_err_t PortalSave(httpd_req_t* req)
{
    std::string body;
    if (!ReadBody(req, body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    }
    auto form = ParseForm(body);
    const std::string ssid = form["s"];
    if (ssid.empty() || !net::SaveCredentials(ssid, form["p"])) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing or could not be saved");
    }
    ESP_LOGI(TAG, "Saved WiFi \"%s\", restarting", ssid.c_str());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style=\"background:#111;color:#00ff00;font-family:monospace;padding:16px\">"
        "Saved. AeroScope is restarting and will join the network.</body></html>");
    RestartSoon();
    return ESP_OK;
}

// Any unknown URL (OS connectivity checks etc.) is redirected to the portal.
static esp_err_t PortalRedirect(httpd_req_t* req, httpd_err_code_t)
{
    const std::string location = "http://" + net::ApIp() + "/";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    return httpd_resp_send(req, nullptr, 0);
}

void StartPortalServer()
{
    httpd_handle_t server = StartServer();
    if (!server) return;
    Register(server, "/", HTTP_GET, PortalGet);
    Register(server, "/wifisave", HTTP_POST, PortalSave);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, PortalRedirect);
}

}  // namespace web
