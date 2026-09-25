// Unit conversion and formatting (pure; host-tested).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "settings.h"

namespace units {

inline double Altitude(double metres, settings::AltUnit u)
{
    return u == settings::AltUnit::Ft ? metres / 0.3048 : metres;
}
inline const char* AltSuffix(settings::AltUnit u) { return u == settings::AltUnit::Ft ? "ft" : "m"; }

inline double Speed(double ms, settings::SpeedUnit u)
{
    switch (u) {
    case settings::SpeedUnit::Kmh: return ms * 3.6;
    case settings::SpeedUnit::Mph: return ms * 2.2369362920544;
    case settings::SpeedUnit::Kt: return ms * 1.9438444924406;
    default: return ms;
    }
}
inline const char* SpeedSuffix(settings::SpeedUnit u)
{
    switch (u) {
    case settings::SpeedUnit::Kmh: return "km/h";
    case settings::SpeedUnit::Mph: return "mph";
    case settings::SpeedUnit::Kt: return "kt";
    default: return "m/s";
    }
}

inline double Distance(double km, settings::DistUnit u)
{
    switch (u) {
    case settings::DistUnit::Mi: return km / 1.609344;
    case settings::DistUnit::Nm: return km / 1.852;
    default: return km;
    }
}
inline const char* DistSuffix(settings::DistUnit u)
{
    switch (u) {
    case settings::DistUnit::Mi: return "mi";
    case settings::DistUnit::Nm: return "nm";
    default: return "km";
    }
}

// Vertical rate: m/s, or ft/min when altitude is shown in feet.
inline double VerticalRate(double ms, settings::AltUnit u)
{
    return u == settings::AltUnit::Ft ? ms * 196.850393700787 : ms;
}
inline const char* VerticalSuffix(settings::AltUnit u) { return u == settings::AltUnit::Ft ? "fpm" : "m/s"; }

inline std::string Format(double value, const char* suffix)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld%s", std::lround(value), suffix);
    return buf;
}

// Great-circle distance (km) and initial bearing (deg) - used by Nearby/Details.
inline double HaversineKm(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371.0088, d2r = M_PI / 180.0;
    const double dlat = (lat2 - lat1) * d2r, dlon = (lon2 - lon1) * d2r;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2 * r * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
inline double BearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double d2r = M_PI / 180.0;
    const double y = std::sin((lon2 - lon1) * d2r) * std::cos(lat2 * d2r);
    const double x = std::cos(lat1 * d2r) * std::sin(lat2 * d2r) -
                     std::sin(lat1 * d2r) * std::cos(lat2 * d2r) * std::cos((lon2 - lon1) * d2r);
    const double b = std::atan2(y, x) / d2r;
    return b < 0 ? b + 360.0 : b;
}

}  // namespace units
