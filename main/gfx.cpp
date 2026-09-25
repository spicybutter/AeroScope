#include "gfx.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "board.h"
#include "esp_heap_caps.h"
#include "font5x7.h"

namespace gfx {

static constexpr int STRIP_LINES = 40;

bool Canvas::Init()
{
    // Frame in PSRAM (115 KB); only the flush strip needs internal DMA memory.
    buf_ = static_cast<uint16_t*>(heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf_) buf_ = static_cast<uint16_t*>(heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_8BIT));
    strip_ = static_cast<uint16_t*>(heap_caps_malloc(W * STRIP_LINES * sizeof(uint16_t),
                                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!buf_ || !strip_) return false;
    Fill(0);
    return true;
}

void Canvas::Fill(uint16_t c)
{
    if (c == 0) {
        memset(buf_, 0, W * H * sizeof(uint16_t));
    } else {
        std::fill(buf_, buf_ + W * H, c);
    }
}

void Canvas::HLine(int x0, int x1, int y, uint16_t c)
{
    if ((unsigned)y >= H) return;
    if (x0 > x1) std::swap(x0, x1);
    x0 = std::max(x0, 0);
    x1 = std::min(x1, W - 1);
    if (x0 > x1) return;
    std::fill(&buf_[y * W + x0], &buf_[y * W + x1 + 1], c);
}

void Canvas::FillRect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++) HLine(x, x + w - 1, j, c);
}

void Canvas::Line(int x0, int y0, int x1, int y1, uint16_t c)
{
    // Bresenham; off-screen pixels are clipped individually (sweep lines extend past the edge).
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        Pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Canvas::Circle(int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        Pixel(cx + x, cy + y, c); Pixel(cx + y, cy + x, c);
        Pixel(cx - y, cy + x, c); Pixel(cx - x, cy + y, c);
        Pixel(cx - x, cy - y, c); Pixel(cx - y, cy - x, c);
        Pixel(cx + y, cy - x, c); Pixel(cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void Canvas::FillCircle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        HLine(cx - dx, cx + dx, cy + dy, c);
    }
}

void Canvas::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    // Sort by y, then scan-convert the two halves.
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
    if (y1 > y2) { std::swap(y1, y2); std::swap(x1, x2); }
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }

    if (y0 == y2) {  // degenerate: single row
        HLine(std::min({x0, x1, x2}), std::max({x0, x1, x2}), y0, c);
        return;
    }
    for (int y = y0; y <= y2; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        // Upper half uses edge 0-1 (y1 > y0 is implied by y < y1), lower half edge 1-2.
        int xb = (y < y1) ? x0 + (x1 - x0) * (y - y0) / (y1 - y0)
               : (y2 == y1) ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        HLine(xa, xb, y, c);
    }
}

void Canvas::DrawString(const std::string& s, int x, int y, uint16_t c)
{
    const int sz = text_size_;
    for (char ch : s) {
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const uint8_t* g = kFont5x7[ch - FONT_FIRST];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 8; row++) {
                if (!((bits >> row) & 1)) continue;
                for (int py = 0; py < sz; py++)
                    for (int px = 0; px < sz; px++)
                        Pixel(x + col * sz + px, y + row * sz + py, c);
            }
        }
        x += FONT_W * sz;
    }
}

void Canvas::DrawCentreString(const std::string& s, int cx, int y, uint16_t c)
{
    DrawString(s, cx - TextWidth(s) / 2, y, c);
}

void Canvas::ReleaseFlushBuffer()
{
    heap_caps_free(strip_);
    strip_ = nullptr;
}

void Canvas::Flush()
{
    if (!strip_) return;
    for (int y = 0; y < H; y += STRIP_LINES) {
        const int lines = std::min(STRIP_LINES, H - y);
        const uint16_t* src = &buf_[y * W];
        for (int i = 0; i < lines * W; i++) strip_[i] = __builtin_bswap16(src[i]);  // SPI sends MSB first
        board::LcdPushRows(y, lines, strip_);
    }
}

}  // namespace gfx
