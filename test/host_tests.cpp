// Host unit tests for the pure logic in main/ (no ESP-IDF needed).
// Run: test/run_host_tests.sh  (builds with g++ in a Docker container)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../main/radar_logic.h"
#include "../main/alerts.h"
#include "../main/settings.h"
#include "../main/units.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

static std::string V(const char* key, const std::string& in, bool* ok = nullptr)
{
    std::string out, err;
    const bool r = settings::Validate(*settings::Find(key), in, out, err);
    if (ok) *ok = r;
    return r ? out : "ERR:" + err;
}

static void TestSchema()
{
    // Every key fits NVS (15 chars) and has a valid default
    for (int i = 0; i < settings::Count(); i++) {
        const auto& d = settings::Defs()[i];
        CHECK(std::string(d.key).size() <= 15);
        std::string out, err;
        const bool defOk = settings::Validate(d, d.def, out, err);
        if (!defOk) printf("  default of %s invalid: %s\n", d.key, err.c_str());
        CHECK(defOk);
        for (int j = i + 1; j < settings::Count(); j++) CHECK(std::string(d.key) != settings::Defs()[j].key);
    }
    // The Arduino build's keys are all still present
    for (const char* k : {"latitude", "longitude", "radius", "opensky-id", "opensky-secret", "scanline", "infotext",
                          "triangle"})
        CHECK(settings::Find(k) != nullptr);
    CHECK(settings::Find("nope") == nullptr);
}

static void TestValidate()
{
    bool ok;
    CHECK(V("scanline", "on") == "true");
    CHECK(V("scanline", "0") == "false");
    CHECK(V("scanline", "") == "false");
    V("scanline", "maybe", &ok);
    CHECK(!ok);

    CHECK(V("fps", "10") == "10");
    CHECK(V("fps", "99") == "30");  // clamped to max
    CHECK(V("fps", "1") == "5");    // clamped to min
    V("fps", "12.5", &ok);
    CHECK(!ok);
    V("fps", "", &ok);
    CHECK(!ok);

    CHECK(V("latitude", "51.5074") == "51.5074");
    CHECK(V("latitude", "") == "");  // optional
    V("latitude", "91", &ok);
    CHECK(!ok);
    V("latitude", "abc", &ok);
    CHECK(!ok);
    V("radius", "3", &ok);
    CHECK(!ok);
    CHECK(V("radius", "2.0") == "2.0");

    CHECK(V("u_spd", "kt") == "kt");
    V("u_spd", "furlongs", &ok);
    CHECK(!ok);
    CHECK(V("zoom", "4") == "4");
    V("zoom", "3", &ok);
    CHECK(!ok);

    V("tz", "", &ok);
    CHECK(!ok);  // required
    V("tz", std::string(64, 'x'), &ok);
    CHECK(!ok);  // too long
}

static void TestParse()
{
    // Nothing saved: upstream defaults
    settings::Settings d = settings::Parse({});
    CHECK(!d.locationSet);
    NEAR(d.radius, 1.0, 1e-9);
    CHECK(d.scanline && d.infoText && d.triangles && d.trails && d.splash && d.showClock);
    CHECK(d.fps == 30 && d.brightness == 75 && d.zoom == 1 && d.trailSeconds == 60 && d.pollSeconds == 0);
    CHECK(d.altUnit == settings::AltUnit::M && d.speedUnit == settings::SpeedUnit::Ms);
    CHECK(d.timezone == "UTC0" && d.ntpServer == "pool.ntp.org");

    // Values exactly as the Arduino build stored them
    settings::Settings a = settings::Parse({{"latitude", "51.47"}, {"longitude", "-0.4543"}, {"radius", "0.5"},
                                            {"scanline", "false"}, {"infotext", "true"}, {"triangle", "false"}});
    CHECK(a.locationSet);
    NEAR(a.lat, 51.47, 1e-9);
    NEAR(a.lon, -0.4543, 1e-9);
    NEAR(a.radius, 0.5, 1e-9);
    CHECK(!a.scanline && a.infoText && !a.triangles);

    settings::Settings u = settings::Parse({{"u_alt", "ft"}, {"u_spd", "mph"}, {"u_dist", "nm"}, {"zoom", "2"},
                                            {"radius", "0"}, {"latitude", "10"}});
    CHECK(u.altUnit == settings::AltUnit::Ft && u.speedUnit == settings::SpeedUnit::Mph &&
          u.distUnit == settings::DistUnit::Nm && u.zoom == 2);
    NEAR(u.radius, 1.0, 1e-9);  // invalid radius falls back
    CHECK(!u.locationSet);      // longitude missing

    CHECK(settings::Parse({{"zoom", "7"}}).zoom == 1);
    CHECK(settings::Parse({{"fps", "garbage"}}).fps == 30);
}

static void TestUnits()
{
    using namespace settings;
    NEAR(units::Altitude(10000, AltUnit::Ft), 32808.4, 0.1);
    NEAR(units::Altitude(10000, AltUnit::M), 10000, 1e-9);
    NEAR(units::Speed(100, SpeedUnit::Kt), 194.384, 0.001);
    NEAR(units::Speed(100, SpeedUnit::Kmh), 360, 1e-9);
    NEAR(units::Speed(100, SpeedUnit::Mph), 223.694, 0.001);
    NEAR(units::Distance(100, DistUnit::Mi), 62.137, 0.001);
    NEAR(units::Distance(100, DistUnit::Nm), 53.996, 0.001);
    NEAR(units::VerticalRate(5.08, AltUnit::Ft), 1000, 0.1);
    CHECK(units::Format(231.45, "m/s") == "231m/s");
    CHECK(units::Format(10668.6, "m") == "10669m");
    CHECK(std::string(units::SpeedSuffix(SpeedUnit::Kt)) == "kt");

    // London Heathrow -> Paris CDG
    const double km = units::HaversineKm(51.4700, -0.4543, 49.0097, 2.5479);
    NEAR(km, 347.0, 3.0);
    const double brg = units::BearingDeg(51.4700, -0.4543, 49.0097, 2.5479);
    NEAR(brg, 141.0, 3.0);
    NEAR(units::BearingDeg(0, 0, 1, 0), 0.0, 1e-6);
    NEAR(units::BearingDeg(0, 0, 0, 1), 90.0, 1e-6);
}

static void TestColoursAndShapes()
{
    CHECK(radar::Rgb(255, 255, 255) == 0xFFFF);
    CHECK(radar::Rgb(0, 0, 0) == 0);
    CHECK(radar::Rgb(255, 0, 0) == 0xF800);
    CHECK(radar::Scale(0xFFFF, 0.0f) == 0);
    CHECK(radar::AltitudeColor(-50) == radar::Rgb(255, 110, 0));      // below ramp = first stop
    CHECK(radar::AltitudeColor(20000) == radar::Rgb(200, 80, 255));   // above ramp = last stop
    CHECK(radar::AltitudeColor(500) != radar::AltitudeColor(9000));

    CHECK(radar::IsEmergencySquawk("7700") && radar::IsEmergencySquawk("7600") && radar::IsEmergencySquawk("7500"));
    CHECK(!radar::IsEmergencySquawk("1200") && !radar::IsEmergencySquawk(""));
    CHECK(std::string(radar::EmergencyName("7600")) == "RADIO FAIL");

    CHECK(radar::ShapeForCategory(8) == radar::Shape::Rotorcraft);
    CHECK(radar::ShapeForCategory(6) == radar::Shape::Heavy);
    CHECK(radar::ShapeForCategory(2) == radar::Shape::Light);
    CHECK(radar::ShapeForCategory(0) == radar::Shape::Default);
    CHECK(radar::ShapeForCategory(14) == radar::Shape::Drone);
}

static void TestPacing()
{
    // Upstream budget: 86.4e6 / 3997 and / 397
    CHECK(radar::BudgetIntervalMs(true) == 21616);
    CHECK(radar::BudgetIntervalMs(false) == 217632);

    radar::PacingInput in;
    in.authenticated = true;
    CHECK(radar::NextIntervalMs(in) == 21616);
    in.userIntervalS = 10;
    CHECK(radar::NextIntervalMs(in) == 10000);
    in.userIntervalS = 1;
    CHECK(radar::NextIntervalMs(in) == 5000);  // floor

    // Plenty of credits: user interval wins
    in.userIntervalS = 30;
    in.creditsRemaining = 3000;
    in.secondsToReset = 3600;
    CHECK(radar::NextIntervalMs(in) == 30000);

    // Few credits left: stretched to last until the reset
    in.creditsRemaining = 103;  // 100 usable
    in.secondsToReset = 36000;
    CHECK(radar::NextIntervalMs(in) == 360000);

    // Exhausted: wait for the reset
    in.creditsRemaining = 0;
    in.secondsToReset = 1234;
    CHECK(radar::NextIntervalMs(in) == 1234000);

    // Unknown time: no stretching
    in.creditsRemaining = 0;
    in.secondsToReset = -1;
    CHECK(radar::NextIntervalMs(in) == 30000);
}

static void TestBatch2Settings()
{
    bool ok;
    // IPv4
    CHECK(V("ip", "192.168.1.50") == "192.168.1.50");
    CHECK(V("ip", "") == "");
    V("ip", "192.168.1.256", &ok);
    CHECK(!ok);
    V("ip", "192.168.1", &ok);
    CHECK(!ok);
    V("ip", "1.2.3.4x", &ok);
    CHECK(!ok);

    // Points of interest
    std::vector<settings::Poi> pois;
    std::string err;
    CHECK(settings::ParsePois("LHR,51.4700,-0.4543; Home,51.5,-0.12 ;", pois, err));
    CHECK(pois.size() == 2 && pois[0].name == "LHR" && pois[1].name == "Home");
    NEAR(pois[0].lat, 51.47, 1e-9);
    NEAR(pois[1].lon, -0.12, 1e-9);
    CHECK(settings::ParsePois("", pois, err) && pois.empty());
    CHECK(!settings::ParsePois("Nowhere,95,0", pois, err));   // latitude out of range
    CHECK(!settings::ParsePois("NoCoords", pois, err));
    CHECK(!settings::ParsePois("A,1", pois, err));
    CHECK(!settings::ParsePois("A,1,x", pois, err));
    CHECK(!settings::ParsePois(",1,2", pois, err));           // empty name
    CHECK(settings::ParsePois("AVeryLongPlaceName,1,2", pois, err) && pois[0].name.size() == 12);
    V("pois", "Bad entry", &ok);
    CHECK(!ok);
    CHECK(V("pois", "A,1,2") == "A,1,2");

    // Parsed into the snapshot
    settings::Settings s = settings::Parse({{"pois", "A,1,2;B,3,4"}, {"static_ip", "true"}, {"ip", "10.0.0.5"}});
    CHECK(s.pois.size() == 2 && s.showPois && s.qrCodes && s.staticIp && s.ip == "10.0.0.5");
    CHECK(s.netmask == "255.255.255.0");  // schema default
    CHECK(!settings::Parse({}).staticIp);
    CHECK(settings::Parse({{"pois", "garbage"}}).pois.empty());  // invalid stored value ignored

    // Batch 3 touch options: gestures on, calibration off by default
    settings::Settings t = settings::Parse({});
    CHECK(t.tapBrightness && t.tapZoom && t.doubleTapReset);
    CHECK(!t.touchSwapXY && !t.touchMirrorX && !t.touchMirrorY);
    settings::Settings t2 = settings::Parse({{"touch_mx", "true"}, {"tap_bright", "false"}});
    CHECK(t2.touchMirrorX && !t2.tapBrightness && t2.tapZoom);
}

static void TestAlerts()
{
    // Watchlist parsing / matching
    auto w = settings::ParseWatchlist(" baw1, ezy;4ca2b6\n");
    CHECK(w.size() == 3 && w[0] == "BAW1" && w[1] == "EZY" && w[2] == "4CA2B6");
    CHECK(settings::WatchMatches(w, "400abc", "BAW123"));   // callsign prefix
    CHECK(settings::WatchMatches(w, "4ca2b6", ""));         // ICAO24, case-insensitive
    CHECK(settings::WatchMatches(w, "400abc", "ezy45"));    // lower-case callsign
    CHECK(!settings::WatchMatches(w, "400abc", "RYR1"));
    CHECK(!settings::WatchMatches({}, "4ca2b6", "BAW1"));
    bool ok;
    V("watchlist", "TOOLONGCALLSIGN", &ok);
    CHECK(!ok);
    CHECK(V("watchlist", "BAW EZY") == "BAW EZY");
    settings::Settings s = settings::Parse({{"watchlist", "baw1 dlh"}});
    CHECK(s.watchlist.size() == 2 && s.sound && s.volume == 80 && s.alertEmergency && s.alertWatch && s.banners);

    // Detector
    using alerts::Seen;
    alerts::Detector d;
    const uint32_t R = 30 * 60 * 1000;
    std::vector<Seen> t0 = {{"aaa111", "BAW1", "7700", false}, {"bbb222", "RYR1", "1200", false}};
    auto r = d.Poll(t0, w, true, true, 1000, R);
    CHECK(r.size() == 2);  // emergency + watched BAW1 appearing
    CHECK(std::find(r.begin(), r.end(), "E:aaa111:7700") != r.end());
    CHECK(std::find(r.begin(), r.end(), "W:aaa111") != r.end());

    r = d.Poll(t0, w, true, true, 3000, R);
    CHECK(r.empty());  // same aircraft, same squawk: no repeat

    std::vector<Seen> t1 = {{"bbb222", "RYR1", "1200", false}};  // BAW1 leaves
    CHECK(d.Poll(t1, w, true, true, 5000, R).empty());
    r = d.Poll(t0, w, true, true, 7000, R);   // BAW1 back, within the repeat window
    CHECK(r.empty());
    d.Poll(t1, w, true, true, 7000 + R, R);   // gone again...
    r = d.Poll(t0, w, true, true, 9000 + R, R);  // ...back after the window: both fire again
    CHECK(r.size() == 2);

    // Squawk change on the same aircraft is a new alert
    std::vector<Seen> t2 = {{"aaa111", "BAW1", "7600", false}};
    r = d.Poll(t2, w, true, true, 10000 + R, R);
    CHECK(r.size() == 1 && r[0] == "E:aaa111:7600");

    // Switches and ground aircraft
    alerts::Detector d2;
    CHECK(d2.Poll(t0, w, false, false, 0, R).empty());
    std::vector<Seen> ground = {{"ccc333", "BAW9", "7700", true}};
    CHECK(alerts::Detector().Poll(ground, w, true, true, 0, R).empty());
}

int main()
{
    TestSchema();
    TestAlerts();
    TestBatch2Settings();
    TestValidate();
    TestParse();
    TestUnits();
    TestColoursAndShapes();
    TestPacing();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
