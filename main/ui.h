// LVGL user interface (#36): swipeable pages (#33) - Radar / Nearby / Details /
// System - driven by the CST816D touch controller (#29) with radar gestures:
// tap to cycle brightness (#31), tap ring or swipe to zoom (#32), double-tap to
// reset the view (#35).
//
// The radar itself is still drawn by radar_view into an RGB565 frame; the UI
// shows it through an LVGL canvas with two frames swapped each render.
#pragma once

#include <cstdint>

class AircraftManager;
namespace alerts {
struct Alert;
}

namespace ui {

// Takes over the display from the direct-flush boot screens. Call once, after
// the last screens:: call.
bool Start(AircraftManager& aircraft);

bool RadarVisible();
// Zoom to draw with: the touch zoom if one is active, else the setting.
int Zoom(int settingZoom);

// Show an alert banner (#55); safe from any task.
void ShowAlert(const alerts::Alert& a);

// Frame the renderer should draw into next, and hand it to the display.
uint16_t* BackBuffer();
void PresentRadar();

}  // namespace ui
