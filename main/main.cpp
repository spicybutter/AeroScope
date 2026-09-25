// AeroScope - ESP-IDF flight radar firmware for the Spotpear SP-ESP32-S3-1.28-BOX.
// Port of micro-radar (github.com/AnthonySturdy/micro-radar @ 95701a8), plus the
// features listed in FEATURES.md. See README.md.
#include <cstdio>

#include "aircraft.h"
#include "alerts.h"
#include "audio.h"
#include "board.h"
#include "captive_dns.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "logbuf.h"
#include "maintenance.h"
#include "mdns.h"
#include "net.h"
#include "nvs_flash.h"
#include "opensky.h"
#include "radar_view.h"
#include "screens.h"
#include "selftest.h"
#include "settings.h"
#include "sysinfo.h"
#include "timekeeping.h"
#include "ui.h"
#include "web.h"

static const char* TAG = "MAIN";

static constexpr uint32_t NETWORK_INFO_SCREEN_MS = 8000;
static constexpr uint32_t NETWORK_INFO_SERIAL_INTERVAL_MS = 60000;
static constexpr uint32_t SETTINGS_POLL_MS = 250;

static gfx::Canvas s_canvas;
static OpenSkyClient* s_opensky;
static AircraftManager* s_aircraft;
static bool s_uiOk = false;  // false: LVGL failed, keep the direct-flush radar

static void InitNvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Standard ESP-IDF recovery; loses saved Wi-Fi and radar settings.
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    // A reset requested from the web page (#64) runs here, before anything uses NVS.
    if (maintenance::ApplyPendingReset()) ESP_ERROR_CHECK(nvs_flash_init());
}

static void StartMdns()
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "failed to start mDNS, continuing without it");
        return;
    }
    mdns_hostname_set(net::HOSTNAME);
    mdns_instance_name_set("AeroScope");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

// Draws radar frames for the LVGL radar page (#36). Idle while another page shows.
static void RenderTask(void*)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        const auto s = settings::Current();
        if (!s_uiOk) {
            radar_view::DrawFrame(s_canvas, *s_aircraft, *s, s->zoom, true);
        } else if (ui::RadarVisible()) {
            s_canvas.SetBuffer(ui::BackBuffer());
            radar_view::DrawFrame(s_canvas, *s_aircraft, *s, ui::Zoom(s->zoom), false);
            ui::PresentRadar();
        }
        const TickType_t period = pdMS_TO_TICKS(1000 / (s->fps > 0 ? s->fps : 30));
        xTaskDelayUntil(&last, period > 0 ? period : 1);
    }
}

// Apply live settings that are not read per frame (brightness, log level) and
// print the network info every minute.
[[noreturn]] static void SupervisorLoop()
{
    std::shared_ptr<const settings::Settings> applied;
    uint32_t lastInfo = Millis();
    for (;;) {
        const auto s = settings::Current();
        if (s != applied) {
            if (!applied || applied->brightness != s->brightness) board::BacklightSet(s->brightness);
            if (!applied || applied->logLevel != s->logLevel) sysinfo::ApplyLogLevel(s->logLevel);
            applied = s;
        }
        if (Millis() - lastInfo >= NETWORK_INFO_SERIAL_INTERVAL_MS) {
            lastInfo = Millis();
            net::PrintInfo();
        }
        vTaskDelay(pdMS_TO_TICKS(SETTINGS_POLL_MS));
    }
}

extern "C" void app_main(void)
{
    // board power / pins that must be set before anything else
    board::PowerHold();
    logbuf::Init();  // capture log lines for the web log viewer from the start
    printf("\nAeroScope (ESP-IDF) starting\n");

    InitNvs();
    settings::Init();
    const auto s = settings::Current();
    sysinfo::ApplyLogLevel(s->logLevel);
    sysinfo::Init();

    // initialise screen (backlight stays off until the first frame is drawn)
    ESP_ERROR_CHECK(board::BacklightInit());
    ESP_ERROR_CHECK(board::LcdInit());
    if (!s_canvas.Init()) {
        ESP_LOGE(TAG, "frame buffer allocation failed");
        return;
    }
    s_canvas.Fill(0);
    s_canvas.Flush();
    board::BacklightSet(s->brightness);
    if (s->splash) radar_view::Splash(s_canvas);
    screens::Connecting(s_canvas);

    // establish WiFi connection (or run the setup portal)
    if (net::Start() == net::Mode::SetupPortal) {
        screens::Setup(s_canvas, s->qrCodes);
        captive_dns::Start();
        web::StartPortalServer();
        SupervisorLoop();  // setup portal saves and restarts
    }

    timekeeping::Start(s->ntpServer, s->timezone);

    // show where the configuration page lives (serial + screen)
    net::PrintInfo();
    screens::Connected(s_canvas, s->qrCodes);
    vTaskDelay(pdMS_TO_TICKS(NETWORK_INFO_SCREEN_MS));

    // web app + mDNS
    s_opensky = new OpenSkyClient();
    s_aircraft = new AircraftManager(*s_opensky);
    StartMdns();
    web::StartConfigServer(*s_aircraft);

    if (!s->locationSet) ESP_LOGW(TAG, "no location saved - set it at http://%s", net::Ip().c_str());

    // aircraft data + display
    s_aircraft->Initialise();
    s_aircraft->StartFetchTask();

    // Hand the display to LVGL (no direct-flush screens after this point).
    s_uiOk = ui::Start(*s_aircraft);
    if (s_uiOk) {
        s_canvas.ReleaseFlushBuffer();
    } else {
        ESP_LOGE(TAG, "UI start failed - falling back to the plain radar screen");
    }

    // Sound, alerts and self-test (#46, #50, #55, #57, #94)
    if (!audio::Init()) ESP_LOGW(TAG, "no audio - alerts will be silent");
    selftest::Init(*s_aircraft);
    if (s_uiOk) alerts::AddListener(ui::ShowAlert);
    alerts::AddListener(web::PushAlert);
    alerts::Start(*s_aircraft);
    xTaskCreatePinnedToCore(RenderTask, "render", 6144, nullptr, 3, nullptr, 1);

    SupervisorLoop();
}
