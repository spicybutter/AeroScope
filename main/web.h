// HTTP servers. Station mode: the web app + JSON API (settings, status).
// Setup mode: the Wi-Fi setup portal (replaces WiFiManager's portal).
#pragma once

class AircraftManager;
namespace alerts {
struct Alert;
}

namespace web {

void StartConfigServer(AircraftManager& aircraft);
void StartPortalServer();

// Queue an alert for the connected browsers (#57); safe from any task.
void PushAlert(const alerts::Alert& a);

}  // namespace web
