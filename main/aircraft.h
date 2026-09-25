// Aircraft model, tracking/interpolation, trails and the fetch loop.
// Port of micro-radar models/Aircraft, models/TrackedAircraft and the data half
// of AircraftManager (drawing lives in radar_view.*).
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

inline uint32_t Millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
inline float Radians(float deg) { return deg * static_cast<float>(M_PI) / 180.0f; }

// Maps to the OpenSky /states/all state vector (field indices in comments)
struct Aircraft {
    std::string icao24;        // [0]
    std::string callsign;      // [1] (trailing spaces trimmed)
    std::string originCountry; // [2]
    long timePosition = 0;     // [3]
    long lastContact = 0;      // [4]
    float longitude = 0;       // [5]
    float latitude = 0;        // [6]
    float baroAltitude = 0;    // [7] metres
    bool onGround = false;     // [8]
    float velocity = 0;        // [9] m/s
    float trueTrack = 0;       // [10] degrees clockwise from north
    float verticalRate = 0;    // [11] m/s
    float geoAltitude = 0;     // [13]
    std::string squawk;        // [14]
    bool spi = false;          // [15]
    int positionSource = 0;    // [16]
    int category = 0;          // [17] (requested with extended=1)
};

struct TrailPoint {
    float lat, lon;
    uint32_t t;
};

struct TrackedAircraft {
    static constexpr int TRAIL_CAP = 64;

    Aircraft state;
    uint32_t lastSeen;

    float blendFromLat;
    float blendFromLon;
    float blendAlpha = 1.0f;   // 1.0 = blend complete
    uint32_t lastTick = 0;

    // Trail ring buffer (PSRAM), sampled from the displayed position.
    struct PsramFree { void operator()(TrailPoint* p) const; };
    std::unique_ptr<TrailPoint[], PsramFree> trail;
    int trailHead = 0;   // next write index
    int trailCount = 0;
    uint32_t lastTrailSample = 0;

    TrackedAircraft(const Aircraft& ac, uint32_t now);

    void Update(const Aircraft& newState, uint32_t now);
    void Tick();
    std::pair<float, float> GetDisplayPosition() const;
    std::pair<float, float> PredictPosition() const;
    void SampleTrail(float lat, float lon, uint32_t now, uint32_t trailMs);
    // Oldest -> newest; returns false when index is out of range.
    const TrailPoint* TrailAt(int i) const;
};

// Data-status for the overlay and web page (#14, #72).
struct FetchStatus {
    bool authenticated = false;
    bool haveData = false;         // at least one successful update
    uint32_t lastAttemptMs = 0;
    uint32_t lastSuccessMs = 0;
    int consecutiveErrors = 0;
    std::string lastError;         // empty after a success
    int httpStatus = 0;
    int creditsRemaining = -1;
    uint32_t latencyMs = 0;
    uint32_t intervalMs = 0;       // current pacing
    uint32_t nextFetchMs = 0;
    int total = 0;                 // aircraft in the box
    int airborne = 0;
};

// Plain copy of one aircraft for the web API / pages (no locking needed).
struct AircraftInfo {
    std::string icao24, callsign, country, squawk;
    float lat, lon;          // displayed (predicted) position
    float altitude;          // barometric, m
    float velocity;          // m/s
    float track;             // deg
    float verticalRate;      // m/s
    int category;
    bool onGround;
    double distanceKm;       // from the radar centre
    double bearingDeg;
    uint32_t ageMs;          // since the last OpenSky update of this aircraft
};

class OpenSkyClient;

class AircraftManager {
public:
    explicit AircraftManager(OpenSkyClient& opensky) : opensky_(opensky), lock_(xSemaphoreCreateMutex()) {}

    // Checks OpenSky credentials (authenticated vs anonymous budget).
    void Initialise();
    // Background fetch task (the Arduino build fetched inside loop()).
    void StartFetchTask();

    // Run `fn` with exclusive access to the tracked aircraft (renderer).
    void WithAircraft(const std::function<void(std::map<std::string, TrackedAircraft>&)>& fn);
    FetchStatus Status();
    // Copy of all tracked aircraft, nearest first.
    std::vector<AircraftInfo> Snapshot(double centreLat, double centreLon);

private:
    static void FetchTask(void* arg);
    void FetchOnce(const settings::Settings& s);

    OpenSkyClient& opensky_;
    SemaphoreHandle_t lock_;
    std::map<std::string, TrackedAircraft> tracked_;
    FetchStatus status_;
};
