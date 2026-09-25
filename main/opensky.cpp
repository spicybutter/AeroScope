#include "opensky.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char* TAG = "OPENSKY";

static constexpr const char* STATES_URL = "https://opensky-network.org/api/states/all";
static constexpr const char* TOKEN_URL =
    "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
static constexpr size_t MAX_RESPONSE = 1024 * 1024;  // PSRAM-backed; generous for a 2.5 deg box

// ---------------------------------------------------------------------------
// Minimal HTTP helper (HttpRequestManager equivalent)

struct HttpResult {
    bool transportOk = false;
    int statusCode = 0;
    int creditsRemaining = -1;
    int retryAfterS = -1;
    uint32_t latencyMs = 0;
    std::string response;
    std::string errorMessage;
};

static esp_err_t OnHttpEvent(esp_http_client_event_t* evt)
{
    auto* r = static_cast<HttpResult*>(evt->user_data);
    if (!r) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (r->response.size() + evt->data_len <= MAX_RESPONSE)
            r->response.append(static_cast<const char*>(evt->data), evt->data_len);
    } else if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "X-Rate-Limit-Remaining") == 0)
            r->creditsRemaining = atoi(evt->header_value);
        else if (strcasecmp(evt->header_key, "X-Rate-Limit-Retry-After-Seconds") == 0)
            r->retryAfterS = atoi(evt->header_value);
    }
    return ESP_OK;
}

static HttpResult Request(const std::string& url, esp_http_client_method_t method, const std::string& body,
                          const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResult result;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.event_handler = OnHttpEvent;
    cfg.user_data = &result;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;  // Authorization: Bearer <JWT> is long

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        result.errorMessage = "client init failed";
        return result;
    }
    for (const auto& [k, v] : headers) esp_http_client_set_header(client, k.c_str(), v.c_str());
    if (!body.empty()) esp_http_client_set_post_field(client, body.c_str(), body.size());

    const int64_t start = esp_timer_get_time();
    const esp_err_t err = esp_http_client_perform(client);
    result.latencyMs = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000);
    result.statusCode = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && result.statusCode > 0) {
        result.transportOk = true;
    } else {
        result.errorMessage = esp_err_to_name(err);
        ESP_LOGW(TAG, "%s transport error (%s)", method == HTTP_METHOD_POST ? "POST" : "GET", esp_err_to_name(err));
    }
    return result;
}

// ---------------------------------------------------------------------------

static void* PsramMalloc(size_t size)
{
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

OpenSkyClient::OpenSkyClient()
{
    // A 2 deg box can hold hundreds of state vectors; keep cJSON's many small
    // nodes out of internal RAM.
    cJSON_Hooks hooks = {PsramMalloc, free};
    cJSON_InitHooks(&hooks);
}

std::string OpenSkyClient::FetchBearerToken(const std::string& clientId, const std::string& clientSecret)
{
    const std::string body =
        "grant_type=client_credentials&client_id=" + clientId + "&client_secret=" + clientSecret;
    const HttpResult resp = Request(TOKEN_URL, HTTP_METHOD_POST, body,
                                    {{"Content-Type", "application/x-www-form-urlencoded"}});
    if (!resp.transportOk || resp.statusCode != 200) {
        ESP_LOGE(TAG, "token request failed: %s (HTTP %d)", resp.errorMessage.c_str(), resp.statusCode);
        return "";
    }
    cJSON* doc = cJSON_Parse(resp.response.c_str());
    if (!doc) {
        ESP_LOGE(TAG, "token response JSON parse failed");
        return "";
    }
    std::string token;
    const cJSON* t = cJSON_GetObjectItemCaseSensitive(doc, "access_token");
    if (cJSON_IsString(t) && t->valuestring) {
        token = t->valuestring;
    } else {
        ESP_LOGW(TAG, "missing or non-string 'access_token' in token response");
    }
    cJSON_Delete(doc);
    return token;
}

std::string OpenSkyClient::GetValidToken(const std::string& clientId, const std::string& clientSecret)
{
    if (clientId.empty() || clientSecret.empty()) return "";
    if (bearerToken_.empty() || Millis() > tokenExpiry_) {
        bearerToken_ = FetchBearerToken(clientId, clientSecret);
        tokenExpiry_ = Millis() + 29u * 60u * 1000u;  // 29 min, 1 min buffer
    }
    return bearerToken_;
}

// Null-tolerant field readers (the Arduino parser used isNull() checks)
static std::string Str(const cJSON* a, int i)
{
    const cJSON* v = cJSON_GetArrayItem(a, i);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
static double Num(const cJSON* a, int i)
{
    const cJSON* v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}
static bool Bool(const cJSON* a, int i) { return cJSON_IsTrue(cJSON_GetArrayItem(a, i)); }

static std::string Trim(std::string s)
{
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

FetchResult OpenSkyClient::FetchStates(double lat, double lon, double radius, const std::string& token)
{
    FetchResult out;
    char url[256];
    snprintf(url, sizeof(url), "%s?lamin=%.4f&lamax=%.4f&lomin=%.4f&lomax=%.4f&extended=1", STATES_URL,
             lat - radius, lat + radius, lon - radius, lon + radius);

    std::vector<std::pair<std::string, std::string>> headers;
    if (!token.empty()) headers.push_back({"Authorization", "Bearer " + token});

    const HttpResult r = Request(url, HTTP_METHOD_GET, "", headers);
    out.httpStatus = r.statusCode;
    out.creditsRemaining = r.creditsRemaining;
    out.retryAfterS = r.retryAfterS;
    out.latencyMs = r.latencyMs;
    if (!r.transportOk) {
        out.error = r.errorMessage;
        return out;
    }
    // An error status (e.g. 429) is not "no aircraft": skip the update.
    if (r.statusCode < 200 || r.statusCode >= 300) {
        char buf[32];
        snprintf(buf, sizeof(buf), r.statusCode == 429 ? "rate limited (429)" : "HTTP %d", r.statusCode);
        out.error = buf;
        return out;
    }

    cJSON* doc = cJSON_Parse(r.response.c_str());
    if (!doc) {
        out.error = "invalid JSON";
        return out;
    }
    // "states" is null when nothing is in the box - a valid empty result.
    const cJSON* states = cJSON_GetObjectItemCaseSensitive(doc, "states");
    const cJSON* s = nullptr;
    cJSON_ArrayForEach(s, states)
    {
        Aircraft a;
        a.icao24 = Str(s, 0);
        a.callsign = Trim(Str(s, 1));
        a.originCountry = Str(s, 2);
        a.timePosition = static_cast<long>(Num(s, 3));
        a.lastContact = static_cast<long>(Num(s, 4));
        a.longitude = static_cast<float>(Num(s, 5));
        a.latitude = static_cast<float>(Num(s, 6));
        a.baroAltitude = static_cast<float>(Num(s, 7));
        a.onGround = Bool(s, 8);
        a.velocity = static_cast<float>(Num(s, 9));
        a.trueTrack = static_cast<float>(Num(s, 10));
        a.verticalRate = static_cast<float>(Num(s, 11));
        // [12] sensors - skipped
        a.geoAltitude = static_cast<float>(Num(s, 13));
        a.squawk = Str(s, 14);
        a.spi = Bool(s, 15);
        a.positionSource = static_cast<int>(Num(s, 16));
        a.category = static_cast<int>(Num(s, 17));
        out.aircraft.push_back(std::move(a));
    }
    cJSON_Delete(doc);
    out.ok = true;
    return out;
}
