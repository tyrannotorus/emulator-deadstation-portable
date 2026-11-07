#include "ppsspp_config.h"

#include <cstdio>
#include <cstdint>

#include "Common/TimeUtil.h"
#include "Common/Data/Random/Rng.h"
#include "Common/Log.h"



#include <sys/time.h>
#include <unistd.h>

// for _mm_pause

#include <ctime>

// TODO: https://github.com/floooh/sokol/blob/9a6237fcdf213e6da48e4f9201f144bcb2dcb46f/sokol_time.h#L229-L248

constexpr double micros = 1000000.0;
constexpr double nanos = 1000000000.0;


void TimeInit() {
	// Nothing to do.
}

// The only intended use is to match the timings in VK_GOOGLE_display_timing
uint64_t time_now_raw() {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return tp.tv_sec * 1000000000ULL + tp.tv_nsec;
}

static uint64_t g_startTime;

double from_time_raw(uint64_t raw_time) {
	return (double)(raw_time - g_startTime) * (1.0 / nanos);
}

double time_now_d() {
	uint64_t raw_time = time_now_raw();
	if (g_startTime == 0) {
		g_startTime = raw_time;
	}
	return from_time_raw(raw_time);
}

double from_time_raw_relative(uint64_t raw_time) {
	return (double)raw_time * (1.0 / nanos);
}

double time_now_unix_utc() {
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	return (double)tp.tv_sec + (double)tp.tv_nsec / 1000000000.0;
}

void yield() {
	// Took this out for now. See issue #17877
	// __builtin_arm_isb(15);
}

Instant::Instant() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	nativeStart_ = ts.tv_sec;
	nsecs_ = ts.tv_nsec;
}

int64_t Instant::ElapsedNanos() const {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t secs = ts.tv_sec - nativeStart_;
	int64_t nsecs = ts.tv_nsec - nsecs_;
	if (nsecs < 0) {
		secs--;
		nsecs += 1000000000;
	}
	return secs * 1000000000ULL + nsecs;
}

double Instant::ElapsedSeconds() const {
	return (double)ElapsedNanos() * (1.0 / nanos);
}


#define SLEEP_LOG_ENABLED 0

void sleep_ms(int ms, const char *reason) {
	if (ms <= 0) {
		return;
	}
#if SLEEP_LOG_ENABLED
	INFO_LOG(Log::System, "Sleep %d ms: %s", ms, reason);
#endif
	usleep(ms * 1000);
}

void sleep_us(int us, const char *reason) {
	if (us <= 0) {
		return;
	}
#if SLEEP_LOG_ENABLED
	INFO_LOG(Log::System, "Sleep %d us: %s", us, reason);
#endif
	usleep(us);
}

// This can be a little more expensive in some circumstances, so only use when necessary.
void sleep_precise(double seconds, const char *reason) {
	if (seconds <= 0.0) {
		return;
	}
#if SLEEP_LOG_ENABLED
	INFO_LOG(Log::System, "Sleep precise %f s: %s", seconds, reason);
#endif
	usleep(seconds * 1000000.0);
}

// Return the current time formatted as Minutes:Seconds:Milliseconds
// in the form 00:00:000.
void GetCurrentTimeFormatted(char formattedTime[13]) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	snprintf(formattedTime, 13, "%02d:%02d:%03d", tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
}

// We don't even bother synchronizing this, it's fine if threads stomp a bit.
static GMRng g_sleepRandom;

void sleep_random(double minSeconds, double maxSeconds, const char *reason) {
	const double waitSeconds = minSeconds + (maxSeconds - minSeconds) * g_sleepRandom.F();
	sleep_precise(waitSeconds, reason);
}
