// Frame timing
//
// A frame on the main thread should look a bit like this:
//
// 1. -- Wait for the right time to start the frame  (alternatively, see this is step 8).
// 2. Sample inputs (on some platforms, this is done continouously during step 3)
// 3. Run CPU
// 4. Submit GPU commands (there's no reason to ever wait before this).
// 5. -- Wait for the right time to present
// 6. Send Present command
// 7. Do other end-of-frame stuff
//
// To minimize latency, we should *maximize* 1 and *minimize* 5 (while still keeping some margin to soak up hitches).
// Additionally, if too many completed frames have been buffered up, we need a feedback mechanism, so we can temporarily
// artificially increase 1 in order to "catch the CPU up".
//
// There are some other things that can influence the frame timing:
// * Unthrottling. If vsync is off or the backend can change present mode dynamically, we can simply disable all waits during unthrottle.
// * Frame skipping. This gets complicated.
// * The game not actually asking for flips, like in static loading screens

#include <algorithm>
#include "ppsspp_config.h"
#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Common/TimeUtil.h"

#include "Core/CoreParameter.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/Config.h"
#include "Core/HW/Display.h"
#include "Core/HLE/sceNet.h"
#include "Core/FrameTiming.h"

FrameTiming g_frameTiming;

constexpr double g_MaxSleepTime = 0.5;  // 500ms.

// Note: To avoid hang bugs, this has a maximum of 0.5 second wait time.
void WaitUntil(double now, double timestamp, const char *reason) {
	// Use precise timing.
	sleep_precise(std::min(timestamp - now, g_MaxSleepTime), reason);
}

void FrameTiming::DeferWaitUntil(double until, double *curTimePtr) {
	_dbg_assert_(until > 0.0);
	waitUntil_ = until;
	curTimePtr_ = curTimePtr;
}

void FrameTiming::PostSubmit() {
	if (waitUntil_ != 0.0) {
		WaitUntil(time_now_d(), waitUntil_, "post-submit");
		if (curTimePtr_) {
			*curTimePtr_ = waitUntil_;
			curTimePtr_ = nullptr;
		}
		waitUntil_ = 0.0;
	}
}

