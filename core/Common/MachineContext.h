// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Note: If MACHINE_CONTEXT_SUPPORTED is not set after including this,
// there is no access to the context from exception handlers (and possibly no exception handling support).

#pragma once

#include "ppsspp_config.h"


#include <csignal>


#define MACHINE_CONTEXT_SUPPORTED

typedef sigcontext SContext;

#define CTX_REG(x) regs[x]
#define CTX_SP sp
#define CTX_PC pc



#ifdef MACHINE_CONTEXT_SUPPORTED


#endif  // MACHINE_CONTEXT_SUPPORTED
