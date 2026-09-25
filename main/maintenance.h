// Deferred resets (#64). A reset requested from the web page is recorded in RTC
// memory, the device restarts, and the erase runs early in the next boot -
// before Wi-Fi or anything else holds NVS open.
#pragma once

namespace maintenance {

enum class ResetScope { None = 0, Settings, Wifi, All };

// Record the request and restart ~1 s later.
void RequestReset(ResetScope scope);
// Call right after nvs_flash_init(); performs a pending reset (and clears it).
// Returns true if NVS was erased entirely (caller must re-init NVS).
bool ApplyPendingReset();

}  // namespace maintenance
