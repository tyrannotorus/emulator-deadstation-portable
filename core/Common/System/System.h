#pragma once

// What the emulation asks of the program around it (core/deadstation).

#include <cstdint>

// a block of the PSP's mixed sound: frames of 32-bit stereo
void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume);
// forget what is queued (a save state loaded, the game ended)
void System_AudioClear();
