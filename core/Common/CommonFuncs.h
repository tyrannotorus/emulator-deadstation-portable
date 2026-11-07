// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#pragma once

#include "ppsspp_config.h"

#include "CommonTypes.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif


#include <unistd.h>
#include <errno.h>

#define Crash() {asm ("brk #0");}



#if defined(__GNUC__) || defined(__clang__)
#define UNREACHABLE() __builtin_unreachable()
#else
#define UNREACHABLE() ((void)0)
#endif

inline u32 __rotl(u32 x, int shift) {
	shift &= 31;
	if (!shift) return x;
	return (x << shift) | (x >> (32 - shift));
}

inline u64 __rotl64(u64 x, unsigned int shift){
	unsigned int n = shift % 64;
	return (x << n) | (x >> (64 - n));
}

inline u32 __rotr(u32 x, int shift) {
    shift &= 31;
    if (!shift) return x;
    return (x >> shift) | (x << (32 - shift));
}

inline u64 __rotr64(u64 x, unsigned int shift){
	unsigned int n = shift % 64;
	return (x >> n) | (x << (64 - n));
}
