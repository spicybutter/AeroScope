// OpenSky Network client: OAuth2 client-credentials token + /states/all area query.
// Port of HttpRequestManager + OpenSkyAuthTokenHandler + the fetch part of
// AircraftManager::Update(). TLS certificates are verified with the ESP-IDF CA
// bundle. Rate-limit headers are captured for pacing (#72).
#pragma once

#include <string>
#include <vector>

#include "aircraft.h"

struct FetchResult {
    bool ok = false;              // response parsed; `aircraft` is the full current set
    int httpStatus = 0;           // 0 = transport error
    int creditsRemaining = -1;    // X-Rate-Limit-Remaining, -1 if absent
    int retryAfterS = -1;         // X-Rate-Limit-Retry-After-Seconds (on 429), -1 if absent
    uint32_t latencyMs = 0;
    std::string error;            // human-readable, empty when ok
    std::vector<Aircraft> aircraft;
};

class OpenSkyClient {
public:
    OpenSkyClient();

    // Cached bearer token (refreshed after 29 min), or "" when no credentials
    // are configured or the token request fails.
    std::string GetValidToken(const std::string& clientId, const std::string& clientSecret);

    // GET /api/states/all?extended=1 for lat/lon +- radius.
    FetchResult FetchStates(double lat, double lon, double radius, const std::string& token);

private:
    std::string FetchBearerToken(const std::string& clientId, const std::string& clientSecret);

    std::string bearerToken_;
    uint32_t tokenExpiry_ = 0;  // Millis() timestamp
};
