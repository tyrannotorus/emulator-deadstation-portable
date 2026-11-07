// Save states: the emulation's, compressed, in ten slots per game: <home>/<game>.state (slot 1), <game>_N.state
// (slot N + 1), <game> being the file NextUI started (its name without its extension), each with the game's picture when it
// was saved in <that>.pic. Between frames only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace deadstation::states
{

constexpr int SlotCount = 10;

void setGame(const std::string& romPath);
std::string slotPath(int slot);

// the picture is RGB24 (may be empty)
bool save(const std::string& path, const std::vector<uint8_t>& rgb, int width, int height);
bool load(const std::string& path);
bool exists(int slot);
// a slot's picture; false if none
bool picture(int slot, std::vector<uint8_t>& rgb, int& width, int& height);

}
