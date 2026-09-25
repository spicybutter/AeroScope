// Typed, schema-driven settings.
//
// Every user option is one row in the schema (settings.cpp). The web page is
// generated from the schema, so adding a row is all it takes to expose a new
// option. Values persist in NVS namespace "config" as strings - the same format
// (and, for the original keys, the same names) as the Arduino build.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace settings {

enum class Type { Bool, Int, Float, Enum, Text, Secret, Ip };

struct Option {
    const char* value;
    const char* label;
};

// Optional extra validation for a Text value; returns false and sets `error`.
using Check = bool (*)(const std::string& value, std::string& error);

struct Def {
    const char* key;      // NVS key (<= 15 chars) and JSON key
    const char* group;    // section on the settings page
    const char* label;
    Type type;
    const char* def;      // default, as stored
    float min;            // Int / Float range
    float max;
    const Option* options;  // Enum choices
    int optionCount;
    bool restart;         // change only takes effect after a restart
    const char* help;
    Check check = nullptr;
};

const Def* Defs();
int Count();
const Def* Find(const std::string& key);

// Unit choices (values stored as the enum's option value)
enum class AltUnit { M, Ft };
enum class SpeedUnit { Ms, Kmh, Mph, Kt };
enum class DistUnit { Km, Mi, Nm };

// User-defined point of interest (#21): "Name,lat,lon" entries separated by ';'
struct Poi {
    std::string name;
    double lat, lon;
};
bool ParsePois(const std::string& text, std::vector<Poi>& out, std::string& error);

// Watchlist (#57): tokens separated by spaces/commas/semicolons, upper-cased.
// A token matches an aircraft whose ICAO24 equals it, or whose callsign starts with it.
std::vector<std::string> ParseWatchlist(const std::string& text);
bool WatchMatches(const std::vector<std::string>& watch, const std::string& icao24, const std::string& callsign);

struct Settings {
    // Location / data
    bool locationSet = false;
    double lat = 0, lon = 0;
    double radius = 1.0;           // degrees, fetch box half-size
    std::string openskyId, openskySecret;
    int pollSeconds = 0;           // 0 = automatic (daily budget)
    // Display
    int brightness = 75;
    int fps = 30;
    int zoom = 1;                  // 1, 2 or 4
    bool splash = true;
    // Radar layers
    bool scanline = true;
    bool infoText = true;
    bool triangles = true;
    bool categoryIcons = true;
    bool altitudeColors = true;
    bool verticalArrows = true;
    bool trails = true;
    int trailSeconds = 60;
    bool trailFade = true;
    bool emergencyHighlight = true;
    // Units
    AltUnit altUnit = AltUnit::M;
    SpeedUnit speedUnit = SpeedUnit::Ms;
    DistUnit distUnit = DistUnit::Km;
    // Overlays
    bool showClock = true;
    bool clock24h = true;
    bool showCount = true;
    bool showWifi = true;
    bool showStatus = true;
    // Time
    std::string timezone = "UTC0";
    std::string ntpServer = "pool.ntp.org";
    // System
    int logLevel = 3;              // esp_log_level_t: 1 error .. 4 debug
    // Map (#21)
    bool showPois = true;
    std::vector<Poi> pois;
    // Screens (#26)
    bool qrCodes = true;
    // Network (#75)
    bool staticIp = false;
    std::string ip, netmask, gateway, dns1, dns2;
    // Touch (#29, #31, #32, #35)
    bool touchSwapXY = false;
    bool touchMirrorX = false;
    bool touchMirrorY = false;
    bool tapBrightness = true;
    bool tapZoom = true;
    bool doubleTapReset = true;
    // Sound & alerts (#46, #50, #55, #57)
    bool sound = true;
    int volume = 80;               // esp_codec_dev volume, 0-100 (-50..0 dB)
    bool alertEmergency = true;
    bool alertWatch = true;
    std::vector<std::string> watchlist;
    bool banners = true;
};

using Raw = std::map<std::string, std::string>;

// Load NVS -> snapshot. Call once at boot (after nvs_flash_init).
void Init();
// Current immutable snapshot; cheap to call every frame.
std::shared_ptr<const Settings> Current();
// Raw stored value (or the schema default).
std::string Value(const std::string& key);

struct UpdateResult {
    bool ok = true;
    bool restartNeeded = false;
    std::string error;            // first validation error, if any
};
// Validate + persist + publish a new snapshot. Unknown keys are rejected;
// a secret containing '*' (the masked value) is ignored.
UpdateResult Update(const Raw& changes);

// Pure helpers (no NVS) - also used by the host unit tests.
bool Validate(const Def& d, const std::string& in, std::string& out, std::string& error);
Settings Parse(const Raw& raw);

}  // namespace settings
