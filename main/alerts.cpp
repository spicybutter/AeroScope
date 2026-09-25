#include "alerts.h"

#include <algorithm>

#include "radar_logic.h"
#include "settings.h"

#ifndef ALERTS_HOST_TEST
#include <cstdio>
#include <mutex>

#include "aircraft.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "units.h"
#endif

namespace alerts {

// ---------------------------------------------------------------------------
// Detection (pure)

std::vector<std::string> Detector::Poll(const std::vector<Seen>& now, const std::vector<std::string>& watch,
                                        bool emergencies, bool watches, uint32_t nowMs, uint32_t repeatMs)
{
    // Forget dedupe entries older than the repeat window.
    fired.erase(std::remove_if(fired.begin(), fired.end(),
                               [&](const auto& f) { return nowMs - f.second >= repeatMs; }),
                fired.end());
    auto recentlyFired = [&](const std::string& key) {
        return std::any_of(fired.begin(), fired.end(), [&](const auto& f) { return f.first == key; });
    };

    std::vector<std::string> raise;
    for (const auto& a : now) {
        if (a.onGround) continue;
        if (emergencies && radar::IsEmergencySquawk(a.squawk)) {
            const std::string key = "E:" + a.icao24 + ":" + a.squawk;
            if (!recentlyFired(key)) raise.push_back(key);
        }
        if (watches && !watch.empty() && settings::WatchMatches(watch, a.icao24, a.callsign)) {
            const bool wasHere = std::find(previous.begin(), previous.end(), a.icao24) != previous.end();
            const std::string key = "W:" + a.icao24;
            if (!wasHere && !recentlyFired(key)) raise.push_back(key);
        }
    }
    for (const auto& k : raise) fired.push_back({k, nowMs});

    previous.clear();
    for (const auto& a : now) previous.push_back(a.icao24);
    return raise;
}

#ifndef ALERTS_HOST_TEST

// ---------------------------------------------------------------------------
// Runtime

static const char* TAG = "ALERT";
static constexpr uint32_t POLL_MS = 2000;
static constexpr uint32_t REPEAT_MS = 30u * 60u * 1000u;
static constexpr size_t MAX_ACTIVE = 10;

static AircraftManager* s_aircraft;
static std::mutex s_mutex;
static std::vector<Listener> s_listeners;
static std::vector<Alert> s_active;
static uint32_t s_nextId = 1;

void AddListener(Listener l) { s_listeners.push_back(std::move(l)); }

std::vector<Alert> Active()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_active;
}

void Ack(uint32_t id)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_active.erase(std::remove_if(s_active.begin(), s_active.end(), [&](const Alert& a) { return a.id == id; }),
                   s_active.end());
}

void AckAll()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_active.clear();
}

static std::string Describe(const AircraftInfo& a, const settings::Settings& s)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s  %s  %s", a.callsign.empty() ? a.icao24.c_str() : a.callsign.c_str(),
             units::Format(units::Altitude(a.altitude, s.altUnit), units::AltSuffix(s.altUnit)).c_str(),
             units::Format(units::Distance(a.distanceKm, s.distUnit), units::DistSuffix(s.distUnit)).c_str());
    return buf;
}

static void Raise(Kind kind, const AircraftInfo& a, const settings::Settings& s)
{
    Alert al;
    al.kind = kind;
    al.icao24 = a.icao24;
    al.timeMs = Millis();
    if (kind == Kind::Emergency) {
        al.title = std::string(radar::EmergencyName(a.squawk)) + " " + a.squawk;
        audio::Play(audio::Sound::Emergency);
    } else {
        al.title = "WATCHED AIRCRAFT";
        audio::Play(audio::Sound::Watch);
    }
    al.text = Describe(a, s);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        al.id = s_nextId++;
        s_active.push_back(al);
        if (s_active.size() > MAX_ACTIVE) s_active.erase(s_active.begin());
    }
    ESP_LOGW(TAG, "%s: %s (%s)", al.title.c_str(), al.text.c_str(), al.icao24.c_str());
    for (const auto& l : s_listeners) l(al);
}

static void Task(void*)
{
    Detector detector;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const auto s = settings::Current();
        if (!s->locationSet) continue;
        const auto list = s_aircraft->Snapshot(s->lat, s->lon);
        std::vector<Seen> seen;
        seen.reserve(list.size());
        for (const auto& a : list) seen.push_back({a.icao24, a.callsign, a.squawk, a.onGround});
        for (const auto& key : detector.Poll(seen, s->watchlist, s->alertEmergency, s->alertWatch, Millis(), REPEAT_MS)) {
            const std::string icao = key.substr(2, key.find(':', 2) == std::string::npos ? std::string::npos
                                                                                         : key.find(':', 2) - 2);
            for (const auto& a : list) {
                if (a.icao24 == icao) {
                    Raise(key[0] == 'E' ? Kind::Emergency : Kind::Watch, a, *s);
                    break;
                }
            }
        }
    }
}

void Start(AircraftManager& aircraft)
{
    s_aircraft = &aircraft;
    xTaskCreatePinnedToCore(Task, "alerts", 4096, nullptr, 2, nullptr, 0);
}

#endif  // ALERTS_HOST_TEST

}  // namespace alerts
