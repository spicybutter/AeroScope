// 240x240 RGB565 software canvas (PSRAM) with the few primitives the radar
// needs. Colours are native RGB565 (see radar::Rgb); Flush() byte-swaps into
// internal DMA strips for the GC9A01.
//
// First ESP-IDF build used RGB332 to mirror the Arduino 8-bit sprite; batch 1
// moved to RGB565 for altitude colours and fading trails.
#pragma once

#include <cstdint>
#include <string>

#include "radar_logic.h"

namespace gfx {

// lgfx::color888() equivalent
constexpr uint16_t Color888(uint8_t r, uint8_t g, uint8_t b) { return radar::Rgb(r, g, b); }

class Canvas {
public:
    static constexpr int W = 240;
    static constexpr int H = 240;
    static constexpr int FONT_W = 6;   // 5x7 glyph in a 6x8 cell (LovyanGFX Font0 shapes)
    static constexpr int FONT_H = 8;

    bool Init();
    uint16_t* Buffer() { return buf_; }
    // Render into an external W*H RGB565 frame instead (LVGL double buffering).
    void SetBuffer(uint16_t* buf) { buf_ = buf; }

    void Fill(uint16_t c);
    void Pixel(int x, int y, uint16_t c)
    {
        if ((unsigned)x < W && (unsigned)y < H) buf_[y * W + x] = c;
    }
    void FillRect(int x, int y, int w, int h, uint16_t c);
    void Line(int x0, int y0, int x1, int y1, uint16_t c);
    void Circle(int cx, int cy, int r, uint16_t c);
    void FillCircle(int cx, int cy, int r, uint16_t c);
    void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);

    void SetTextSize(int s) { text_size_ = s < 1 ? 1 : s; }
    int FontHeight() const { return FONT_H * text_size_; }
    int TextWidth(const std::string& s) const { return (int)s.size() * FONT_W * text_size_; }
    // Top-left datum, transparent background (LovyanGFX drawString default).
    void DrawString(const std::string& s, int x, int y, uint16_t c);
    // Top-centre datum (LovyanGFX drawCentreString).
    void DrawCentreString(const std::string& s, int cx, int y, uint16_t c);

    // Push the whole frame to the panel.
    void Flush();
    // Free the internal DMA strip once LVGL owns the display (Flush() then does nothing).
    void ReleaseFlushBuffer();

private:
    void HLine(int x0, int x1, int y, uint16_t c);

    uint16_t* buf_ = nullptr;
    uint16_t* strip_ = nullptr;
    int text_size_ = 1;
};

}  // namespace gfx
