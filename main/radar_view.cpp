#include "radar_view.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "radar_logic.h"
#include "timekeeping.h"
#include "units.h"

namespace radar_view {

static constexpr int SCREEN_SIZE = 240;
static constexpr int SCREEN_SIZE_DIV_2 = SCREEN_SIZE / 2;
static constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;

static constexpr uint16_t GREEN = radar::Rgb(0, 255, 0);
static constexpr uint16_t LABEL = radar::Rgb(0, 128, 0);
static constexpr uint16_t DIM = radar::Rgb(0, 150, 0);
static constexpr uint16_t YELLOW = radar::Rgb(255, 200, 0);
static constexpr uint16_t RED = radar::Rgb(255, 40, 40);
static constexpr uint16_t WHITE = radar::Rgb(230, 230, 230);

// ---------------------------------------------------------------------------
// Upstream pieces (DrawHelpers.h, AircraftManager.cpp)

static void DrawScanLines(gfx::Canvas& c, int x0, int y0, int x1, int y1, int thickness, int trailBrightness,
                          int spacing)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float px = -dy / len;  // perpendicular unit vector
    const float py = dx / len;
    for (int i = 0; i <= thickness; i++) {
        const float t = i / static_cast<float>(thickness);  // 1.0 at centre, 0.0 at edges
        const uint8_t brightness = static_cast<uint8_t>(t * trailBrightness);
        c.Line(x0, y0, static_cast<int>(x1 + px * (i * spacing)), static_cast<int>(y1 + py * (i * spacing)),
               radar::Rgb(0, brightness, 0));
    }
    c.Line(x0, y0, static_cast<int>(x1 + px * (thickness * spacing)),
           static_cast<int>(y1 + py * (thickness * spacing)), radar::Rgb(0, 200, 0));
}

static void DrawRadarCircles(gfx::Canvas& c)
{
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 1;
    c.Circle(CENTRE, CENTRE, OUTER, radar::Rgb(0, 200, 0));
    c.Circle(CENTRE, CENTRE, (OUTER / 3) * 2, radar::Rgb(0, 64, 0));
    c.Circle(CENTRE, CENTRE, OUTER / 3, radar::Rgb(0, 32, 0));
}

// Linear projection kept identical to upstream; the zoom only shrinks the shown radius.
struct Projection {
    float lat, lon, rad;
    std::pair<int, int> operator()(float pLat, float pLon) const
    {
        const float normLon = (pLon - lon + rad) / (2.0f * rad);
        const float normLat = (pLat - lat + rad) / (2.0f * rad);
        return {static_cast<int>(normLon * SCREEN_SIZE), static_cast<int>(SCREEN_SIZE - normLat * SCREEN_SIZE)};
    }
};

// ---------------------------------------------------------------------------
// Aircraft symbols (#19). Shapes are triangles in a local frame: u = right,
// v = forward (along the track), rotated onto the screen.

struct Frame {
    float x, y, fx, fy, rx, ry;  // origin, forward unit, right unit (screen coords)
    int X(float u, float v) const { return static_cast<int>(std::lround(x + u * rx + v * fx)); }
    int Y(float u, float v) const { return static_cast<int>(std::lround(y + u * ry + v * fy)); }
};

static void Tri(gfx::Canvas& c, const Frame& f, float s, float u0, float v0, float u1, float v1, float u2, float v2,
                uint16_t col)
{
    c.FillTriangle(f.X(u0 * s, v0 * s), f.Y(u0 * s, v0 * s), f.X(u1 * s, v1 * s), f.Y(u1 * s, v1 * s),
                   f.X(u2 * s, v2 * s), f.Y(u2 * s, v2 * s), col);
}

static void DrawPlane(gfx::Canvas& c, const Frame& f, float s, float wing, uint16_t col)
{
    Tri(c, f, s, 0, 5, -1, -4, 1, -4, col);                 // fuselage
    Tri(c, f, s, -wing, 0.5f, wing, 0.5f, 0, 2.5f, col);    // wings
    Tri(c, f, s, -2, -4, 2, -4, 0, -2.5f, col);             // tail
}

static void DrawSymbol(gfx::Canvas& c, int x, int y, const TrackedAircraft& t, const settings::Settings& s,
                       uint16_t col)
{
    if (!s.triangles) {
        c.FillCircle(x, y, 3, col);  // upstream "directional aircraft" off
        return;
    }
    const float h = Radians(t.state.trueTrack);
    const Frame f{static_cast<float>(x), static_cast<float>(y), std::sin(h), -std::cos(h), std::cos(h), std::sin(h)};

    const radar::Shape shape = s.categoryIcons ? radar::ShapeForCategory(t.state.category) : radar::Shape::Default;
    switch (shape) {
    case radar::Shape::Light: DrawPlane(c, f, 0.9f, 4.5f, col); break;
    case radar::Shape::Large: DrawPlane(c, f, 1.2f, 5.0f, col); break;
    case radar::Shape::Heavy: DrawPlane(c, f, 1.5f, 5.5f, col); break;
    case radar::Shape::Glider:
        Tri(c, f, 1, 0, 4, -0.7f, -4, 0.7f, -4, col);
        Tri(c, f, 1, -8, 0.5f, 8, 0.5f, 0, 1.5f, col);
        break;
    case radar::Shape::Rotorcraft: {
        c.FillCircle(x, y, 2, col);
        const float a = Millis() / 60.0f;  // spinning rotor
        for (int k = 0; k < 2; k++) {
            const float ca = std::cos(a + k * M_PI / 2) * 6, sa = std::sin(a + k * M_PI / 2) * 6;
            c.Line(static_cast<int>(x - ca), static_cast<int>(y - sa), static_cast<int>(x + ca),
                   static_cast<int>(y + sa), col);
        }
        c.Line(x, y, f.X(0, -7), f.Y(0, -7), col);  // tail boom
        break;
    }
    case radar::Shape::Balloon:
        c.Circle(x, y - 2, 4, col);
        c.FillRect(x - 1, y + 3, 3, 2, col);
        break;
    case radar::Shape::Drone:
        c.Line(x - 4, y - 4, x + 4, y + 4, col);
        c.Line(x - 4, y + 4, x + 4, y - 4, col);
        for (int dx : {-4, 4})
            for (int dy : {-4, 4}) c.Circle(x + dx, y + dy, 2, col);
        break;
    case radar::Shape::Default:
    default: {
        // Upstream triangle: length 6, width 3, tip ahead of the position.
        constexpr float L = 6.0f, W = 3.0f;
        Tri(c, f, 1, 0, L, W * 0.5f, -L * 0.5f, -W * 0.5f, -L * 0.5f, col);
        break;
    }
    }
}

static void DrawVerticalArrow(gfx::Canvas& c, int x, int y, bool up, uint16_t col)
{
    // 5 px wide arrow in the 6x8 text cell starting at (x, y)
    if (up) {
        c.FillTriangle(x + 2, y, x, y + 3, x + 4, y + 3, col);
        c.FillRect(x + 1, y + 3, 3, 4, col);
    } else {
        c.FillRect(x + 1, y, 3, 4, col);
        c.FillTriangle(x + 2, y + 7, x, y + 4, x + 4, y + 4, col);
    }
}

static void DrawTrail(gfx::Canvas& c, const TrackedAircraft& t, const Projection& proj, int headX, int headY,
                      uint16_t col, const settings::Settings& s, uint32_t now)
{
    const uint32_t maxAge = static_cast<uint32_t>(s.trailSeconds) * 1000u;
    int px = -1, py = -1;
    for (int i = 0; i < t.trailCount; i++) {
        const TrailPoint* p = t.TrailAt(i);
        if (!p || now - p->t > maxAge) continue;
        auto [x, y] = proj(p->lat, p->lon);
        if (px >= 0) {
            const float f = s.trailFade ? 0.15f + 0.6f * (1.0f - static_cast<float>(now - p->t) / maxAge) : 0.6f;
            c.Line(px, py, x, y, radar::Scale(col, f));
        }
        px = x;
        py = y;
    }
    if (px >= 0) c.Line(px, py, headX, headY, radar::Scale(col, s.trailFade ? 0.75f : 0.6f));
}

// ---------------------------------------------------------------------------
// Overlays (#11, #12, #13, #14, #71, #4)

static void DrawWifiIcon(gfx::Canvas& c, int x, int y)
{
    // 3 bars, bottom-aligned at y+8
    const int rssi = net::Rssi();
    const bool up = net::Connected();
    const int bars = !up ? 0 : rssi > -60 ? 3 : rssi > -70 ? 2 : rssi > -80 ? 1 : 0;
    for (int b = 0; b < 3; b++) {
        const int h = 3 + b * 3;
        c.FillRect(x + b * 4, y + 9 - h, 3, h, b < bars ? GREEN : radar::Rgb(0, 60, 0));
    }
    if (!up) {
        c.Line(x, y, x + 10, y + 9, RED);
        c.Line(x, y + 9, x + 10, y, RED);
    }
}

static std::string AgeText(uint32_t ms)
{
    char buf[16];
    const uint32_t s = ms / 1000;
    if (s < 100) snprintf(buf, sizeof(buf), "%us", static_cast<unsigned>(s));
    else if (s < 6000) snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>(s / 60));
    else snprintf(buf, sizeof(buf), "%uh", static_cast<unsigned>(s / 3600));
    return buf;
}

static void DrawBanner(gfx::Canvas& c, const std::string& l1, const std::string& l2, uint16_t col)
{
    const int y = 150;
    c.FillRect(40, y, 160, 30, radar::Rgb(20, 0, 0));
    c.Line(40, y, 199, y, col);
    c.Line(40, y + 29, 199, y + 29, col);
    c.SetTextSize(1);
    c.DrawCentreString(l1, SCREEN_SIZE_DIV_2, y + 5, col);
    c.DrawCentreString(l2, SCREEN_SIZE_DIV_2, y + 17, WHITE);
}

static void DrawOverlays(gfx::Canvas& c, const settings::Settings& s, const FetchStatus& st, int airborne, int zoom,
                         uint32_t now)
{
    if (s.showClock) {
        const std::string clock = timekeeping::ClockText(s.clock24h);
        if (!clock.empty()) {
            c.SetTextSize(2);
            c.DrawCentreString(clock, SCREEN_SIZE_DIV_2, 12, DIM);
            c.SetTextSize(1);
        }
    }
    if (s.showWifi) DrawWifiIcon(c, 114, 32);

    // Bottom line 1: count + zoom
    std::string line;
    if (s.showCount) line = std::to_string(airborne) + " AC";
    if (zoom > 1) line += (line.empty() ? "" : "  ") + std::string("x") + std::to_string(zoom);
    c.SetTextSize(1);
    if (!line.empty()) c.DrawCentreString(line, SCREEN_SIZE_DIV_2, 206, DIM);

    // Bottom line 2: data status
    if (s.showStatus && s.locationSet) {
        std::string text;
        uint16_t col = DIM;
        if (!st.lastError.empty()) {
            text = st.lastError.size() > 18 ? st.lastError.substr(0, 18) : st.lastError;
            col = RED;
        } else if (st.haveData) {
            const uint32_t age = now - st.lastSuccessMs;
            text = "upd " + AgeText(age);
            if (age > st.intervalMs * 2 + 10000) col = YELLOW;
        } else {
            text = "waiting for data";
        }
        c.DrawCentreString(text, SCREEN_SIZE_DIV_2, 218, col);
    }

    // Offline / error banner (#71)
    if (!net::Connected()) {
        DrawBanner(c, "WIFI LOST", "reconnecting...", RED);
    } else if (s.locationSet && st.consecutiveErrors >= 3) {
        DrawBanner(c, "NO DATA", st.httpStatus == 429 ? "rate limited, waiting" : st.lastError, YELLOW);
    }
}

// ---------------------------------------------------------------------------

void DrawFrame(gfx::Canvas& c, AircraftManager& aircraft, const settings::Settings& s, int zoom, bool flush)
{
    const uint32_t now = Millis();
    c.Fill(0);

    if (s.scanline) {
        const float t = now / 3000.0f;
        DrawScanLines(c, CENTRE, CENTRE, static_cast<int>(CENTRE + std::cos(t) * SCREEN_SIZE_DIV_2),
                      static_cast<int>(CENTRE + std::sin(t) * SCREEN_SIZE_DIV_2), 20, 128, 5);
    }
    DrawRadarCircles(c);

    const Projection proj{static_cast<float>(s.lat), static_cast<float>(s.lon),
                          static_cast<float>(s.radius / zoom)};
    const bool blinkOn = (now / 400) % 2 == 0;
    int airborne = 0;

    // Points of interest (#21), under the aircraft
    if (s.showPois && s.locationSet) {
        const uint16_t poiCol = radar::Rgb(60, 200, 200);
        c.SetTextSize(1);
        for (const auto& p : s.pois) {
            auto [x, y] = proj(static_cast<float>(p.lat), static_cast<float>(p.lon));
            if (x < -20 || x > SCREEN_SIZE + 20 || y < -20 || y > SCREEN_SIZE + 20) continue;
            c.FillTriangle(x, y - 4, x - 4, y, x + 4, y, poiCol);
            c.FillTriangle(x, y + 4, x - 4, y, x + 4, y, poiCol);
            c.DrawString(p.name, x + 6, y - 3, radar::Scale(poiCol, 0.8f));
        }
    }

    aircraft.WithAircraft([&](std::map<std::string, TrackedAircraft>& tracked) {
        for (auto& [icao, t] : tracked) {
            if (t.state.onGround) continue;
            airborne++;

            t.Tick();
            auto [lat, lon] = t.GetDisplayPosition();
            t.SampleTrail(lat, lon, now, static_cast<uint32_t>(s.trailSeconds) * 1000u);
            auto [x, y] = proj(lat, lon);

            const bool emergency = s.emergencyHighlight && radar::IsEmergencySquawk(t.state.squawk);
            uint16_t col = s.altitudeColors ? radar::AltitudeColor(t.state.baroAltitude) : GREEN;
            if (emergency) col = RED;

            if (s.trails) DrawTrail(c, t, proj, x, y, col, s, now);
            if (emergency && blinkOn) c.Circle(x, y, 10, RED);
            DrawSymbol(c, x, y, t, s, col);

            if (s.infoText) {
                const int lh = c.FontHeight() + 1;
                c.SetTextSize(1);
                c.DrawString(t.state.callsign, x + 5, y + 5, emergency ? RED : LABEL);
                c.DrawString(units::Format(units::Speed(t.state.velocity, s.speedUnit), units::SpeedSuffix(s.speedUnit)),
                             x + 5, y + 5 + lh, LABEL);
                const std::string alt =
                    units::Format(units::Altitude(t.state.baroAltitude, s.altUnit), units::AltSuffix(s.altUnit));
                c.DrawString(alt, x + 5, y + 5 + lh * 2, LABEL);
                if (s.verticalArrows && std::fabs(t.state.verticalRate) >= 1.0f) {
                    DrawVerticalArrow(c, x + 6 + c.TextWidth(alt), y + 5 + lh * 2, t.state.verticalRate > 0,
                                      t.state.verticalRate > 0 ? radar::Rgb(0, 220, 255) : radar::Rgb(255, 150, 0));
                }
                if (emergency) c.DrawString(radar::EmergencyName(t.state.squawk), x + 5, y + 5 + lh * 3, RED);
            } else if (s.verticalArrows && std::fabs(t.state.verticalRate) >= 1.0f) {
                DrawVerticalArrow(c, x + 6, y - 4, t.state.verticalRate > 0,
                                  t.state.verticalRate > 0 ? radar::Rgb(0, 220, 255) : radar::Rgb(255, 150, 0));
            }
        }
    });

    if (!s.locationSet) {
        c.SetTextSize(1);
        c.DrawCentreString("SET LOCATION AT", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - 12, GREEN);
        c.DrawCentreString("http://" + net::Ip(), SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 + 4, GREEN);
    }

    DrawOverlays(c, s, aircraft.Status(), airborne, zoom, now);
    if (flush) c.Flush();
}

void Splash(gfx::Canvas& c)
{
    const uint32_t start = Millis();
    for (;;) {
        const uint32_t t = Millis() - start;
        if (t > 1500) break;
        c.Fill(0);
        const int r = static_cast<int>((t / 1500.0f) * 119);
        for (int k = 1; k <= 3; k++) {
            const int rr = r * k / 3;
            if (rr > 0) c.Circle(CENTRE, CENTRE, rr, radar::Rgb(0, static_cast<uint8_t>(60 * k), 0));
        }
        c.SetTextSize(2);
        c.DrawCentreString("AEROSCOPE", SCREEN_SIZE_DIV_2, 100, GREEN);
        c.SetTextSize(1);
        c.DrawCentreString("flight radar", SCREEN_SIZE_DIV_2, 124, DIM);
        c.Flush();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

}  // namespace radar_view
