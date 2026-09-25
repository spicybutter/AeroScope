// Raw NVS string storage, exactly like the Arduino build's Preferences:
// NVS namespace "config", every value a string. Settings saved by either
// build are therefore readable by the other. Typed access: settings.h.
#pragma once

#include <string>

namespace config {

constexpr const char* NAMESPACE = "config";

std::string Get(const char* key, const char* fallback = "");
bool Set(const char* key, const std::string& value);

}  // namespace config
