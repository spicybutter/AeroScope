// Boot diagnostics: reset reason, last crash from the flash core dump (#95),
// firmware identity, and log-level control (#97).
#pragma once

#include <string>

namespace sysinfo {

// Call once early in app_main.
void Init();

std::string ResetReason();     // e.g. "panic", "power-on"
bool LastBootWasCrash();
std::string CrashSummary();    // "task 'fetch' PC 0x42001234 ..." or "" if no core dump
std::string FirmwareVersion(); // project version + build date
std::string IdfVersion();

// esp_log level for all tags (1 error .. 4 debug).
void ApplyLogLevel(int level);

}  // namespace sysinfo
