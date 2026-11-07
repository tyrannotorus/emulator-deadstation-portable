#include "ppsspp_config.h"


// TODO: Many other platforms also support TLS, in fact probably nearly all that we support
// these days.

#include <cstring>
#include <cstdint>

#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Data/Encoding/Utf8.h"

AttachDetachFunc g_attach;
AttachDetachFunc g_detach;

void AttachThreadToJNI() {
	if (g_attach) {
		g_attach();
	} else {
	}
}

void DetachThreadFromJNI() {
	if (g_detach) {
		g_detach();
	}
}

void RegisterAttachDetach(AttachDetachFunc attach, AttachDetachFunc detach) {
	g_attach = attach;
	g_detach = detach;
}

#if (PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(LINUX)) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>


#ifdef TLS_SUPPORTED
static thread_local const char *curThreadName;
#endif



const char *GetCurrentThreadName() {
#ifdef TLS_SUPPORTED
	return curThreadName;
#else
	return "";
#endif
}

void SetCurrentThreadName(const char *threadName) {
	pthread_setname_np(pthread_self(), threadName);

	// Set the locally known threadname using a thread local variable.
#ifdef TLS_SUPPORTED
	curThreadName = threadName;
#endif
}


void AssertCurrentThreadName(const char *threadName) {
#ifdef TLS_SUPPORTED
	if (curThreadName && strcmp(curThreadName, threadName) != 0) {
		ERROR_LOG(Log::System, "Thread name assert failed: Expected %s, was %s", threadName, curThreadName);
		_dbg_assert_msg_(false, "Thread name assert failed: Expected %s, was %s", threadName, curThreadName);
	}
#endif
}

int GetCurrentThreadIdForDebug() {
	// See issue 14545
	return (int)syscall(__NR_gettid);
	// return (int)gettid();
}
