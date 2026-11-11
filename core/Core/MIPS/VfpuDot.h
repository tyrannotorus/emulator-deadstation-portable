// The VFPU's dot product as the PSP computes it (vdot, vmmul with MoreAccurateVMMUL, vtfm, vhdp in the interpreter):
// products of 24-bit mantissas with 2 extra bits, aligned to the largest exponent with truncation, summed as integers,
// then rounded to nearest even. Shared by MIPSVFPUUtils.cpp and its test (local/brick/vfpudot-test.cpp), which checks the
// NEON version against the reference bit for bit. Self-contained: no emulator headers.
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace vfpudot {

inline uint32_t bitsOf(float f) {
	uint32_t u;
	std::memcpy(&u, &f, 4);
	return u;
}

inline float floatOf(uint32_t u) {
	float f;
	std::memcpy(&f, &u, 4);
	return f;
}

constexpr int EXTRA_BITS = 2;

// the sum's sign, normalization and rounding: the reference's tail, shared by both versions
inline float finish(int32_t mant_sum, int32_t max_exp) {
	uint32_t sign_sum = 0;
	if (mant_sum < 0) {
		sign_sum = 0x80000000;
		mant_sum = -mant_sum;
	}

	// Truncate off the extra bits now.  We want to zero them for rounding purposes.
	mant_sum >>= EXTRA_BITS;

	if (mant_sum == 0 || max_exp <= 0) {
		return 0.0f;
	}

	int8_t shift = (int8_t)__builtin_clz((uint32_t)mant_sum) - 8;
	if (shift < 0) {
		// Round to even if we'd shift away a 0.5.
		const uint32_t round_bit = 1 << (-shift - 1);
		if ((mant_sum & round_bit) && (mant_sum & (round_bit << 1))) {
			mant_sum += round_bit;
			shift = (int8_t)__builtin_clz((uint32_t)mant_sum) - 8;
		} else if ((mant_sum & round_bit) && (mant_sum & (round_bit - 1))) {
			mant_sum += round_bit;
			shift = (int8_t)__builtin_clz((uint32_t)mant_sum) - 8;
		}
		mant_sum >>= -shift;
		max_exp += -shift;
	} else {
		mant_sum <<= shift;
		max_exp -= shift;
	}

	if (max_exp >= 255) {
		max_exp = 255;
		mant_sum = 0;
	} else if (max_exp <= 0) {
		return 0.0f;
	}

	return floatOf(sign_sum | ((uint32_t)max_exp << 23) | ((uint32_t)mant_sum & 0x007FFFFF));
}

// Reference C++ version.
inline float reference(const float a[4], const float b[4]) {
	int32_t exps[4];
	int32_t mants[4];
	int32_t signs[4];
	int32_t max_exp = 0;
	int32_t last_inf = -1;

	for (int i = 0; i < 4; i++) {
		const uint32_t ai = bitsOf(a[i]), bi = bitsOf(b[i]);

		int32_t aexp = (ai >> 23) & 0xFF;
		int32_t bexp = (bi >> 23) & 0xFF;
		int32_t amant = (int32_t)((ai & 0x007FFFFF) | 0x00800000) << EXTRA_BITS;
		int32_t bmant = (int32_t)((bi & 0x007FFFFF) | 0x00800000) << EXTRA_BITS;

		exps[i] = aexp + bexp - 127;
		if (aexp == 255) {
			// INF * 0 = NAN
			if ((ai & 0x007FFFFF) != 0 || bexp == 0) {
				return floatOf(0x7F800001);
			}
			mants[i] = 0x00800000 << EXTRA_BITS;
			exps[i] = 255;
		} else if (bexp == 255) {
			if ((bi & 0x007FFFFF) != 0 || aexp == 0) {
				return floatOf(0x7F800001);
			}
			mants[i] = 0x00800000 << EXTRA_BITS;
			exps[i] = 255;
		} else {
			// TODO: Adjust precision?
			uint64_t adjust = (uint64_t)amant * (uint64_t)bmant;
			mants[i] = (adjust >> (23 + EXTRA_BITS)) & 0x7FFFFFFF;
		}
		signs[i] = (int32_t)((ai ^ bi) & 0x80000000);

		if (exps[i] > max_exp) {
			max_exp = exps[i];
		}
		if (exps[i] >= 255) {
			// Infinity minus infinity is not a real number.
			if (last_inf != -1 && signs[i] != last_inf) {
				return floatOf(0x7F800001);
			}
			last_inf = signs[i];
		}
	}

	int32_t mant_sum = 0;
	for (int i = 0; i < 4; i++) {
		int exp = max_exp - exps[i];
		if (exp >= 32) {
			mants[i] = 0;
		} else {
			mants[i] >>= exp;
		}
		if (signs[i]) {
			mants[i] = -mants[i];
		}
		mant_sum += mants[i];
	}
	return finish(mant_sum, max_exp);
}

#if defined(__aarch64__)
// The same arithmetic, the four products at once, when every one is finite: no input with exponent 255 and no product
// exponent at or past 255. The rest (infinities, NaNs, overflow) goes to the reference.
// Each step is the reference's: exponents aexp + bexp - 127; mantissas with the hidden 1 (denormals too), times 4, their
// 64-bit product >> 25 (under 2^27, so its & 0x7FFFFFFF changes nothing); the largest exponent, at least 0; each mantissa
// >> (that - its exponent), 0 at 32 or more; negated where the signs differ; summed (under 2^29: no overflow).
inline float neon(const float a[4], const float b[4]) {
	const uint32x4_t ua = vld1q_u32((const uint32_t *)a), ub = vld1q_u32((const uint32_t *)b);
	const uint32x4_t expMask = vdupq_n_u32(0xFF);
	const int32x4_t aexp = vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(ua, 23), expMask));
	const int32x4_t bexp = vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(ub, 23), expMask));
	const int32x4_t exps = vsubq_s32(vaddq_s32(aexp, bexp), vdupq_n_s32(127));
	// an input exponent of 255 (the largest of them, as none is over 255), or a product's of 255 or more: the reference
	int32_t max_exp = vmaxvq_s32(exps);
	if (vmaxvq_s32(vmaxq_s32(aexp, bexp)) == 255 || max_exp >= 255)
		return reference(a, b);

	const uint32x4_t fracMask = vdupq_n_u32(0x007FFFFF), hidden = vdupq_n_u32(0x00800000);
	const uint32x4_t amant = vshlq_n_u32(vorrq_u32(vandq_u32(ua, fracMask), hidden), EXTRA_BITS);
	const uint32x4_t bmant = vshlq_n_u32(vorrq_u32(vandq_u32(ub, fracMask), hidden), EXTRA_BITS);
	const uint64x2_t productLow = vmull_u32(vget_low_u32(amant), vget_low_u32(bmant));
	const uint64x2_t productHigh = vmull_high_u32(amant, bmant);
	uint32x4_t mants = vcombine_u32(vshrn_n_u64(productLow, 23 + EXTRA_BITS), vshrn_n_u64(productHigh, 23 + EXTRA_BITS));

	if (max_exp < 0)
		max_exp = 0;
	int32x4_t shift = vsubq_s32(vdupq_n_s32(max_exp), exps);
	const uint32x4_t gone = vcgeq_s32(shift, vdupq_n_s32(32));
	shift = vminq_s32(shift, vdupq_n_s32(31));
	mants = vbicq_u32(vshlq_u32(mants, vnegq_s32(shift)), gone);

	const uint32x4_t negative = vtstq_u32(veorq_u32(ua, ub), vdupq_n_u32(0x80000000));
	const int32x4_t magnitude = vreinterpretq_s32_u32(mants);
	const int32x4_t signedMants = vbslq_s32(negative, vnegq_s32(magnitude), magnitude);
	return finish(vaddvq_s32(signedMants), max_exp);
}

// finish() for four sums at once, each lane its arithmetic: the sign; nothing left or no exponent, 0.0; over 24 bits, rounded to
// nearest (ties to even) at the last bit the shift drops; normalized to 24 bits (a negative shift is a right shift); an exponent
// of 255 or more, infinity; 0 or less, 0.0.
inline void finish4(int32x4_t sum, int32x4_t maxExp, float out[4]) {
	const uint32x4_t sign = vandq_u32(vreinterpretq_u32_s32(sum), vdupq_n_u32(0x80000000));
	uint32x4_t m = vshrq_n_u32(vreinterpretq_u32_s32(vabsq_s32(sum)), EXTRA_BITS);
	uint32x4_t zero = vorrq_u32(vceqzq_u32(m), vclezq_s32(maxExp));
	const int32x4_t firstShift = vsubq_s32(vreinterpretq_s32_u32(vclzq_u32(m)), vdupq_n_s32(8));
	const uint32x4_t roundBit = vshlq_u32(vdupq_n_u32(1), vsubq_s32(vnegq_s32(firstShift), vdupq_n_s32(1)));
	const uint32x4_t others = vorrq_u32(vshlq_n_u32(roundBit, 1), vsubq_u32(roundBit, vdupq_n_u32(1)));
	const uint32x4_t roundUp = vandq_u32(vcltzq_s32(firstShift), vandq_u32(vtstq_u32(m, roundBit), vtstq_u32(m, others)));
	m = vaddq_u32(m, vandq_u32(roundUp, roundBit));
	const int32x4_t shift = vsubq_s32(vreinterpretq_s32_u32(vclzq_u32(m)), vdupq_n_s32(8));
	m = vshlq_u32(m, shift);
	int32x4_t exp = vsubq_s32(maxExp, shift);
	const uint32x4_t inf = vcgeq_s32(exp, vdupq_n_s32(255));
	zero = vorrq_u32(zero, vclezq_s32(exp));
	exp = vminq_s32(exp, vdupq_n_s32(255));
	m = vbicq_u32(m, inf);
	const uint32x4_t bits = vorrq_u32(sign, vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(exp), 23), vandq_u32(m, vdupq_n_u32(0x007FFFFF))));
	vst1q_f32(out, vreinterpretq_f32_u32(vbicq_u32(bits, zero)));
}

// Four dots sharing b: out[r] = dot(rows + 4r, b), r 0 to 3 (vmmul: one column of T against S's rows). A lane each, so each
// lane is the reference's arithmetic for its row with nothing across lanes; its sign, rounding and exponent the scalar
// tail's. Any input of exponent 255 or product of 255 or more in any lane: the reference for all four.
inline void neon4(const float rows[16], const float b[4], float out[4]) {
	const uint32x4x4_t r = vld4q_u32((const uint32_t *)rows);
	const uint32x4_t ub4 = vld1q_u32((const uint32_t *)b);
	const uint32x4_t expMask = vdupq_n_u32(0xFF), fracMask = vdupq_n_u32(0x007FFFFF), hidden = vdupq_n_u32(0x00800000);
	const uint32x4_t signBit = vdupq_n_u32(0x80000000);
	int32x4_t exps[4];
	uint32x4_t mants[4], negative[4];
	int32x4_t maxExp = vdupq_n_s32(0);
	int32x4_t maxInput = vdupq_n_s32(0);
	for (int k = 0; k < 4; k++) {
		const uint32x4_t ua = r.val[k];
		const uint32x4_t ubk = k == 0 ? vdupq_laneq_u32(ub4, 0) : k == 1 ? vdupq_laneq_u32(ub4, 1) : k == 2 ? vdupq_laneq_u32(ub4, 2) : vdupq_laneq_u32(ub4, 3);
		const int32x4_t aexp = vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(ua, 23), expMask));
		const int32x4_t bexp = vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(ubk, 23), expMask));
		exps[k] = vsubq_s32(vaddq_s32(aexp, bexp), vdupq_n_s32(127));
		maxExp = vmaxq_s32(maxExp, exps[k]);
		maxInput = vmaxq_s32(maxInput, vmaxq_s32(aexp, bexp));
		const uint32x4_t amant = vshlq_n_u32(vorrq_u32(vandq_u32(ua, fracMask), hidden), EXTRA_BITS);
		const uint32x4_t bmant = vshlq_n_u32(vorrq_u32(vandq_u32(ubk, fracMask), hidden), EXTRA_BITS);
		const uint64x2_t productLow = vmull_u32(vget_low_u32(amant), vget_low_u32(bmant));
		const uint64x2_t productHigh = vmull_high_u32(amant, bmant);
		mants[k] = vcombine_u32(vshrn_n_u64(productLow, 23 + EXTRA_BITS), vshrn_n_u64(productHigh, 23 + EXTRA_BITS));
		negative[k] = vtstq_u32(veorq_u32(ua, ubk), signBit);
	}
	if (vmaxvq_s32(maxInput) == 255 || vmaxvq_s32(maxExp) >= 255) {
		for (int i = 0; i < 4; i++)
			out[i] = reference(rows + 4 * i, b);
		return;
	}
	int32x4_t sum = vdupq_n_s32(0);
	for (int k = 0; k < 4; k++) {
		int32x4_t shift = vsubq_s32(maxExp, exps[k]);
		const uint32x4_t gone = vcgeq_s32(shift, vdupq_n_s32(32));
		shift = vminq_s32(shift, vdupq_n_s32(31));
		const int32x4_t magnitude = vreinterpretq_s32_u32(vbicq_u32(vshlq_u32(mants[k], vnegq_s32(shift)), gone));
		sum = vaddq_s32(sum, vbslq_s32(negative[k], vnegq_s32(magnitude), magnitude));
	}
	finish4(sum, maxExp, out);
}
#endif

inline float dot(const float a[4], const float b[4]) {
#if defined(__aarch64__)
	return neon(a, b);
#else
	return reference(a, b);
#endif
}

inline void dot4(const float rows[16], const float b[4], float out[4]) {
#if defined(__aarch64__)
	neon4(rows, b, out);
#else
	for (int i = 0; i < 4; i++)
		out[i] = reference(rows + 4 * i, b);
#endif
}

}  // namespace vfpudot
