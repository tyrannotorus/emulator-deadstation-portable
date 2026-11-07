// The PSP's sound to the speaker: the mix goes into a ring, at the rate the game makes it, and SDL's thread takes it from there.
// Never stretched: a game behind its time leaves the speaker waiting (silence, and an underrun counted), and the ring refills
// before it plays again.
#pragma once
#include <cstdint>

namespace deadstation::audio
{

bool start();
void stop();
void pause(bool paused);
// forget what is queued
void clear();
// since the last call
uint32_t takeUnderruns();
uint32_t takeDropped();
// frames waiting now
uint32_t level();

}
