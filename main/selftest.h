// Built-in hardware self-test (#94), reusing the diagnostic firmware's checks
// in a form that is safe while the radar runs. Started from the LCD System page
// or the web System tab; runs in its own task.
#pragma once

#include <string>
#include <vector>

class AircraftManager;

namespace selftest {

enum class Status { Pass, Warn, Fail, Skip };
const char* StatusName(Status s);

struct Result {
    std::string name;
    Status status;
    std::string detail;
};

void Init(AircraftManager& aircraft);
// Starts a run in the background; false if one is already running.
bool Start();
bool Running();
// Results of the current / last run (partial while running).
std::vector<Result> Results();

}  // namespace selftest
