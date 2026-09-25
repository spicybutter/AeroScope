// Wi-Fi: several remembered networks (#74), static IP (#75), setup hotspot
// fallback, automatic reconnect.
#pragma once

#include <string>
#include <vector>

namespace net {

constexpr const char* AP_SSID = "AeroScope-Setup";    // was "MicroRadar-Setup" before the rename
constexpr const char* HOSTNAME = "aeroscope";  // aeroscope.local
constexpr int MAX_SAVED = 8;

enum class Mode { Station, SetupPortal };

// Blocks until connected to one of the saved networks (Station), or starts the
// setup hotspot when none is saved / reachable (SetupPortal).
Mode Start();

bool Connected();
bool PortalActive();
std::string Ip();
std::string ApIp();
std::string Ssid();
int Rssi();
void PrintInfo();

struct ScanResult {
    std::string ssid;
    int rssi;
    bool secure;
};
std::vector<ScanResult> Scan();
// Compatibility helper for the portal page (strongest first, unique).
std::vector<std::string> ScanSsids();

// Saved networks, highest priority first.
struct SavedNetwork {
    std::string ssid;
    std::string password;
};
std::vector<SavedNetwork> SavedNetworks();
bool SaveNetworks(const std::vector<SavedNetwork>& list);
// Add (or update) a network and move it to the front.
bool SaveCredentials(const std::string& ssid, const std::string& password);
bool ForgetNetwork(const std::string& ssid);

}  // namespace net
