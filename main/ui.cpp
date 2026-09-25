#include "ui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "aircraft.h"
#include "alerts.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "net.h"
#include "radar_logic.h"
#include "selftest.h"
#include "settings.h"
#include "sysinfo.h"
#include "timekeeping.h"
#include "touch.h"
#include "units.h"

namespace ui {

static const char* TAG = "UI";

static constexpr int SIZE = 240;
static constexpr int CENTRE = SIZE / 2;
static constexpr int RING_TAP_RADIUS = 88;     // taps farther out than this hit the "range ring"
static constexpr int ZOOM_LEVELS[] = {1, 2, 4, 8};
static constexpr int BRIGHTNESS_STEPS[] = {25, 50, 75, 100};
enum Page { PAGE_RADAR, PAGE_NEARBY, PAGE_DETAILS, PAGE_SYSTEM, PAGE_COUNT };

static AircraftManager* s_aircraft;
static lv_display_t* s_disp;
static lv_obj_t* s_tileview;
static lv_obj_t* s_tiles[PAGE_COUNT];
static lv_obj_t* s_dots[PAGE_COUNT];
static lv_obj_t* s_canvas;
static uint16_t* s_frames[2];
static int s_back = 0;

static std::atomic<int> s_page{PAGE_RADAR};
static std::atomic<int> s_zoomOverride{0};     // 0 = use the setting
static std::string s_selected;                  // ICAO24 picked on the Nearby page (LVGL task only)

// Nearby / Details / System widgets
static lv_obj_t* s_nearbyList;
static std::vector<std::string> s_nearbyIcao;
static lv_obj_t* s_detTitle;
static lv_obj_t* s_detBody;
static lv_obj_t* s_sysBody;
static lv_obj_t* s_sysQr;
static std::string s_qrText;

static const lv_color_t GREEN = lv_color_hex(0x00E060);
static const lv_color_t DIM = lv_color_hex(0x5FAF7F);

// ---------------------------------------------------------------------------
// Touch input (#29)

static void ReadTouch(lv_indev_t*, lv_indev_data_t* data)
{
    int x, y;
    if (touch::Read(x, y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;  // LVGL keeps the last point
    }
}

// ---------------------------------------------------------------------------
// Radar page gestures (#31, #32, #35)

static int CurrentZoom()
{
    const int o = s_zoomOverride.load();
    return o > 0 ? o : settings::Current()->zoom;
}

static void StepZoom(int dir)
{
    const int z = CurrentZoom();
    int idx = 0;
    for (int i = 0; i < 4; i++)
        if (ZOOM_LEVELS[i] == z) idx = i;
    idx = dir > 0 ? (idx + 1) % 4 : std::max(idx - 1, 0);
    s_zoomOverride = ZOOM_LEVELS[idx];
    ESP_LOGI(TAG, "zoom x%d", ZOOM_LEVELS[idx]);
}

static void CycleBrightness()
{
    const int cur = settings::Current()->brightness;
    int next = BRIGHTNESS_STEPS[0];
    for (int b : BRIGHTNESS_STEPS) {
        if (b > cur) {
            next = b;
            break;
        }
    }
    settings::Update({{"bright", std::to_string(next)}});  // applied live by the supervisor
    ESP_LOGI(TAG, "brightness %d%%", next);
}

static void ShowPage(int page, bool animate = true)
{
    lv_tileview_set_tile_by_index(s_tileview, page, 0, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void OnRadarEvent(lv_event_t* e)
{
    const auto s = settings::Current();
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SINGLE_CLICKED) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        const float r = std::hypot(p.x - CENTRE, p.y - CENTRE);
        if (r > RING_TAP_RADIUS) {
            if (s->tapZoom) StepZoom(+1);
        } else if (s->tapBrightness) {
            CycleBrightness();
        }
    } else if (code == LV_EVENT_DOUBLE_CLICKED) {
        if (s->doubleTapReset) {
            s_zoomOverride = 0;
            ESP_LOGI(TAG, "view reset");
        }
    } else if (code == LV_EVENT_GESTURE) {
        if (!s->tapZoom) return;
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP) StepZoom(+1);
        else if (dir == LV_DIR_BOTTOM && CurrentZoom() > 1) StepZoom(-1);
    }
}

// Double-tap anywhere else also returns to the radar (#35).
static void OnPageDoubleTap(lv_event_t*)
{
    if (!settings::Current()->doubleTapReset) return;
    s_zoomOverride = 0;
    ShowPage(PAGE_RADAR);
}

static void OnTileChanged(lv_event_t*)
{
    lv_obj_t* active = lv_tileview_get_tile_active(s_tileview);
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_tiles[i] == active) s_page = i;
        lv_obj_set_style_bg_color(s_dots[i], s_tiles[i] == active ? GREEN : lv_color_hex(0x244a34), 0);
    }
}

// ---------------------------------------------------------------------------
// Page content helpers

static lv_obj_t* Title(lv_obj_t* parent, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, GREEN, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

static lv_obj_t* Body(lv_obj_t* parent, int y, int h)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_width(l, 176);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC8F5D6), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_height(l, h);
    lv_label_set_text(l, "");
    return l;
}

static std::string Fmt(double v, const char* suffix, int decimals = 0)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f %s", decimals, v, suffix);
    return buf;
}

static const char* CategoryName(int c)
{
    switch (c) {
    case 2: return "light";
    case 3: return "small";
    case 4: return "large";
    case 5: return "high vortex";
    case 6: return "heavy";
    case 7: return "high perf.";
    case 8: return "helicopter";
    case 9: return "glider";
    case 10: return "balloon";
    case 12: return "ultralight";
    case 14: return "drone";
    default: return "unknown type";
    }
}

// ---------------------------------------------------------------------------
// Nearby page (#33)

static void OnNearbyClick(lv_event_t* e)
{
    const size_t idx = reinterpret_cast<size_t>(lv_event_get_user_data(e));
    if (idx < s_nearbyIcao.size()) {
        s_selected = s_nearbyIcao[idx];
        ShowPage(PAGE_DETAILS);
    }
}

static void RefreshNearby()
{
    const auto s = settings::Current();
    auto list = s_aircraft->Snapshot(s->lat, s->lon);
    const int32_t scrollY = lv_obj_get_scroll_y(s_nearbyList);  // keep the user's place across refreshes
    lv_obj_clean(s_nearbyList);
    s_nearbyIcao.clear();
    int shown = 0;
    for (const auto& a : list) {
        if (a.onGround) continue;
        const auto d = units::Distance(a.distanceKm, s->distUnit);
        const auto alt = units::Altitude(a.altitude, s->altUnit);
        char row[64];
        snprintf(row, sizeof(row), "%-8s %4.0f%s %5.0f%s", a.callsign.empty() ? a.icao24.c_str() : a.callsign.c_str(),
                 d, units::DistSuffix(s->distUnit), alt, units::AltSuffix(s->altUnit));
        lv_obj_t* b = lv_list_add_button(s_nearbyList, nullptr, row);
        lv_obj_add_event_cb(b, OnNearbyClick, LV_EVENT_CLICKED, reinterpret_cast<void*>(s_nearbyIcao.size()));
        if (radar::IsEmergencySquawk(a.squawk)) lv_obj_set_style_text_color(b, lv_color_hex(0xFF5050), 0);
        s_nearbyIcao.push_back(a.icao24);
        if (++shown >= 25) break;
    }
    if (shown == 0) lv_list_add_text(s_nearbyList, s->locationSet ? "No aircraft in range" : "Set a location first");
    lv_obj_update_layout(s_nearbyList);
    lv_obj_scroll_to_y(s_nearbyList, scrollY, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// Details page (#33)

static void RefreshDetails()
{
    const auto s = settings::Current();
    auto list = s_aircraft->Snapshot(s->lat, s->lon);
    const AircraftInfo* a = nullptr;
    for (const auto& x : list)
        if (x.icao24 == s_selected) a = &x;
    if (!a) {  // nothing picked (or it left): show the nearest airborne aircraft
        for (const auto& x : list)
            if (!x.onGround) {
                a = &x;
                break;
            }
    }
    if (!a) {
        lv_label_set_text(s_detTitle, "DETAILS");
        lv_label_set_text(s_detBody, "No aircraft.\nPick one on the\nNearby page.");
        return;
    }
    lv_label_set_text(s_detTitle, a->callsign.empty() ? a->icao24.c_str() : a->callsign.c_str());
    std::string body;
    body += std::string(CategoryName(a->category)) + "  " + a->icao24 + "\n";
    body += a->country + "\n";
    body += "ALT " + Fmt(units::Altitude(a->altitude, s->altUnit), units::AltSuffix(s->altUnit)) + "\n";
    body += "SPD " + Fmt(units::Speed(a->velocity, s->speedUnit), units::SpeedSuffix(s->speedUnit)) + "\n";
    body += "V/S " + Fmt(units::VerticalRate(a->verticalRate, s->altUnit), units::VerticalSuffix(s->altUnit)) + "\n";
    body += "TRK " + Fmt(a->track, "deg") + "\n";
    body += "DST " + Fmt(units::Distance(a->distanceKm, s->distUnit), units::DistSuffix(s->distUnit), 1) + "  " +
            Fmt(a->bearingDeg, "deg") + "\n";
    if (!a->squawk.empty()) {
        body += "SQK " + a->squawk;
        if (radar::IsEmergencySquawk(a->squawk)) body += std::string(" ") + radar::EmergencyName(a->squawk);
    }
    lv_label_set_text(s_detBody, body.c_str());
    lv_obj_set_style_text_color(s_detTitle, radar::IsEmergencySquawk(a->squawk) ? lv_color_hex(0xFF5050) : GREEN, 0);
}

// ---------------------------------------------------------------------------
// System page (#33, with the dashboard QR code)

static void RefreshSystem()
{
    const FetchStatus st = s_aircraft->Status();
    char buf[320];
    const uint32_t now = Millis();
    snprintf(buf, sizeof(buf), "%s  %d dBm\n%s\nup %lu min  %s\n%s\n%s",
             net::Connected() ? net::Ssid().c_str() : "WiFi lost", net::Rssi(), net::Ip().c_str(),
             static_cast<unsigned long>(esp_timer_get_time() / 60000000), timekeeping::ClockText(true).c_str(),
             !st.lastError.empty() ? st.lastError.c_str()
             : st.haveData         ? ("data " + std::to_string((now - st.lastSuccessMs) / 1000) + " s ago").c_str()
                                   : "waiting for data",
             st.creditsRemaining >= 0 ? ("credits " + std::to_string(st.creditsRemaining)).c_str() : "");
    lv_label_set_text(s_sysBody, buf);
    const std::string url = "http://" + net::Ip() + "/";
    if (url != s_qrText) {
        s_qrText = url;
        lv_qrcode_update(s_sysQr, url.c_str(), url.size());
    }
}

static void RefreshTimer(lv_timer_t*)
{
    switch (s_page.load()) {
    case PAGE_NEARBY: RefreshNearby(); break;
    case PAGE_DETAILS: RefreshDetails(); break;
    case PAGE_SYSTEM: RefreshSystem(); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Alert banner (#55): shown on top of every page, tap to acknowledge.

static lv_obj_t* s_banner;
static uint32_t s_bannerAlertId;
static lv_timer_t* s_bannerTimer;

static void HideBanner()
{
    if (s_banner) lv_obj_delete(s_banner);
    s_banner = nullptr;
    if (s_bannerTimer) lv_timer_delete(s_bannerTimer);
    s_bannerTimer = nullptr;
}

static void ShowNextBanner();

static void OnBannerTap(lv_event_t*)
{
    alerts::Ack(s_bannerAlertId);
    HideBanner();
    ShowNextBanner();
}

static void OnBannerTimeout(lv_timer_t*)
{
    s_bannerTimer = nullptr;  // one-shot timers delete themselves
    HideBanner();
}

static void ShowBanner(const alerts::Alert& a)
{
    HideBanner();
    s_bannerAlertId = a.id;
    const bool emergency = a.kind == alerts::Kind::Emergency;
    s_banner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_banner, 196, 104);
    lv_obj_center(s_banner);
    lv_obj_set_style_radius(s_banner, 14, 0);
    lv_obj_set_style_bg_color(s_banner, emergency ? lv_color_hex(0x4A0808) : lv_color_hex(0x3A2A00), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_banner, emergency ? lv_color_hex(0xFF4040) : lv_color_hex(0xFFC400), 0);
    lv_obj_set_style_border_width(s_banner, 2, 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_banner, OnBannerTap, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* t = lv_label_create(s_banner);
    lv_label_set_text(t, a.title.c_str());
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, emergency ? lv_color_hex(0xFF6060) : lv_color_hex(0xFFD040), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, -4);
    lv_obj_t* body = lv_label_create(s_banner);
    lv_label_set_text(body, a.text.c_str());
    lv_obj_set_style_text_color(body, lv_color_white(), 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 6);
    lv_obj_t* hint = lv_label_create(s_banner);
    lv_label_set_text(hint, "tap to dismiss");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA0A0), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 4);

    s_bannerTimer = lv_timer_create(OnBannerTimeout, 60000, nullptr);
    lv_timer_set_repeat_count(s_bannerTimer, 1);
}

static void ShowNextBanner()
{
    if (!settings::Current()->banners) return;
    const auto active = alerts::Active();
    if (!active.empty()) ShowBanner(active.back());
}

void ShowAlert(const alerts::Alert& a)
{
    if (!s_disp || !settings::Current()->banners) return;
    lvgl_port_lock(0);
    ShowBanner(a);
    lvgl_port_unlock();
}

// ---------------------------------------------------------------------------
// Self-test view (#94)

static lv_obj_t* s_testView;
static lv_obj_t* s_testText;
static lv_timer_t* s_testTimer;

static void RefreshTestView(lv_timer_t*)
{
    std::string text;
    for (const auto& r : selftest::Results())
        text += std::string(selftest::StatusName(r.status)) + "  " + r.name + "\n   " + r.detail + "\n";
    text += selftest::Running() ? "\nrunning..." : "\ndone - tap Close";
    lv_label_set_text(s_testText, text.c_str());
}

static void CloseTestView(lv_event_t*)
{
    if (s_testTimer) lv_timer_delete(s_testTimer);
    s_testTimer = nullptr;
    if (s_testView) lv_obj_delete(s_testView);
    s_testView = nullptr;
}

static void OpenTestView(lv_event_t*)
{
    if (s_testView) return;
    selftest::Start();
    s_testView = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_testView, SIZE, SIZE);
    lv_obj_set_style_bg_color(s_testView, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_testView, 0, 0);
    lv_obj_set_style_radius(s_testView, 0, 0);
    lv_obj_t* title = lv_label_create(s_testView);
    lv_label_set_text(title, "SELF-TEST");
    lv_obj_set_style_text_color(title, GREEN, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    // Scrollable results box inside the round glass
    lv_obj_t* box = lv_obj_create(s_testView);
    lv_obj_set_size(box, 180, 140);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    s_testText = lv_label_create(box);
    lv_obj_set_width(s_testText, 170);
    lv_label_set_long_mode(s_testText, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_testText, lv_color_hex(0xC8F5D6), 0);
    lv_label_set_text(s_testText, "starting...");
    lv_obj_t* close = lv_button_create(s_testView);
    lv_obj_set_size(close, 90, 34);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(close, CloseTestView, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    s_testTimer = lv_timer_create(RefreshTestView, 500, nullptr);
}

// ---------------------------------------------------------------------------
// Build

static lv_obj_t* PlainTile(int col)
{
    lv_obj_t* t = lv_tileview_add_tile(s_tileview, col, 0, static_cast<lv_dir_t>(LV_DIR_LEFT | LV_DIR_RIGHT));
    lv_obj_set_style_bg_color(t, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    if (col != PAGE_RADAR) lv_obj_add_event_cb(t, OnPageDoubleTap, LV_EVENT_DOUBLE_CLICKED, nullptr);
    return t;
}

static void Build()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_tileview = lv_tileview_create(scr);
    lv_obj_set_size(s_tileview, SIZE, SIZE);
    lv_obj_set_style_bg_color(s_tileview, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tileview, OnTileChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    for (int i = 0; i < PAGE_COUNT; i++) s_tiles[i] = PlainTile(i);

    // Radar: canvas showing the renderer's frames
    s_canvas = lv_canvas_create(s_tiles[PAGE_RADAR]);
    lv_canvas_set_buffer(s_canvas, s_frames[0], SIZE, SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, OnRadarEvent, LV_EVENT_ALL, nullptr);

    // Nearby
    Title(s_tiles[PAGE_NEARBY], "NEARBY");
    s_nearbyList = lv_list_create(s_tiles[PAGE_NEARBY]);
    lv_obj_set_size(s_nearbyList, 184, 166);
    lv_obj_align(s_nearbyList, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(s_nearbyList, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_nearbyList, 0, 0);
    lv_obj_set_style_text_font(s_nearbyList, &lv_font_montserrat_14, 0);

    // Details
    s_detTitle = Title(s_tiles[PAGE_DETAILS], "DETAILS");
    lv_obj_set_style_text_font(s_detTitle, &lv_font_montserrat_20, 0);
    s_detBody = Body(s_tiles[PAGE_DETAILS], 50, 160);

    // System
    Title(s_tiles[PAGE_SYSTEM], "SYSTEM");
    s_sysBody = Body(s_tiles[PAGE_SYSTEM], 40, 90);
    s_sysQr = lv_qrcode_create(s_tiles[PAGE_SYSTEM]);
    lv_qrcode_set_size(s_sysQr, 76);
    lv_qrcode_set_dark_color(s_sysQr, lv_color_black());
    lv_qrcode_set_light_color(s_sysQr, lv_color_white());
    lv_obj_align(s_sysQr, LV_ALIGN_TOP_MID, -36, 130);
    lv_obj_t* testBtn = lv_button_create(s_tiles[PAGE_SYSTEM]);
    lv_obj_set_size(testBtn, 62, 40);
    lv_obj_align(testBtn, LV_ALIGN_TOP_MID, 46, 148);
    lv_obj_add_event_cb(testBtn, OpenTestView, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* testLbl = lv_label_create(testBtn);
    lv_label_set_text(testLbl, "Self\ntest");
    lv_obj_set_style_text_align(testLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(testLbl);

    // Page dots
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_dots[i] = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 6, 6);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_align(s_dots[i], LV_ALIGN_BOTTOM_MID, (i - (PAGE_COUNT - 1) / 2.0f) * 12, -8);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    OnTileChanged(nullptr);

    lv_timer_create(RefreshTimer, 1000, nullptr);
}

bool Start(AircraftManager& aircraft)
{
    s_aircraft = &aircraft;

    const size_t frameBytes = SIZE * SIZE * sizeof(uint16_t);
    for (auto& f : s_frames) {
        f = static_cast<uint16_t*>(heap_caps_calloc(1, frameBytes, MALLOC_CAP_SPIRAM));
        if (!f) {
            ESP_LOGE(TAG, "no memory for radar frames");
            return false;
        }
    }

    const bool haveTouch = touch::Init();

    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;
    port.task_affinity = 1;
    port.task_stack = 8192;
    if (lvgl_port_init(&port) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return false;
    }

    lvgl_port_display_cfg_t disp = {};
    disp.io_handle = board::LcdIo();
    disp.panel_handle = board::LcdPanel();
    disp.buffer_size = SIZE * 40;
    disp.double_buffer = true;
    disp.hres = SIZE;
    disp.vres = SIZE;
    disp.monochrome = false;
    disp.color_format = LV_COLOR_FORMAT_RGB565;
    // Keep the panel's single horizontal flip (MADCTL 0x48) when the port applies rotation.
    disp.rotation.swap_xy = false;
    disp.rotation.mirror_x = true;
    disp.rotation.mirror_y = false;
    disp.flags.buff_dma = 1;
    disp.flags.swap_bytes = 1;
    s_disp = lvgl_port_add_disp(&disp);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return false;
    }

    lvgl_port_lock(0);
    lv_theme_t* theme = lv_theme_default_init(s_disp, GREEN, DIM, true, &lv_font_montserrat_14);
    lv_display_set_theme(s_disp, theme);
    if (haveTouch) {
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, ReadTouch);
        lv_indev_set_display(indev, s_disp);
    }
    Build();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "LVGL UI started (touch %s)", haveTouch ? "on" : "not found");
    return true;
}

bool RadarVisible() { return s_page.load() == PAGE_RADAR; }
int Zoom(int settingZoom)
{
    const int o = s_zoomOverride.load();
    return o > 0 ? o : settingZoom;
}

uint16_t* BackBuffer() { return s_frames[s_back]; }

void PresentRadar()
{
    lvgl_port_lock(0);
    lv_canvas_set_buffer(s_canvas, s_frames[s_back], SIZE, SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(s_canvas);
    lvgl_port_unlock();
    s_back ^= 1;  // the other frame is no longer referenced by LVGL
}

}  // namespace ui
