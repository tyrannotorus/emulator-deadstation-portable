#include "clock.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace deadstation::cpufreq
{

namespace
{

constexpr const char *MinPath = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq";
constexpr const char *MaxPath = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq";

bool usable = false;		// the floor read, and above it a top to hold
bool fixed = true;			// CPU Freq 1.8GHz
bool forced = false;		// (tests) DEADSTATION_PORTABLE_CPUFREQ over the setting
bool held = false;			// the floor at the top now
int originalKhz = 0, topKhz = 0, writtenKhz = 0;

int readKhz(const char *path)
{
	int khz = 0;
	if (FILE *f = std::fopen(path, "r"))
	{
		if (std::fscanf(f, "%d", &khz) != 1)
			khz = 0;
		std::fclose(f);
	}
	return khz;
}

bool write(int khz, const char *why)
{
	// someone else moved the floor (a test's pin, NextUI): theirs from now on
	const int now = readKhz(MinPath);
	if (now != writtenKhz)
	{
		DS_LOG("cpu freq: the floor is %d MHz, not the %d MHz set here: left alone from now on", now / 1000, writtenKhz / 1000);
		usable = false;
		return false;
	}
	FILE *f = std::fopen(MinPath, "w");
	if (f == nullptr || std::fprintf(f, "%d", khz) < 0 || std::fclose(f) != 0)
	{
		DS_LOG("cpu freq: the floor can't be set to %d MHz: left to NextUI", khz / 1000);
		usable = false;
		return false;
	}
	writtenKhz = khz;
	DS_LOG("cpu freq: floor %d MHz (%s)", khz / 1000, why);
	return true;
}

void giveBack(const char *why)
{
	if (usable && held && write(originalKhz, why))
		held = false;
}

}

void start()
{
	if (const char *env = std::getenv("DEADSTATION_PORTABLE_CPUFREQ"); env != nullptr && *env != '\0')
	{
		forced = true;
		fixed = std::strcmp(env, "dynamic") != 0;
	}
	originalKhz = writtenKhz = readKhz(MinPath);
	topKhz = readKhz(MaxPath);
	usable = originalKhz > 0 && topKhz > originalKhz;
	DS_LOG("cpu freq: %s%s; NextUI's range %d to %d MHz", fixed ? "held at the top while the game runs" : "dynamic",
			forced ? " (DEADSTATION_PORTABLE_CPUFREQ)" : "", originalKhz / 1000, topKhz / 1000);
}

void setFixed(bool on)
{
	if (!forced)
		fixed = on;
}

void tick()
{
	if (!usable)
		return;
	if (fixed && !held)
		held = write(topKhz, "CPU Freq 1.8GHz");
	else if (!fixed && held)
		giveBack("CPU Freq Dynamic");
}

void release()
{
	giveBack(fixed ? "the menu" : "CPU Freq Dynamic");
}

void stop()
{
	giveBack("quitting");
	usable = false;
}

}
