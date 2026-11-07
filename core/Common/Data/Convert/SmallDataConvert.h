#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>

#include "Common/Common.h"
#include "ppsspp_config.h"
#include "Common/Math/SIMDHeaders.h"


extern const float one_over_255_x4[4];
extern const float exactly_255_x4[4];

// Utilities useful for filling in std140-layout uniform buffers, and similar.
// NEON intrinsics: https://developer.arm.com/documentation/den0018/a/NEON-Intrinsics?lang=en

// LSBs in f[0], etc.
inline void Uint8x4ToFloat4(float f[4], uint32_t u) {
	const uint8x8_t value = (uint8x8_t)vdup_n_u32(u);
	const uint16x8_t value16 = vmovl_u8(value);
	const uint32x4_t value32 = vmovl_u16(vget_low_u16(value16));
	const float32x4_t valueFloat = vmulq_f32(vcvtq_f32_u32(value32), vdupq_n_f32(1.0f / 255.0f));
	vst1q_f32(f, valueFloat);
}

// Could be SSE optimized.
inline uint32_t Float4ToUint8x4(const float f[4]) {
	const float32x4_t value = vmulq_f32(vld1q_f32(f), vdupq_n_f32(255.0f));
	uint32x4_t ivalue32 = vcvtq_u32_f32(value);
	uint16x4_t ivalue16 = vqmovn_u32(ivalue32);
	uint8x8_t ivalue8 = vqmovn_u16(vcombine_u16(ivalue16, ivalue16));  // Is there no way to avoid the combine here?
	uint32x2_t outValue32 = vreinterpret_u32_u8(ivalue8);
	return vget_lane_u32(outValue32, 0);
}

inline uint32_t Float4ToUint8x4_NoClamp(const float f[4]) {
	const float32x4_t value = vmulq_f32(vld1q_f32(f), vdupq_n_f32(255.0f));
	uint32x4_t ivalue32 = vcvtq_u32_f32(value);
	uint16x4_t ivalue16 = vqmovn_u32(ivalue32);
	uint8x8_t ivalue8 = vqmovn_u16(vcombine_u16(ivalue16, ivalue16));  // Is there no way to avoid the combine here?
	uint32x2_t outValue32 = vreinterpret_u32_u8(ivalue8);
	return vget_lane_u32(outValue32, 0);
}

inline void Uint8x3ToFloat4_AlphaUint8(float f[4], uint32_t u, uint8_t alpha) {
	Uint8x4ToFloat4(f, (u & 0xFFFFFF) | (alpha << 24));
}

inline void Uint8x3ToFloat4(float f[4], uint32_t u) {
	Uint8x4ToFloat4(f, u & 0xFFFFFF);
}

inline void Uint8x3ToFloat3(float f[4], uint32_t u) {
	float temp[4];
	Uint8x4ToFloat4(temp, u & 0xFFFFFF);
	f[0] = temp[0];
	f[1] = temp[1];
	f[2] = temp[2];
}

inline void Uint8x3ToInt4(int i[4], uint32_t u) {
	i[0] = ((u >> 0) & 0xFF);
	i[1] = ((u >> 8) & 0xFF);
	i[2] = ((u >> 16) & 0xFF);
	i[3] = 0;
}

inline void Uint8x3ToFloat4_Alpha(float f[4], uint32_t u, float alpha) {
	f[0] = ((u >> 0) & 0xFF) * (1.0f / 255.0f);
	f[1] = ((u >> 8) & 0xFF) * (1.0f / 255.0f);
	f[2] = ((u >> 16) & 0xFF) * (1.0f / 255.0f);
	f[3] = alpha;
}

// These are just for readability.

inline void CopyFloat2(float dest[2], const float src[2]) {
	dest[0] = src[0];
	dest[1] = src[1];
}

inline void CopyFloat3(float dest[3], const float src[3]) {
	dest[0] = src[0];
	dest[1] = src[1];
	dest[2] = src[2];
}

inline void CopyFloat4(float dest[4], const float src[4]) {
	dest[0] = src[0];
	dest[1] = src[1];
	dest[2] = src[2];
	dest[3] = src[3];
}

inline void CopyFloat1To4(float dest[4], const float src) {
	dest[0] = src;
	dest[1] = 0.0f;
	dest[2] = 0.0f;
	dest[3] = 0.0f;
}

inline void CopyFloat2To4(float dest[4], const float src[2]) {
	dest[0] = src[0];
	dest[1] = src[1];
	dest[2] = 0.0f;
	dest[3] = 0.0f;
}

inline void CopyFloat3To4(float dest[4], const float src[3]) {
	dest[0] = src[0];
	dest[1] = src[1];
	dest[2] = src[2];
	dest[3] = 0.0f;
}

inline void CopyMatrix4x4(float dest[16], const float src[16]) {
	memcpy(dest, src, sizeof(float) * 16);
}

// WARNING: This can quietly "over-load" src by 4 bytes.
inline void ExpandFloat24x3ToFloat4(float dest[4], const uint32_t src[3]) {
	const uint32x4_t values = vshlq_n_u32(vld1q_u32(src), 8);
	vst1q_u32((uint32_t *)dest, values);
}

// Note: If length is 0.0, it's gonna be left as 0.0 instead of trying to normalize. This is important.
inline void ExpandFloat24x3ToFloat4AndNormalize(float dest[4], const uint32_t src[3]) {
	float temp[4];
	ExpandFloat24x3ToFloat4(temp, src);
	// TODO: Reuse code from NormalizedOr001 and optimize
	float x = temp[0];
	float y = temp[1];
	float z = temp[2];
	float len = sqrtf(x * x + y * y + z * z);
	if (len != 0.0f)
		len = 1.0f / len;
	dest[0] = x * len;
	dest[1] = y * len;
	dest[2] = z * len;
	dest[3] = 0.0f;
}

inline uint32_t BytesToUint32(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
	return (a) | (b << 8) | (c << 16) | (d << 24);
}

constexpr int32_t SignExtend8ToS32(uint32_t value) {
	// This extends this sign at the 8th bit to the other 24 bits.
	return (int8_t)(value & 0xFF);
}

constexpr uint32_t SignExtend8ToU32(uint32_t value) {
	// Just treat the bits as unsigned.
	return (uint32_t)SignExtend8ToS32(value);
}

constexpr int32_t SignExtend16ToS32(uint32_t value) {
	// Same as SignExtend8toS32, but from the 16th bit.
	return (int16_t)(value & 0xFFFF);
}

constexpr uint32_t SignExtend16ToU32(uint32_t value) {
	return (uint32_t)SignExtend16ToS32(value);
}
