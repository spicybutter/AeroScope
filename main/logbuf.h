// Log capture for the web log viewer (#63): every ESP_LOG line still goes to
// the serial console and is also queued for the web broadcaster, which keeps a
// short history for newly opened pages.
#pragma once

#include <string>
#include <vector>

namespace logbuf {

void Init();  // install the esp_log hook (call early)

// Move queued lines into `out` (non-blocking). Called by the web broadcaster.
void Drain(std::vector<std::string>& out, size_t maxLines = 64);

// Last lines seen by Drain() (history for GET /api/logs and new WebSocket clients).
std::vector<std::string> History();
void AddToHistory(const std::vector<std::string>& lines);

}  // namespace logbuf
