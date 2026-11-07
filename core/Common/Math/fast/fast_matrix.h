#pragma once

#include "ppsspp_config.h"

#include "Common/Math/SIMDHeaders.h"
#include "Common/Common.h"


#ifdef B0
#undef B0
#endif

// From https://developer.arm.com/documentation/102467/0100/Matrix-multiplication-example
inline void fast_matrix_mul_4x4(float *C, const float *A, const float *B) {
	// these are the columns A
	float32x4_t A0;
	float32x4_t A1;
	float32x4_t A2;
	float32x4_t A3;

	// these are the columns B
	float32x4_t B0;
	float32x4_t B1;
	float32x4_t B2;
	float32x4_t B3;

	// these are the columns C
	float32x4_t C0;
	float32x4_t C1;
	float32x4_t C2;
	float32x4_t C3;

	A0 = vld1q_f32(A);
	A1 = vld1q_f32(A + 4);
	A2 = vld1q_f32(A + 8);
	A3 = vld1q_f32(A + 12);

	// Multiply accumulate in 4x1 blocks, i.e. each column in C
	B0 = vld1q_f32(B);
	B1 = vld1q_f32(B + 4);
	B2 = vld1q_f32(B + 8);
	B3 = vld1q_f32(B + 12);

	C0 = vmulq_laneq_f32(A0, B0, 0);
	C0 = vfmaq_laneq_f32(C0, A1, B0, 1);
	C0 = vfmaq_laneq_f32(C0, A2, B0, 2);
	C0 = vfmaq_laneq_f32(C0, A3, B0, 3);
	vst1q_f32(C, C0);

	C1 = vmulq_laneq_f32(A0, B1, 0);
	C1 = vfmaq_laneq_f32(C1, A1, B1, 1);
	C1 = vfmaq_laneq_f32(C1, A2, B1, 2);
	C1 = vfmaq_laneq_f32(C1, A3, B1, 3);
	vst1q_f32(C + 4, C1);

	C2 = vmulq_laneq_f32(A0, B2, 0);
	C2 = vfmaq_laneq_f32(C2, A1, B2, 1);
	C2 = vfmaq_laneq_f32(C2, A2, B2, 2);
	C2 = vfmaq_laneq_f32(C2, A3, B2, 3);
	vst1q_f32(C + 8, C2);

	C3 = vmulq_laneq_f32(A0, B3, 0);
	C3 = vfmaq_laneq_f32(C3, A1, B3, 1);
	C3 = vfmaq_laneq_f32(C3, A2, B3, 2);
	C3 = vfmaq_laneq_f32(C3, A3, B3, 3);
	vst1q_f32(C + 12, C3);
}

