#include "audio.h"
#include "log.h"

#include "Common/System/System.h"
#include "Core/HLE/sceDisplay.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace deadstation::audio
{
namespace
{

constexpr int Rate = 44100;
constexpr int DeviceFrames = 512;
constexpr uint32_t RingFrames = 16384;		// about 370 ms: more than the game ever gets ahead
constexpr uint32_t PlayFrom = Rate / 10;	// 100 ms queued before it plays, at the start and after running out

SDL_AudioDeviceID device = 0;
int16_t ring[RingFrames * 2];
std::atomic<uint64_t> written { 0 };
std::atomic<uint64_t> readFrames { 0 };
std::atomic<uint32_t> underruns { 0 };
std::atomic<uint32_t> dropped { 0 };
std::atomic<uint32_t> loudest { 0 };
std::atomic<bool> playing { false };

// clamped to 16 bits; returns the loudest sample
int narrow(int16_t *to, const int32_t *from, uint32_t samples, int peak)
{
	for (uint32_t i = 0; i < samples; i++)
	{
		const int s = std::clamp(from[i], -32768, 32767);
		to[i] = (int16_t)s;
		peak = std::max(peak, s < 0 ? -s : s);
	}
	return peak;
}

void callback(void *, Uint8 *stream, int bytes)
{
	int16_t *out = (int16_t *)stream;
	uint32_t want = (uint32_t)bytes / 4;
	uint64_t r = readFrames.load(std::memory_order_relaxed);
	const uint64_t w = written.load(std::memory_order_acquire);
	const uint32_t have = (uint32_t)(w - r);
	if (!playing.load(std::memory_order_relaxed))
	{
		if (have < PlayFrom)
		{
			memset(stream, 0, (size_t)bytes);
			return;
		}
		playing.store(true, std::memory_order_relaxed);
	}
	const uint32_t take = std::min(want, have);
	for (uint32_t i = 0; i < take; i++, r++)
	{
		const uint32_t at = (uint32_t)(r % RingFrames) * 2;
		*out++ = ring[at];
		*out++ = ring[at + 1];
	}
	readFrames.store(r, std::memory_order_release);
	if (take < want)
	{
		memset(out, 0, (size_t)(want - take) * 4);
		underruns.fetch_add(1, std::memory_order_relaxed);
		playing.store(false, std::memory_order_relaxed);
	}
}

}	// namespace

bool start()
{
	SDL_AudioSpec wanted {}, got {};
	wanted.freq = Rate;
	wanted.format = AUDIO_S16SYS;
	wanted.channels = 2;
	wanted.samples = DeviceFrames;
	wanted.callback = callback;
	device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &got, 0);
	if (device == 0)
	{
		DS_LOG("no sound: %s", SDL_GetError());
		return false;
	}
	DS_LOG("sound: %d Hz, %d frames a block", got.freq, got.samples);
	SDL_PauseAudioDevice(device, 0);
	return true;
}

void stop()
{
	if (device != 0)
		SDL_CloseAudioDevice(device);
	device = 0;
}

void pause(bool paused)
{
	if (device != 0)
		SDL_PauseAudioDevice(device, paused ? 1 : 0);
}

void clear()
{
	if (device != 0)
		SDL_LockAudioDevice(device);
	readFrames.store(written.load());
	playing.store(false);
	if (device != 0)
		SDL_UnlockAudioDevice(device);
}

uint32_t takeUnderruns()
{
	return underruns.exchange(0);
}

uint32_t takeDropped()
{
	return dropped.exchange(0);
}

uint32_t level()
{
	return (uint32_t)(written.load() - readFrames.load());
}

uint32_t takeLoudest()
{
	return loudest.exchange(0);
}

}

void System_AudioPushSamples(const int32_t *audio, int numSamples)
{
	using namespace deadstation::audio;
	uint64_t w = written.load(std::memory_order_relaxed);
	const uint64_t r = readFrames.load(std::memory_order_acquire);
	const uint32_t room = RingFrames - (uint32_t)(w - r);
	uint32_t count = (uint32_t)numSamples;
	if (count > room)
	{
		dropped.fetch_add(count - room, std::memory_order_relaxed);
		count = room;
	}
	// up to the ring's end, then from its start
	const uint32_t at = (uint32_t)(w % RingFrames);
	const uint32_t first = std::min(count, RingFrames - at);
	int peak = narrow(ring + at * 2, audio, first * 2, 0);
	peak = narrow(ring, audio + first * 2, (count - first) * 2, peak);
	w += count;
	written.store(w, std::memory_order_release);
	if ((uint32_t)peak > loudest.load(std::memory_order_relaxed))
		loudest.store((uint32_t)peak, std::memory_order_relaxed);
	// for Auto frame skip: how much is queued, against where it starts playing
	__DisplayNoteAudioQueued((float)(w - readFrames.load(std::memory_order_acquire)) / Rate, (float)PlayFrom / Rate);
}

void System_AudioClear()
{
	deadstation::audio::clear();
}
