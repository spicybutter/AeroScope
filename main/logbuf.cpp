#include "logbuf.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

namespace logbuf {

static constexpr size_t RING_BYTES = 16 * 1024;
static constexpr size_t HISTORY_LINES = 200;
static constexpr size_t MAX_LINE = 240;

static RingbufHandle_t s_ring;
static vprintf_like_t s_original;
static std::mutex s_history_mutex;
static std::deque<std::string> s_history;

// Runs in whatever task logged. Must not block and must not log.
static int Hook(const char* fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int n = s_original ? s_original(fmt, args) : vprintf(fmt, args);
    if (s_ring) {
        char line[MAX_LINE];
        int len = vsnprintf(line, sizeof(line), fmt, copy);
        if (len > 0) {
            if (len >= static_cast<int>(sizeof(line))) len = sizeof(line) - 1;
            // Strip ANSI colour codes and the trailing newline
            char clean[MAX_LINE];
            int c = 0;
            for (int i = 0; i < len; i++) {
                if (line[i] == '\x1b') {
                    while (i < len && line[i] != 'm') i++;
                    continue;
                }
                if (line[i] != '\n' && line[i] != '\r') clean[c++] = line[i];
            }
            if (c > 0) xRingbufferSend(s_ring, clean, c, 0);  // drop the line if full
        }
    }
    va_end(copy);
    return n;
}

void Init()
{
    // The ring's storage can live in PSRAM; only the handle is internal.
    s_ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    if (!s_ring) s_ring = xRingbufferCreate(RING_BYTES / 4, RINGBUF_TYPE_NOSPLIT);
    s_original = esp_log_set_vprintf(Hook);
}

void Drain(std::vector<std::string>& out, size_t maxLines)
{
    if (!s_ring) return;
    for (size_t i = 0; i < maxLines; i++) {
        size_t len = 0;
        char* item = static_cast<char*>(xRingbufferReceive(s_ring, &len, 0));
        if (!item) break;
        out.emplace_back(item, len);
        vRingbufferReturnItem(s_ring, item);
    }
}

std::vector<std::string> History()
{
    std::lock_guard<std::mutex> lock(s_history_mutex);
    return {s_history.begin(), s_history.end()};
}

void AddToHistory(const std::vector<std::string>& lines)
{
    std::lock_guard<std::mutex> lock(s_history_mutex);
    for (const auto& l : lines) {
        s_history.push_back(l);
        if (s_history.size() > HISTORY_LINES) s_history.pop_front();
    }
}

}  // namespace logbuf
