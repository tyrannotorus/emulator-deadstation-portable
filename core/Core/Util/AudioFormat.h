// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include "ppsspp_config.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#define IS_LITTLE_ENDIAN (*(const u16 *)"\0\xff" >= 0x100)

static inline u8 clamp_u8(int i) {
	if (i > 255)
		return 255;
	if (i < 0)
		return 0;
	return i;
}

static inline s16 clamp_s16(int i) {
	if (i > 32767)
		return 32767;
	if (i < -32768)
		return -32768;
	return i;
}

static inline s16 ApplySampleVolume(s16 sample, int vol) {
	return clamp_s16((sample * vol) >> 16);
}

// We sacrifice a little volume precision to fit in 32 bits, for speed.
// Probably not worth it to make a special path for 64-bit CPUs.
static inline s16 ApplySampleVolume20Bit(s16 sample, int vol20) {
	return clamp_s16((sample * (vol20 >> 4)) >> 12);
}

void AdjustVolumeBlock(s16 *out, s16 *in, size_t size, int leftVol, int rightVol);
void ConvertS16ToF32(float *ou, const s16 *in, size_t size);
