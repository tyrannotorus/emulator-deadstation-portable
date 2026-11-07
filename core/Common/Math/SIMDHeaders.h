#pragma once

// SIMD headers
// Let's include these in one consistent way across the code base.
// Here we'll also add wrappers that paper over differences between different versions
// of an instruction set, like NEON vs ASIMD (64-bit).

#pragma once

#include "ppsspp_config.h"

#include "stdint.h"
#include <string.h>

#ifdef __clang__
// Weird how you can't just use #pragma in a macro.
#define DO_NOT_VECTORIZE_LOOP _Pragma("clang loop vectorize(disable)")
#else
#define DO_NOT_VECTORIZE_LOOP
#endif


#include <arm_neon.h>


// Basic types


// No special ones here.


