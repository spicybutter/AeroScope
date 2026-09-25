# Features

Every feature that can be optional has a toggle or choice in the dashboard's Settings tab, generated from the schema in `main/settings.cpp`. The `#` numbers are feature IDs; source comments refer to them (e.g. `// Static IP (#75)`).

## Radar and core

| # | Feature | Where | Option(s) | What you see |
|---|---|---|---|---|
| 4 | Zoom levels | `radar_view`, `ui` | `zoom` (1/2/4); touch adds 8 | ×2/×4 shrinks the shown radius; "x2" shows at the bottom |
| 5 | Trails | `aircraft` (ring buffer in PSRAM), `radar_view` | `trail`, `trail_s` | lines behind moving aircraft |
| 6 | Fading trails | `radar_view` | `trail_fade` | older segments dimmer |
| 7 | Colour by altitude | `radar_logic::AltitudeColor` | `alt_color` | orange (low) → yellow → green → cyan → blue → magenta (high) |
| 8 | Climb/descent arrows | `radar_view` | `vs_arrow` | cyan ↑ / orange ↓ after the altitude label (\|V/S\| ≥ 1 m/s) |
| 11 | Aircraft count | `radar_view` overlays | `show_count` | "N AC" at the bottom |
| 12 | Clock | `timekeeping`, overlays | `show_clock`, `clock_24h` | time at the top once NTP syncs |
| 13 | Wi-Fi icon | overlays | `show_wifi` | 3 bars under the clock; red X when down |
| 14 | Data status | overlays + web | `show_status` | "upd 12s"; yellow when stale, red with error text |
| 15 | Units | `units.h` | `u_alt` m/ft, `u_spd` m/s·km/h·mph·kt, `u_dist` km/mi/nm | labels, web table and pages follow the choice |
| 19 | Category icons | `radar_view` (OpenSky `extended=1`) | `cat_icons` | plane sizes by class, helicopter rotor, glider, balloon, drone |
| 20 | Emergency squawk highlight | `radar_view` | `emerg_hl` | red symbol, blinking ring, "EMERGENCY/RADIO FAIL/HIJACK" |
| 27 | Boot splash | `radar_view::Splash` | `splash` | 1.5 s "AEROSCOPE" rings at boot |
| 28 | Frame rate | render task | `fps` 5–30 | smoother, or lower power |
| 71 | Offline / error banner | overlays | — | "WIFI LOST" / "NO DATA" banner |
| 72 | Rate-limit awareness | `radar_logic::NextIntervalMs`, `opensky` | — | status shows remaining credits; waits Retry-After on 429; paces to the daily reset |
| 73 | NTP + time zone | `timekeeping` | `tz` (POSIX), `ntp` | clock correct for the chosen zone |
| 76 | Polling interval | pacing | `poll_s` (0 = automatic) | interval shown on the status page |
| 95 | Crash reason + core dump | `sysinfo`, coredump partition | — | status: "last reset", "stored crash" |
| 97 | Log levels | `sysinfo::ApplyLogLevel` | `log_level` | serial/web log verbosity changes live |

## Web dashboard and network

| # | Feature | Where | Option(s) | What you see |
|---|---|---|---|---|
| 58 | Live browser radar | `web/app.html` Radar tab, WebSocket `/ws` (1 Hz) | — | planes move without refresh; several tabs at once |
| 59 | Live aircraft table | Aircraft tab | — | sortable columns; click a row → radar + details |
| 60 | Settings on the web | Settings tab (schema-driven) | all | changes apply live, or show "restart" |
| 61 | Change Wi-Fi from the page | Wi-Fi tab, `/api/wifi/*` | — | scan, add, forget, restart to connect |
| 62 | System info | System tab, `/api/status` | — | firmware, memory, network, data status |
| 63 | Live device log | `logbuf` + WS | `log_level` | lines stream in the System tab |
| 64 | Reboot / reset buttons | `/api/reboot`, `/api/reset` (`maintenance`: erase runs at next boot) | — | reset settings / forget Wi-Fi / factory reset |
| 65 | Backup / restore | `/api/backup[?secrets=1]`, `/api/restore` | secrets checkbox | download JSON, restore → restart |
| 66 | JSON API | `/api/aircraft`, `/api/status`, `/api/settings`, … | — | see README "Web API" |
| 67 | Works offline | self-contained HTML/CSS/JS (no CDN) | — | page loads with no internet on the phone |
| 74 | Several remembered networks | `net` (NVS `wifi`/`list`, max 8) | — | strongest saved network at boot; switches after repeated loss |
| 75 | Static IP / DNS | `net::ApplyIpConfig` | `static_ip`, `ip`, `netmask`, `gateway`, `dns1`, `dns2` | device keeps the set address |
| 21 | Points of interest | settings `pois` + LCD and web radar | `show_pois`, `pois` | cyan diamonds with names |
| 26 | QR codes | `screens` (setup + connected), System page | `qr` | phone camera joins the hotspot / opens the page |

## LVGL and touch

| # | Feature | Where | Option(s) | What you see |
|---|---|---|---|---|
| 36 | LVGL UI | `ui` (LVGL 9.3 + esp_lvgl_port 2.9) | — | smooth pages; radar via double-buffered canvas |
| 29 | Touch driver + calibration | `touch` (CST816D) | `touch_swap`, `touch_mx`, `touch_my` | touches land where you tap |
| 31 | Tap to cycle brightness | radar page, middle area | `tap_bright` | 25 → 50 → 75 → 100 % |
| 32 | Tap ring / swipe to zoom | radar page | `tap_zoom` | tap near the edge ring or swipe up = zoom in; swipe down = out |
| 33 | Swipe pages | tileview: Radar / Nearby / Details / System | — | swipe left/right, dots at the bottom |
| 35 | Double-tap reset | radar and other pages | `dbl_reset` | zoom back to the setting, return to the radar |

## Sound, alerts, self-test

| # | Feature | Where | Option(s) | What you see |
|---|---|---|---|---|
| 46 | Audio + chimes | `audio` (ES8311, PA on only while playing) | `sound`, `volume` | System tab → "Play chime" |
| 50 | Emergency squawk alert | `alerts` (dedupe 30 min) | `alert_emerg` | emergency sound + banner |
| 55 | On-screen banner, tap to dismiss | `ui` (LVGL top layer) | `banners` | card over any page; tap = acknowledge |
| 57 | Browser alert for watched aircraft | `alerts` + WS + `app.html` | `alert_watch`, `watchlist` | in-page banner, beep, flashing tab title |
| 94 | Hardware self-test | `selftest` (LCD System page button, web System tab) | — | PSRAM, RAM, flash, touch, backlight, codec, speaker→mic loopback, Wi-Fi, NTP, data |

### Notes

- **Alerts** fire when a matching aircraft newly appears in range, at most once per 30 min per aircraft. The watchlist accepts ICAO24 codes or callsign prefixes.
- **Browser notifications (#57):** browsers only allow system notifications on HTTPS pages. The device serves plain HTTP on the LAN, so alerts appear as an in-page banner with a beep and a flashing tab title while the page is open.
- **Self-test:** items that need a human (backlight) are marked "visual check". "Speaker + microphone" passes when the mic hears the 1 kHz tone at least 10 dB above the room baseline.
- **Aircraft category (#19):** relies on the OpenSky `extended=1` category field. Many transponders report "no info", which draws the default triangle.

## Differences from micro-radar

- **TLS:** OpenSky certificates are verified with the ESP-IDF CA bundle.
- **Rendering:** a dedicated render task keeps the radar animating while a request is in flight.
- **HTTP errors** (e.g. 429) skip the update and keep the current aircraft instead of clearing the screen.
- **Setup portal:** a built-in captive portal (network scan + password) replaces WiFiManager.
- **No location set:** instead of querying 0,0, the screen asks you to set a location on the dashboard.
- **Wi-Fi:** reconnects automatically after a loss.
