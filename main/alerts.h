// Alerts (#50 emergency squawks, #57 watched aircraft): detection, sound, and
// fan-out to listeners (LCD banner #55, web page / browser notification #57).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class AircraftManager;

namespace alerts {

enum class Kind { Emergency, Watch };

struct Alert {
    uint32_t id;
    Kind kind;
    std::string title;   // e.g. "EMERGENCY 7700"
    std::string text;    // e.g. "BAW123  3200 m  12.4 km"
    std::string icao24;
    uint32_t timeMs;
};

using Listener = std::function<void(const Alert&)>;
void AddListener(Listener l);   // call before Start()

void Start(AircraftManager& aircraft);

std::vector<Alert> Active();    // not yet acknowledged, oldest first
void Ack(uint32_t id);
void AckAll();

// Pure detection logic (host-tested).
struct Seen {
    std::string icao24, callsign, squawk;
    bool onGround;
};
struct Detector {
    std::vector<std::string> previous;             // ICAO24s present last poll
    std::vector<std::pair<std::string, uint32_t>> fired;  // key -> time (dedupe)
    // Returns keys ("E:<icao>:<squawk>" / "W:<icao>") that should raise an alert now.
    std::vector<std::string> Poll(const std::vector<Seen>& now, const std::vector<std::string>& watch,
                                  bool emergencies, bool watches, uint32_t nowMs, uint32_t repeatMs);
};

}  // namespace alerts
