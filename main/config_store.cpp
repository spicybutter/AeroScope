#include "config_store.h"

#include "esp_log.h"
#include "nvs.h"

namespace config {

static const char* TAG = "config";

std::string Get(const char* key, const char* fallback)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return fallback;
    size_t len = 0;
    std::string out = fallback;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0) {
        out.resize(len);
        if (nvs_get_str(h, key, out.data(), &len) == ESP_OK) {
            out.resize(len - 1);  // drop terminator
        } else {
            out = fallback;
        }
    }
    nvs_close(h);
    return out;
}

bool Set(const char* key, const std::string& value)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, value.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save %s failed: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}

}  // namespace config
