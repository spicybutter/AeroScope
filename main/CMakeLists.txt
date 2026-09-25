idf_component_register(
    SRCS "main.cpp" "board.cpp" "gfx.cpp" "config_store.cpp" "settings.cpp" "net.cpp" "captive_dns.cpp"
         "web.cpp" "opensky.cpp" "aircraft.cpp" "screens.cpp" "radar_view.cpp" "timekeeping.cpp"
         "sysinfo.cpp" "logbuf.cpp" "maintenance.cpp" "touch.cpp" "ui.cpp"
         "audio.cpp" "alerts.cpp" "selftest.cpp"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/app.html"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_spi esp_lcd esp_psram esp_timer spi_flash
             esp_wifi esp_netif esp_event nvs_flash lwip esp_http_client esp_http_server
             mbedtls esp_system espcoredump esp_app_format esp_ringbuf
)
