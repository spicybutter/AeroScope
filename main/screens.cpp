#include "screens.h"

#include "net.h"
#include "qrcode.h"

namespace screens {

static constexpr int SCREEN_SIZE = 240;
static constexpr int SCREEN_SIZE_DIV_2 = SCREEN_SIZE / 2;
static constexpr uint16_t GREEN = gfx::Color888(0, 255, 0);
static constexpr uint16_t DIM = gfx::Color888(0, 150, 0);

void Connecting(gfx::Canvas& c)
{
    c.Fill(gfx::Color888(0, 0, 0));
    c.SetTextSize(1);
    c.DrawCentreString("Connecting to WiFi...", SCREEN_SIZE / 2, SCREEN_SIZE / 2, GREEN);
    c.Flush();
}

struct QrContext {
    gfx::Canvas* canvas;
    int cx, y, maxSize;
    int drawn;
};

static void DrawQrModules(esp_qrcode_handle_t qr, void* user)
{
    auto* ctx = static_cast<QrContext*>(user);
    const int n = esp_qrcode_get_size(qr);
    const int quiet = 2;
    const int scale = ctx->maxSize / (n + 2 * quiet);
    if (scale < 1) return;
    const int size = (n + 2 * quiet) * scale;
    const int x0 = ctx->cx - size / 2, y0 = ctx->y;
    ctx->canvas->FillRect(x0, y0, size, size, gfx::Color888(255, 255, 255));
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (esp_qrcode_get_module(qr, x, y))
                ctx->canvas->FillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, 0);
    ctx->drawn = size;
}

int DrawQr(gfx::Canvas& c, const std::string& text, int cx, int y, int maxSize)
{
    QrContext ctx{&c, cx, y, maxSize, 0};
    esp_qrcode_config_t cfg = {};
    cfg.display_func_with_cb = DrawQrModules;
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    cfg.user_data = &ctx;
    esp_qrcode_generate(&cfg, text.c_str());
    return ctx.drawn;
}

void Setup(gfx::Canvas& c, bool qr)
{
    // WiFiManagerHelpers AP callback layout + the hotspot address line
    c.Fill(gfx::Color888(0, 0, 0));
    c.SetTextSize(1);
    const int lineHeight = c.FontHeight() + (qr ? 6 : 10);
    const int top = qr ? 32 : SCREEN_SIZE / 2 - lineHeight;
    c.DrawCentreString("- SETUP -", SCREEN_SIZE / 2, top, GREEN);
    c.DrawCentreString("Connect to this WiFi hotspot:", SCREEN_SIZE / 2, top + lineHeight, GREEN);
    c.DrawCentreString(net::AP_SSID, SCREEN_SIZE / 2, top + lineHeight * 2, GREEN);
    c.DrawCentreString("then open " + net::ApIp(), SCREEN_SIZE / 2, top + lineHeight * 3, GREEN);
    if (qr) {
        // Standard Wi-Fi QR: phones offer to join the open hotspot.
        const int size = DrawQr(c, std::string("WIFI:T:nopass;S:") + net::AP_SSID + ";;", SCREEN_SIZE_DIV_2,
                                top + lineHeight * 4 + 2, 96);
        if (size) c.DrawCentreString("scan to join", SCREEN_SIZE_DIV_2, top + lineHeight * 4 + 6 + size, DIM);
    }
    c.Flush();
}

void Connected(gfx::Canvas& c, bool qr)
{
    c.Fill(gfx::Color888(0, 0, 0));
    c.SetTextSize(1);
    const std::string url = "http://" + net::Ip() + "/";
    if (!qr) {
        const int lineHeight = c.FontHeight() + 10;
        c.DrawCentreString("- CONNECTED -", SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight * 2, GREEN);
        c.DrawCentreString(net::Ssid(), SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2 - lineHeight, GREEN);
        c.SetTextSize(2);
        c.DrawCentreString(net::Ip(), SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2, GREEN);
        c.SetTextSize(1);
        c.DrawCentreString(std::string(net::HOSTNAME) + ".local", SCREEN_SIZE_DIV_2,
                           SCREEN_SIZE_DIV_2 + lineHeight * 2, GREEN);
    } else {
        c.DrawCentreString("- CONNECTED -", SCREEN_SIZE_DIV_2, 30, GREEN);
        c.DrawCentreString(net::Ssid(), SCREEN_SIZE_DIV_2, 44, DIM);
        c.SetTextSize(2);
        c.DrawCentreString(net::Ip(), SCREEN_SIZE_DIV_2, 58, GREEN);
        c.SetTextSize(1);
        c.DrawCentreString(std::string(net::HOSTNAME) + ".local", SCREEN_SIZE_DIV_2, 80, DIM);
        DrawQr(c, url, SCREEN_SIZE_DIV_2, 96, 104);
    }
    c.Flush();
}

}  // namespace screens
