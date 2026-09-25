// NTP time sync + local time zone (#73).
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace timekeeping {

void Start(const std::string& ntpServer, const std::string& posixTz);
bool Synced();
// "14:05" / "2:05 PM"; empty until synced.
std::string ClockText(bool h24);
// Seconds until the next 00:00 UTC (OpenSky's daily credit reset); -1 if not synced.
int64_t SecondsToUtcMidnight();
// "2026-09-24 21:44:55" local time, or "not synced".
std::string LocalTimestamp();

}  // namespace timekeeping
