// CST816D touch controller (#29). Bus setup, reset and register reads are the
// ones proven on this board by the hardware diagnostic firmware; the reported
// point is transformed by the touch calibration settings (swap / mirror).
#pragma once

namespace touch {

// I2C1 SDA 11 / SCL 7, RST 6, INT 12 (input only). Returns false if no chip answers.
bool Init();
bool Available();
// Latest point in display coordinates. Returns false (and leaves x/y) when not touched.
bool Read(int& x, int& y);

}  // namespace touch
