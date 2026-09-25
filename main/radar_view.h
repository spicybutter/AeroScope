// Radar frame renderer: sweep, rings, trails, aircraft symbols/labels and the
// status overlays. Every layer is driven by the settings snapshot.
#pragma once

#include "aircraft.h"
#include "gfx.h"
#include "settings.h"

namespace radar_view {

// Draws one frame into the canvas. `zoom` overrides s.zoom (touch zoom, #32).
// `flush` pushes it straight to the panel (only before LVGL runs).
void DrawFrame(gfx::Canvas& c, AircraftManager& aircraft, const settings::Settings& s, int zoom, bool flush);
// Boot splash (#27), blocks for ~1.5 s.
void Splash(gfx::Canvas& c);

}  // namespace radar_view
