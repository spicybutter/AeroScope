// Pure radar logic shared by the renderer and the host unit tests:
// colours, altitude ramp, request pacing, emergency squawks, category shapes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace radar {

// RGB565 (native order; the display flush byte-swaps)
constexpr uint16_t Rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t Scale(uint16_t c, float f)
{
    return Rgb(static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31 * f),
               static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63 * f),
               static_cast<uint8_t>((c & 0x1F) * 255 / 31 * f));
}

// Altitude (m) -> colour ramp: low orange -> yellow -> green -> cyan -> blue -> magenta (high)
inline uint16_t AltitudeColor(float metres)
{
    struct Stop { float alt; uint8_t r, g, b; };
    static constexpr Stop stops[] = {
        {0, 255, 110, 0},     {1000, 255, 220, 0},  {3000, 120, 255, 0},
        {6000, 0, 255, 160},  {9000, 0, 170, 255},  {12000, 200, 80, 255},
    };
    constexpr int n = sizeof(stops) / sizeof(stops[0]);
    if (metres <= stops[0].alt) return Rgb(stops[0].r, stops[0].g, stops[0].b);
    for (int i = 1; i < n; i++) {
        if (metres <= stops[i].alt) {
            const float t = (metres - stops[i - 1].alt) / (stops[i].alt - stops[i - 1].alt);
            auto lerp = [t](uint8_t a, uint8_t b) { return static_cast<uint8_t>(a + (b - a) * t); };
            return Rgb(lerp(stops[i - 1].r, stops[i].r), lerp(stops[i - 1].g, stops[i].g),
                       lerp(stops[i - 1].b, stops[i].b));
        }
    }
    return Rgb(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
}

inline bool IsEmergencySquawk(const std::string& sq) { return sq == "7500" || sq == "7600" || sq == "7700"; }
inline const char* EmergencyName(const std::string& sq)
{
    return sq == "7500" ? "HIJACK" : sq == "7600" ? "RADIO FAIL" : sq == "7700" ? "EMERGENCY" : "";
}

// OpenSky "category" (extended=1) -> symbol family
enum class Shape { Default, Light, Large, Heavy, Rotorcraft, Glider, Balloon, Drone };
inline Shape ShapeForCategory(int category)
{
    switch (category) {
    case 2: case 3: case 12: return Shape::Light;   // light, small, ultralight
    case 4: return Shape::Large;                    // large
    case 5: case 6: return Shape::Heavy;            // high-vortex large, heavy
    case 8: return Shape::Rotorcraft;
    case 9: return Shape::Glider;
    case 10: return Shape::Balloon;                 // lighter-than-air
    case 14: return Shape::Drone;                   // UAV
    default: return Shape::Default;                 // 0/1 unknown, 7 high-perf, 11+, ...
    }
}

// Request pacing. Budget-based interval (upstream behaviour), optionally a user
// interval, stretched so the remaining OpenSky credits last until the daily
// reset at 00:00 UTC. All times in ms / s as named.
struct PacingInput {
    bool authenticated = false;
    int userIntervalS = 0;        // 0 = automatic
    int creditsRemaining = -1;    // -1 unknown
    int64_t secondsToReset = -1;  // -1 unknown (no NTP yet)
};
inline uint32_t BudgetIntervalMs(bool authenticated)
{
    constexpr uint32_t MS_PER_DAY = 24u * 60u * 60u * 1000u;
    return MS_PER_DAY / (authenticated ? 4000 - 3 : 400 - 3);
}
inline uint32_t NextIntervalMs(const PacingInput& in)
{
    constexpr uint32_t MIN_MS = 5000;
    uint32_t ms = in.userIntervalS > 0 ? static_cast<uint32_t>(in.userIntervalS) * 1000u
                                       : BudgetIntervalMs(in.authenticated);
    if (in.creditsRemaining >= 0 && in.secondsToReset > 0) {
        const int64_t usable = std::max<int64_t>(in.creditsRemaining - 3, 1);  // keep a small buffer
        const int64_t paced = in.secondsToReset * 1000 / usable;
        ms = static_cast<uint32_t>(std::max<int64_t>(ms, std::min<int64_t>(paced, INT32_MAX)));
    }
    return std::max(ms, MIN_MS);
}

}  // namespace radar
