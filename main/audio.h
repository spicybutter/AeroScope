// ES8311 audio (#46): notification sounds through the speaker, plus microphone
// level measurement for the self-test. Codec bring-up, I2S format and PA
// (GPIO46, active-high) handling are the ones proven by the hardware
// diagnostic firmware; the PA is only enabled while a sound plays.
#pragma once

#include <cstdint>

namespace audio {

enum class Sound { Chime, Watch, Emergency, Test };

// Detects the ES8311 (chip ID read) and opens the codec. False if absent.
bool Init();
bool Available();

// Queue a sound (non-blocking). Respects the sound on/off and volume settings
// unless `force` (self-test).
void Play(Sound s, bool force = false);

// Blocking helpers for the self-test.
// Plays `ms` of a sine at `hz` and returns the loudest microphone RMS (dBFS)
// measured meanwhile; `baselineDb` gets the level measured just before.
float MeasureToneLoopback(int hz, int ms, float& baselineDb);
// Chip ID registers 0xFD/0xFE (0x83 0x11 for an ES8311).
bool ReadChipId(uint8_t& id1, uint8_t& id2);

}  // namespace audio
