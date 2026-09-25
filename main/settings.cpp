#include "settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef SETTINGS_HOST_TEST
#include "config_store.h"
#include "esp_log.h"
#endif

namespace settings {

// ---------------------------------------------------------------------------
// Schema

static const Option kZoom[] = {{"1", "1x (full radius)"}, {"2", "2x"}, {"4", "4x"}};
static const Option kAlt[] = {{"m", "metres"}, {"ft", "feet"}};
static const Option kSpeed[] = {{"ms", "m/s"}, {"kmh", "km/h"}, {"mph", "mph"}, {"kt", "knots"}};
static const Option kDist[] = {{"km", "kilometres"}, {"mi", "miles"}, {"nm", "nautical miles"}};
static const Option kLog[] = {{"1", "error"}, {"2", "warning"}, {"3", "info"}, {"4", "debug"}};

#define N(a) static_cast<int>(sizeof(a) / sizeof((a)[0]))

static bool CheckWatchlist(const std::string& v, std::string& error)
{
    for (const auto& t : ParseWatchlist(v)) {
        if (t.size() > 8) {
            error = "'" + t + "' is longer than a callsign / ICAO24";
            return false;
        }
    }
    return true;
}

static bool CheckPois(const std::string& v, std::string& error)
{
    std::vector<Poi> pois;
    return ParsePois(v, pois, error);
}

// clang-format off
static const Def kDefs[] = {
    // key               group       label                         type          default          min    max     options  n           restart help
    {"latitude",         "Location", "Latitude",                   Type::Float,  "",              -90,   90,     nullptr, 0,          false, "Centre of the radar, decimal degrees"},
    {"longitude",        "Location", "Longitude",                  Type::Float,  "",              -180,  180,    nullptr, 0,          false, "Centre of the radar, decimal degrees"},
    {"radius",           "Location", "Radius (degrees)",           Type::Float,  "1.0",           0.05f, 2.49f,  nullptr, 0,          false, "Half-size of the area fetched from OpenSky (max 2.49 keeps a request at 1 credit)"},
    {"opensky-id",       "OpenSky",  "Client ID",                  Type::Text,   "",              0,     128,    nullptr, 0,          true,  "Optional: 4000 instead of 400 requests per day"},
    {"opensky-secret",   "OpenSky",  "Client secret",              Type::Secret, "",              0,     128,    nullptr, 0,          true,  ""},
    {"poll_s",           "OpenSky",  "Update interval (s)",        Type::Int,    "0",             0,     3600,   nullptr, 0,          false, "0 = automatic (spread the daily budget). Never faster than the remaining credits allow."},
    {"bright",           "Display",  "Brightness (%)",             Type::Int,    "75",            5,     100,    nullptr, 0,          false, ""},
    {"fps",              "Display",  "Frame rate (fps)",           Type::Int,    "30",            5,     30,     nullptr, 0,          false, "Lower saves power"},
    {"zoom",             "Display",  "Zoom",                       Type::Enum,   "1",             0,     0,      kZoom,   N(kZoom),   false, "Display-only zoom; the fetched area is unchanged"},
    {"splash",           "Display",  "Boot splash screen",         Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"scanline",         "Radar",    "Radar sweep",                Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"infotext",         "Radar",    "Aircraft info labels",       Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Callsign, speed and altitude next to each aircraft"},
    {"triangle",         "Radar",    "Directional aircraft",       Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Off = plain dots"},
    {"cat_icons",        "Radar",    "Icons by aircraft category", Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Heavy / light / helicopter / glider / balloon / drone shapes"},
    {"alt_color",        "Radar",    "Colour by altitude",         Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"vs_arrow",         "Radar",    "Climb / descent arrows",     Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"trail",            "Radar",    "Trails",                     Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"trail_s",          "Radar",    "Trail length (s)",           Type::Int,    "60",            10,    300,    nullptr, 0,          false, ""},
    {"trail_fade",       "Radar",    "Fading trails",              Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"emerg_hl",         "Radar",    "Highlight emergency squawks",Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "7500 / 7600 / 7700"},
    {"u_alt",            "Units",    "Altitude",                   Type::Enum,   "m",             0,     0,      kAlt,    N(kAlt),    false, ""},
    {"u_spd",            "Units",    "Speed",                      Type::Enum,   "ms",            0,     0,      kSpeed,  N(kSpeed),  false, ""},
    {"u_dist",           "Units",    "Distance",                   Type::Enum,   "km",            0,     0,      kDist,   N(kDist),   false, ""},
    {"show_clock",       "Overlays", "Clock",                      Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Needs internet time (NTP)"},
    {"clock_24h",        "Overlays", "24-hour clock",              Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"show_count",       "Overlays", "Aircraft count",             Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"show_wifi",        "Overlays", "Wi-Fi signal icon",          Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"show_status",      "Overlays", "Data status",                Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Age of the last update; turns yellow/red on problems"},
    {"tz",               "Time",     "Time zone (POSIX TZ)",       Type::Text,   "UTC0",          1,     63,     nullptr, 0,          true,  "e.g. EST5EDT,M3.2.0,M11.1.0 or CET-1CEST,M3.5.0,M10.5.0/3"},
    {"ntp",              "Time",     "NTP server",                 Type::Text,   "pool.ntp.org",  1,     63,     nullptr, 0,          true,  ""},
    {"log_level",        "System",   "Log level",                  Type::Enum,   "3",             0,     0,      kLog,    N(kLog),    false, "Serial / web log verbosity"},
    {"show_pois",        "Map",      "Show points of interest",    Type::Bool,   "true",          0,     0,      nullptr, 0,          false, ""},
    {"pois",             "Map",      "Points of interest",         Type::Text,   "",              0,     600,    nullptr, 0,          false, "Name,lat,lon entries separated by ';' e.g. LHR,51.4700,-0.4543;Home,51.50,-0.12", CheckPois},
    {"qr",               "Display",  "QR codes on setup/connect screens", Type::Bool, "true",     0,     0,      nullptr, 0,          false, "Scan to join the setup hotspot / open this page"},
    {"static_ip",        "Network",  "Static IP",                  Type::Bool,   "false",         0,     0,      nullptr, 0,          true,  "Off = DHCP"},
    {"ip",               "Network",  "IP address",                 Type::Ip,     "",              0,     0,      nullptr, 0,          true,  ""},
    {"netmask",          "Network",  "Netmask",                    Type::Ip,     "255.255.255.0", 0,     0,      nullptr, 0,          true,  ""},
    {"gateway",          "Network",  "Gateway",                    Type::Ip,     "",              0,     0,      nullptr, 0,          true,  ""},
    {"dns1",             "Network",  "DNS 1",                      Type::Ip,     "",              0,     0,      nullptr, 0,          true,  "Empty = gateway"},
    {"dns2",             "Network",  "DNS 2",                      Type::Ip,     "",              0,     0,      nullptr, 0,          true,  ""},
    {"tap_bright",       "Touch",    "Tap to cycle brightness",    Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Tap the middle of the radar: 25 / 50 / 75 / 100 %"},
    {"tap_zoom",         "Touch",    "Tap ring / swipe to zoom",   Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Tap near the outer ring to cycle zoom; swipe up / down to zoom in / out"},
    {"dbl_reset",        "Touch",    "Double-tap to reset view",   Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Back to the configured zoom and the radar page"},
    {"touch_swap",       "Touch",    "Calibration: swap X/Y",      Type::Bool,   "false",         0,     0,      nullptr, 0,          false, "Only if touches land in the wrong place"},
    {"touch_mx",         "Touch",    "Calibration: mirror X",      Type::Bool,   "false",         0,     0,      nullptr, 0,          false, ""},
    {"touch_my",         "Touch",    "Calibration: mirror Y",      Type::Bool,   "false",         0,     0,      nullptr, 0,          false, ""},
    {"sound",            "Sound & alerts", "Sounds",               Type::Bool,   "true",          0,     0,      nullptr, 0,          false, "Master switch for all alert sounds"},
    {"volume",           "Sound & alerts", "Volume",               Type::Int,    "80",            0,     100,    nullptr, 0,          false, "0-100 (logarithmic: 50 is about -25 dB)"},
    {"alert_emerg",      "Sound & alerts", "Alert on emergency squawk", Type::Bool, "true",       0,     0,      nullptr, 0,          false, "7500 / 7600 / 7700"},
    {"alert_watch",      "Sound & alerts", "Alert on watched aircraft", Type::Bool, "true",       0,     0,      nullptr, 0,          false, ""},
    {"watchlist",        "Sound & alerts", "Watchlist",            Type::Text,   "",              0,     300,    nullptr, 0,          false, "Callsigns / prefixes / ICAO24 codes, e.g. BAW1 EZY 4CA2B6 - alert when one appears", CheckWatchlist},
    {"banners",          "Sound & alerts", "On-screen alert banners", Type::Bool, "true",         0,     0,      nullptr, 0,          false, "Tap the banner to dismiss"},
};
// clang-format on

const Def* Defs() { return kDefs; }
int Count() { return N(kDefs); }

const Def* Find(const std::string& key)
{
    for (const auto& d : kDefs)
        if (key == d.key) return &d;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Validation / parsing (pure)

bool Validate(const Def& d, const std::string& in, std::string& out, std::string& error)
{
    char buf[48];
    switch (d.type) {
    case Type::Bool:
        if (in == "true" || in == "1" || in == "on") { out = "true"; return true; }
        if (in == "false" || in == "0" || in == "off" || in.empty()) { out = "false"; return true; }
        error = std::string(d.label) + ": not a boolean";
        return false;
    case Type::Int: {
        char* end = nullptr;
        const long v = strtol(in.c_str(), &end, 10);
        if (in.empty() || *end != '\0') { error = std::string(d.label) + ": not a whole number"; return false; }
        const long c = std::lround(std::fmin(std::fmax(static_cast<float>(v), d.min), d.max));
        snprintf(buf, sizeof(buf), "%ld", c);
        out = buf;
        return true;
    }
    case Type::Float: {
        if (in.empty() && d.def[0] == '\0') { out = ""; return true; }  // optional (lat/lon unset)
        char* end = nullptr;
        const double v = strtod(in.c_str(), &end);
        if (in.empty() || *end != '\0' || std::isnan(v)) { error = std::string(d.label) + ": not a number"; return false; }
        if (v < d.min || v > d.max) {
            snprintf(buf, sizeof(buf), ": must be %g..%g", d.min, d.max);
            error = std::string(d.label) + buf;
            return false;
        }
        out = in;  // keep the user's precision
        return true;
    }
    case Type::Enum:
        for (int i = 0; i < d.optionCount; i++)
            if (in == d.options[i].value) { out = in; return true; }
        error = std::string(d.label) + ": invalid choice";
        return false;
    case Type::Text:
    case Type::Secret:
        if (in.size() > static_cast<size_t>(d.max)) { error = std::string(d.label) + ": too long"; return false; }
        if (in.size() < static_cast<size_t>(d.min)) { error = std::string(d.label) + ": required"; return false; }
        if (d.check && !d.check(in, error)) { error = std::string(d.label) + ": " + error; return false; }
        out = in;
        return true;
    case Type::Ip: {
        if (in.empty()) { out = ""; return true; }  // optional; checked when static IP is used
        unsigned a, b, c, e;
        char tail;
        if (sscanf(in.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &e, &tail) != 4 || a > 255 || b > 255 || c > 255 ||
            e > 255) {
            error = std::string(d.label) + ": not an IPv4 address";
            return false;
        }
        out = in;
        return true;
    }
    }
    return false;
}

std::vector<std::string> ParseWatchlist(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + " ") {
        if (c == ' ' || c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
    }
    return out;
}

bool WatchMatches(const std::vector<std::string>& watch, const std::string& icao24, const std::string& callsign)
{
    std::string icao = icao24, cs = callsign;
    for (auto& c : icao) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    for (auto& c : cs) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    for (const auto& t : watch) {
        if (t == icao) return true;
        if (!cs.empty() && cs.compare(0, t.size(), t) == 0) return true;
    }
    return false;
}

bool ParsePois(const std::string& text, std::vector<Poi>& out, std::string& error)
{
    out.clear();
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(';', start);
        if (end == std::string::npos) end = text.size();
        std::string entry = text.substr(start, end - start);
        start = end + 1;
        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\n' || entry.front() == '\r')) entry.erase(0, 1);
        while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\n' || entry.back() == '\r')) entry.pop_back();
        if (entry.empty()) continue;
        const size_t c1 = entry.find(','), c2 = c1 == std::string::npos ? c1 : entry.find(',', c1 + 1);
        if (c2 == std::string::npos) { error = "'" + entry + "' should be Name,lat,lon"; return false; }
        Poi p;
        p.name = entry.substr(0, c1);
        char* e1 = nullptr;
        char* e2 = nullptr;
        const std::string latS = entry.substr(c1 + 1, c2 - c1 - 1), lonS = entry.substr(c2 + 1);
        p.lat = strtod(latS.c_str(), &e1);
        p.lon = strtod(lonS.c_str(), &e2);
        if (p.name.empty() || latS.empty() || lonS.empty() || *e1 || *e2 || p.lat < -90 || p.lat > 90 ||
            p.lon < -180 || p.lon > 180) {
            error = "'" + entry + "' should be Name,lat,lon";
            return false;
        }
        if (p.name.size() > 12) p.name.resize(12);
        out.push_back(p);
        if (out.size() > 32) { error = "at most 32 points"; return false; }
    }
    return true;
}

static std::string Get(const Raw& raw, const char* key)
{
    auto it = raw.find(key);
    if (it != raw.end()) return it->second;
    const Def* d = Find(key);
    return d ? d->def : "";
}

static bool B(const Raw& raw, const char* key)
{
    const std::string v = Get(raw, key);
    return v.empty() || v == "true";  // never saved = default on (Arduino semantics)
}

static int I(const Raw& raw, const char* key)
{
    const Def* d = Find(key);
    std::string out, err;
    if (d && Validate(*d, Get(raw, key), out, err)) return atoi(out.c_str());
    return d ? atoi(d->def) : 0;
}

Settings Parse(const Raw& raw)
{
    Settings s;
    const std::string lat = Get(raw, "latitude"), lon = Get(raw, "longitude");
    s.locationSet = !lat.empty() && !lon.empty();
    s.lat = strtod(lat.c_str(), nullptr);
    s.lon = strtod(lon.c_str(), nullptr);
    const double r = strtod(Get(raw, "radius").c_str(), nullptr);
    s.radius = r > 0 ? r : 1.0;
    s.openskyId = Get(raw, "opensky-id");
    s.openskySecret = Get(raw, "opensky-secret");
    s.pollSeconds = I(raw, "poll_s");

    s.brightness = I(raw, "bright");
    s.fps = I(raw, "fps");
    const int z = atoi(Get(raw, "zoom").c_str());
    s.zoom = (z == 2 || z == 4) ? z : 1;
    s.splash = B(raw, "splash");

    s.scanline = B(raw, "scanline");
    s.infoText = B(raw, "infotext");
    s.triangles = B(raw, "triangle");
    s.categoryIcons = B(raw, "cat_icons");
    s.altitudeColors = B(raw, "alt_color");
    s.verticalArrows = B(raw, "vs_arrow");
    s.trails = B(raw, "trail");
    s.trailSeconds = I(raw, "trail_s");
    s.trailFade = B(raw, "trail_fade");
    s.emergencyHighlight = B(raw, "emerg_hl");

    const std::string a = Get(raw, "u_alt"), sp = Get(raw, "u_spd"), di = Get(raw, "u_dist");
    s.altUnit = a == "ft" ? AltUnit::Ft : AltUnit::M;
    s.speedUnit = sp == "kmh" ? SpeedUnit::Kmh : sp == "mph" ? SpeedUnit::Mph : sp == "kt" ? SpeedUnit::Kt : SpeedUnit::Ms;
    s.distUnit = di == "mi" ? DistUnit::Mi : di == "nm" ? DistUnit::Nm : DistUnit::Km;

    s.showClock = B(raw, "show_clock");
    s.clock24h = B(raw, "clock_24h");
    s.showCount = B(raw, "show_count");
    s.showWifi = B(raw, "show_wifi");
    s.showStatus = B(raw, "show_status");

    s.timezone = Get(raw, "tz");
    if (s.timezone.empty()) s.timezone = "UTC0";
    s.ntpServer = Get(raw, "ntp");
    if (s.ntpServer.empty()) s.ntpServer = "pool.ntp.org";
    s.logLevel = I(raw, "log_level");

    s.showPois = B(raw, "show_pois");
    std::string err;
    if (!ParsePois(Get(raw, "pois"), s.pois, err)) s.pois.clear();
    s.qrCodes = B(raw, "qr");

    s.staticIp = Get(raw, "static_ip") == "true";
    s.ip = Get(raw, "ip");
    s.netmask = Get(raw, "netmask");
    s.gateway = Get(raw, "gateway");
    s.dns1 = Get(raw, "dns1");
    s.dns2 = Get(raw, "dns2");

    s.tapBrightness = B(raw, "tap_bright");
    s.tapZoom = B(raw, "tap_zoom");
    s.doubleTapReset = B(raw, "dbl_reset");
    s.touchSwapXY = Get(raw, "touch_swap") == "true";
    s.touchMirrorX = Get(raw, "touch_mx") == "true";
    s.touchMirrorY = Get(raw, "touch_my") == "true";

    s.sound = B(raw, "sound");
    s.volume = I(raw, "volume");
    s.alertEmergency = B(raw, "alert_emerg");
    s.alertWatch = B(raw, "alert_watch");
    s.watchlist = ParseWatchlist(Get(raw, "watchlist"));
    s.banners = B(raw, "banners");
    return s;
}

// ---------------------------------------------------------------------------
// Storage + snapshot (device only)

#ifndef SETTINGS_HOST_TEST

static const char* TAG = "SETTINGS";
static std::mutex s_mutex;
static Raw s_raw;
static std::mutex s_snapshot_mutex;  // guards s_current only (never held while writing NVS)
static std::shared_ptr<const Settings> s_current = std::make_shared<Settings>();

static void Publish(const Raw& raw)
{
    auto next = std::make_shared<const Settings>(Parse(raw));
    std::lock_guard<std::mutex> lock(s_snapshot_mutex);
    s_current = std::move(next);
}

void Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_raw.clear();
    for (const auto& d : kDefs) {
        const std::string v = config::Get(d.key, "\x01");
        if (v != "\x01") s_raw[d.key] = v;  // only keys that were actually saved
    }
    Publish(s_raw);
}

std::shared_ptr<const Settings> Current()
{
    std::lock_guard<std::mutex> lock(s_snapshot_mutex);
    return s_current;
}

std::string Value(const std::string& key)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_raw.find(key);
    if (it != s_raw.end()) return it->second;
    const Def* d = Find(key);
    return d ? d->def : "";
}

UpdateResult Update(const Raw& changes)
{
    UpdateResult result;
    Raw validated;
    for (const auto& [key, value] : changes) {
        const Def* d = Find(key);
        if (!d) {
            result.ok = false;
            result.error = "unknown setting: " + key;
            return result;
        }
        if (d->type == Type::Secret && value.find('*') != std::string::npos) continue;  // masked echo
        std::string out;
        if (!Validate(*d, value, out, result.error)) {
            result.ok = false;
            return result;
        }
        validated[key] = out;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    for (const auto& [key, value] : validated) {
        const Def* d = Find(key);
        auto it = s_raw.find(key);
        const std::string old = it != s_raw.end() ? it->second : d->def;
        if (old == value) continue;
        if (!config::Set(key.c_str(), value)) {
            result.ok = false;
            result.error = "could not save " + key;
            return result;
        }
        s_raw[key] = value;
        if (d->restart) result.restartNeeded = true;
        ESP_LOGI(TAG, "%s = %s%s", key.c_str(), d->type == Type::Secret ? "***" : value.c_str(),
                 d->restart ? " (restart)" : "");
    }
    Publish(s_raw);
    return result;
}

#endif  // SETTINGS_HOST_TEST

}  // namespace settings
