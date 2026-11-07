// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// The corresponding file is called MemTools in the Dolphin project.

#include "ppsspp_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/MachineContext.h"
#include "Common/ExceptionHandlerSetup.h"

static BadAccessHandler g_badAccessHandler;
static void *altStack = nullptr;

#ifdef MACHINE_CONTEXT_SUPPORTED

// We cannot handle exceptions in UWP builds. Bleh.

#include <signal.h>

static struct sigaction old_sa_segv;
static struct sigaction old_sa_bus;

static void sigsegv_handler(int sig, siginfo_t* info, void* raw_context) {
	if (sig != SIGSEGV && sig != SIGBUS) {
		// We are not interested in other signals - handle it as usual.
		return;
	}
	ucontext_t* context = (ucontext_t*)raw_context;
	int sicode = info->si_code;
	if (sicode != SEGV_MAPERR && sicode != SEGV_ACCERR) {
		// Huh? Return.
		return;
	}
	uintptr_t bad_address = (uintptr_t)info->si_addr;

	// Get all the information we can out of the context.
	mcontext_t* ctx = &context->uc_mcontext;
	// assume it's not a write
	if (!g_badAccessHandler(bad_address,
		ctx
	)) {
		// retry and crash
		// According to the sigaction man page, if sa_flags "SA_SIGINFO" is set to the sigaction
		// function pointer, otherwise sa_handler contains one of:
		// SIG_DEF: The 'default' action is performed
		// SIG_IGN: The signal is ignored
		// Any other value is a function pointer to a signal handler

		struct sigaction* old_sa;
		if (sig == SIGSEGV) {
			old_sa = &old_sa_segv;
		} else {
			old_sa = &old_sa_bus;
		}

		if (old_sa->sa_flags & SA_SIGINFO) {
			old_sa->sa_sigaction(sig, info, raw_context);
			return;
		}
		if (old_sa->sa_handler == SIG_DFL) {
			signal(sig, SIG_DFL);
			return;
		}
		if (old_sa->sa_handler == SIG_IGN) {
			// Ignore signal
			return;
		}
		old_sa->sa_handler(sig);
	}
}

void InstallExceptionHandler(BadAccessHandler badAccessHandler) {
	if (!badAccessHandler) {
		return;
	}
	if (g_badAccessHandler) {
		g_badAccessHandler = badAccessHandler;
		return;
	}
	
	size_t altStackSize = SIGSTKSZ;

	// Add some extra room.
	altStackSize += 65536;

	INFO_LOG(Log::System, "Installed exception handler. stack size: %d", (int)altStackSize);
	g_badAccessHandler = badAccessHandler;

	stack_t signal_stack{};
	altStack = malloc(altStackSize);
	signal_stack.ss_sp = altStack;
	signal_stack.ss_size = altStackSize;
	signal_stack.ss_flags = 0;
	if (sigaltstack(&signal_stack, nullptr)) {
		_assert_msg_(false, "sigaltstack failed");
	}
	struct sigaction sa{};
	sa.sa_handler = nullptr;
	sa.sa_sigaction = &sigsegv_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old_sa_segv);
}

void UninstallExceptionHandler() {
	if (!g_badAccessHandler) {
		return;
	}
	stack_t signal_stack{};
	signal_stack.ss_flags = SS_DISABLE;
	if (0 != sigaltstack(&signal_stack, nullptr)) {
		ERROR_LOG(Log::System, "Could not remove signal altstack");
	}
	if (altStack) {
		free(altStack);
		altStack = nullptr;
	}
	sigaction(SIGSEGV, &old_sa_segv, nullptr);
	INFO_LOG(Log::System, "Uninstalled exception handler");
	g_badAccessHandler = nullptr;
}


#else  // !MACHINE_CONTEXT_SUPPORTED

void InstallExceptionHandler(BadAccessHandler badAccessHandler) {
	ERROR_LOG(Log::System, "Exception handler not implemented on this platform, can't install");
}
void UninstallExceptionHandler() { }

#endif  // MACHINE_CONTEXT_SUPPORTED
