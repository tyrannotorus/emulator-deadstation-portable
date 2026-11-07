#pragma once

#include "ppsspp_config.h"
#include <cstdint>


// Use this if you know the value is non-zero.
inline uint32_t clz32_nonzero(uint32_t value) {
	return __builtin_clz(value);
}

inline uint32_t clz32(uint32_t value) {
	if (!value)
		return 32;
	return __builtin_clz(value);
}

