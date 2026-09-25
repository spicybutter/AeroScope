// Full-screen text views (Connecting / Setup / Connected), reproducing the
// Arduino build's layout, optionally with QR codes (#26).
// The radar itself is drawn by radar_view.
#pragma once

#include <string>

#include "gfx.h"

namespace screens {

void Connecting(gfx::Canvas& c);
void Setup(gfx::Canvas& c, bool qr);
void Connected(gfx::Canvas& c, bool qr);

// Draw `text` as a QR code (dark modules on a white quiet zone) centred at cx,
// top edge at y, at most `maxSize` px wide. Returns the drawn size, 0 on failure.
int DrawQr(gfx::Canvas& c, const std::string& text, int cx, int y, int maxSize);

}  // namespace screens
