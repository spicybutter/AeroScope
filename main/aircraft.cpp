#include "aircraft.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "net.h"
#include "opensky.h"
#include "radar_logic.h"
#include "timekeeping.h"
#include "units.h"

static const char* TAG = "RADAR";

// ---------------------------------------------------------------------------
// TrackedAircraft (prediction/blend maths identical to models/TrackedAircraft.h)

void TrackedAircraft::PsramFree::operator()(TrailPoint* p) const { heap_caps_free(p); }

TrackedAircraft::TrackedAircraft(const Aircraft& ac, uint32_t now)
    : state(ac), lastSeen(now), blendFromLat(ac.latitude), blendFromLon(ac.longitude),
      trail(static_cast<TrailPoint*>(heap_caps_malloc(sizeof(TrailPoint) * TRAIL_CAP, MALLOC_CAP_SPIRAM)))
{
}

void TrackedAircraft::Update(const Aircraft& newState, uint32_t now)
{
    auto [curLat, curLon] = GetDisplayPosition();
    blendFromLat = curLat;
    blendFromLon = curLon;
    blendAlpha = 0.0f;
    state = newState;
    lastSeen = now;
}

void TrackedAircraft::Tick()
{
    const uint32_t now = Millis();
    const float deltaSeconds = (now - lastTick) / 1000.0f;
    lastTick = now;
    const float blendSpeed = 0.15f;
    blendAlpha = std::min(blendAlpha + deltaSeconds * blendSpeed, 1.0f);
}

std::pair<float, float> TrackedAircraft::GetDisplayPosition() const
{
    auto [deadLat, deadLon] = PredictPosition();
    if (blendAlpha >= 1.0f) return {deadLat, deadLon};
    const float t = blendAlpha * blendAlpha * (3.0f - 2.0f * blendAlpha);  // smoothstep
    return {blendFromLat + t * (deadLat - blendFromLat), blendFromLon + t * (deadLon - blendFromLon)};
}

std::pair<float, float> TrackedAircraft::PredictPosition() const
{
    float dataAgeOnArrival = 0.0f;
    if (state.timePosition > 0 && state.lastContact > 0)
        dataAgeOnArrival = static_cast<float>(state.lastContact - state.timePosition);

    const float localElapsed = (Millis() - lastSeen) / 1000.0f;
    const float dt = localElapsed + dataAgeOnArrival;

    const float headingRad = Radians(state.trueTrack);
    const float latMetersPerDeg = 111320.0f;
    const float deltaLat = (state.velocity * dt * std::cos(headingRad)) / latMetersPerDeg;
    const float deltaLon = (state.velocity * dt * std::sin(headingRad)) /
                           (latMetersPerDeg * std::cos(Radians(state.latitude)));
    return {state.latitude + deltaLat, state.longitude + deltaLon};
}

void TrackedAircraft::SampleTrail(float lat, float lon, uint32_t now, uint32_t trailMs)
{
    if (!trail) return;
    // Spread the ring over the configured trail length (at least 1 s apart).
    const uint32_t interval = std::max<uint32_t>(1000, trailMs / (TRAIL_CAP - 1));
    if (trailCount > 0 && now - lastTrailSample < interval) return;
    lastTrailSample = now;
    trail[trailHead] = {lat, lon, now};
    trailHead = (trailHead + 1) % TRAIL_CAP;
    trailCount = std::min(trailCount + 1, TRAIL_CAP);
}

const TrailPoint* TrackedAircraft::TrailAt(int i) const
{
    if (!trail || i < 0 || i >= trailCount) return nullptr;
    const int oldest = (trailHead - trailCount + TRAIL_CAP) % TRAIL_CAP;
    return &trail[(oldest + i) % TRAIL_CAP];
}

// ---------------------------------------------------------------------------
// AircraftManager

void AircraftManager::Initialise()
{
    const auto s = settings::Current();
    const bool authed = !opensky_.GetValidToken(s->openskyId, s->openskySecret).empty();
    status_.authenticated = authed;
    status_.intervalMs = radar::BudgetIntervalMs(authed);
    ESP_LOGI(TAG, "OpenSky %s, base interval %.1f s", authed ? "authenticated" : "anonymous",
             status_.intervalMs / 1000.0f);
}

void AircraftManager::StartFetchTask()
{
    // TLS + JSON parsing need a roomy stack; runs beside the render task so the
    // display keeps animating during requests.
    xTaskCreatePinnedToCore(FetchTask, "fetch", 12288, this, 4, nullptr, 0);
}

void AircraftManager::WithAircraft(const std::function<void(std::map<std::string, TrackedAircraft>&)>& fn)
{
    xSemaphoreTake(lock_, portMAX_DELAY);
    fn(tracked_);
    xSemaphoreGive(lock_);
}

FetchStatus AircraftManager::Status()
{
    xSemaphoreTake(lock_, portMAX_DELAY);
    FetchStatus copy = status_;
    xSemaphoreGive(lock_);
    return copy;
}

std::vector<AircraftInfo> AircraftManager::Snapshot(double centreLat, double centreLon)
{
    std::vector<AircraftInfo> out;
    const uint32_t now = Millis();
    xSemaphoreTake(lock_, portMAX_DELAY);
    out.reserve(tracked_.size());
    for (const auto& [icao, t] : tracked_) {
        auto [lat, lon] = t.GetDisplayPosition();
        AircraftInfo a;
        a.icao24 = t.state.icao24;
        a.callsign = t.state.callsign;
        a.country = t.state.originCountry;
        a.squawk = t.state.squawk;
        a.lat = lat;
        a.lon = lon;
        a.altitude = t.state.baroAltitude;
        a.velocity = t.state.velocity;
        a.track = t.state.trueTrack;
        a.verticalRate = t.state.verticalRate;
        a.category = t.state.category;
        a.onGround = t.state.onGround;
        a.distanceKm = units::HaversineKm(centreLat, centreLon, lat, lon);
        a.bearingDeg = units::BearingDeg(centreLat, centreLon, lat, lon);
        a.ageMs = now - t.lastSeen;
        out.push_back(std::move(a));
    }
    xSemaphoreGive(lock_);
    std::sort(out.begin(), out.end(), [](const AircraftInfo& a, const AircraftInfo& b) { return a.distanceKm < b.distanceKm; });
    return out;
}

void AircraftManager::FetchTask(void* arg)
{
    auto* self = static_cast<AircraftManager*>(arg);
    double lastLat = NAN, lastLon = NAN, lastRadius = NAN;

    for (;;) {
        const auto s = settings::Current();
        uint32_t waitMs = 1000;

        if (s->lat != lastLat || s->lon != lastLon || s->radius != lastRadius) {
            // Location/radius changed (or first run): old aircraft no longer apply.
            if (!std::isnan(lastLat)) ESP_LOGI(TAG, "location changed, clearing aircraft");
            self->WithAircraft([](auto& m) { m.clear(); });
            lastLat = s->lat;
            lastLon = s->lon;
            lastRadius = s->radius;
        }

        if (!s->locationSet) {
            // Nothing sensible to ask for until a location is configured.
        } else if (!net::Connected()) {
            xSemaphoreTake(self->lock_, portMAX_DELAY);
            self->status_.lastError = "WiFi not connected";
            xSemaphoreGive(self->lock_);
            waitMs = 2000;
        } else {
            self->FetchOnce(*s);
            xSemaphoreTake(self->lock_, portMAX_DELAY);
            waitMs = self->status_.intervalMs;
            xSemaphoreGive(self->lock_);
        }

        // Sleep in slices so a location change is picked up promptly.
        const uint32_t until = Millis() + waitMs;
        while (static_cast<int32_t>(until - Millis()) > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            const auto now = settings::Current();
            if (now->lat != lastLat || now->lon != lastLon || now->radius != lastRadius) break;
        }
    }
}

void AircraftManager::FetchOnce(const settings::Settings& s)
{
    const std::string token = opensky_.GetValidToken(s.openskyId, s.openskySecret);
    const uint32_t attempt = Millis();
    FetchResult r = opensky_.FetchStates(s.lat, s.lon, s.radius, token);
    const uint32_t now = Millis();  // post-parse timestamp

    radar::PacingInput pacing;
    pacing.authenticated = !token.empty();
    pacing.userIntervalS = s.pollSeconds;
    pacing.creditsRemaining = r.creditsRemaining;
    pacing.secondsToReset = timekeeping::SecondsToUtcMidnight();
    uint32_t interval = radar::NextIntervalMs(pacing);
    if (r.httpStatus == 429) {
        // Out of credits: wait what the server says (or 10 min), not less than pacing.
        const uint32_t retry = r.retryAfterS > 0 ? static_cast<uint32_t>(r.retryAfterS) * 1000u : 600000u;
        interval = std::max(interval, retry);
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    status_.authenticated = !token.empty();
    status_.lastAttemptMs = attempt;
    status_.httpStatus = r.httpStatus;
    status_.latencyMs = r.latencyMs;
    if (r.creditsRemaining >= 0) status_.creditsRemaining = r.creditsRemaining;
    status_.intervalMs = interval;
    status_.nextFetchMs = now + interval;

    if (r.ok) {
        for (auto& ac : r.aircraft) {
            auto it = tracked_.find(ac.icao24);
            if (it == tracked_.end())
                tracked_.emplace(ac.icao24, TrackedAircraft{ac, now});
            else
                it->second.Update(ac, now);
        }
        // Remove any aircraft that disappeared from the feed
        for (auto it = tracked_.begin(); it != tracked_.end();) {
            const bool present = std::any_of(r.aircraft.begin(), r.aircraft.end(),
                                             [&](const Aircraft& ac) { return ac.icao24 == it->first; });
            it = present ? std::next(it) : tracked_.erase(it);
        }
        status_.haveData = true;
        status_.lastSuccessMs = now;
        status_.consecutiveErrors = 0;
        status_.lastError.clear();
        status_.total = static_cast<int>(tracked_.size());
        status_.airborne = static_cast<int>(std::count_if(tracked_.begin(), tracked_.end(),
                                                          [](const auto& kv) { return !kv.second.state.onGround; }));
    } else {
        status_.consecutiveErrors++;
        status_.lastError = r.error;
    }
    const FetchStatus st = status_;
    xSemaphoreGive(lock_);

    if (r.ok) {
        ESP_LOGI(TAG, "%d aircraft (%d airborne), %u ms, credits %d, next in %.1f s", st.total, st.airborne,
                 static_cast<unsigned>(st.latencyMs), st.creditsRemaining, interval / 1000.0f);
    } else {
        ESP_LOGW(TAG, "update failed: %s - keeping previous aircraft, retry in %.1f s", r.error.c_str(),
                 interval / 1000.0f);
    }
}
