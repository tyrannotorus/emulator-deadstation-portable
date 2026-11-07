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

#include "GPU/Math3D.h"
#include "Common/Common.h"
#include "Common/Math/SIMDHeaders.h"


namespace Math3D {

template<>
float Vec2<float>::Length() const
{
	// Doubt this is worth it for a vec2 :/
	float32x2_t vec = vld1_f32(&x);
	float32x2_t sq = vmul_f32(vec, vec);
	float32x2_t add2 = vpadd_f32(sq, sq);
	float32x2_t res = vsqrt_f32(add2);
	return vget_lane_f32(res, 0);
}

template<>
void Vec2<float>::SetLength(const float l)
{
	(*this) *= l / Length();
}

template<>
Vec2<float> Vec2<float>::WithLength(const float l) const
{
	return (*this) * l / Length();
}

template<>
float Vec2<float>::Distance2To(const Vec2<float> &other) const {
	return Vec2<float>(other-(*this)).Length2();
}

template<>
Vec2<float> Vec2<float>::Normalized() const
{
	return (*this) / Length();
}

template<>
float Vec2<float>::Normalize()
{
	float len = Length();
	(*this) = (*this)/len;
	return len;
}

template<>
float Vec3<float>::Length() const
{
	float32x4_t sq = vsetq_lane_f32(0.0f, vmulq_f32(vec, vec), 3);
	float32x2_t add1 = vget_low_f32(vpaddq_f32(sq, sq));
	float32x2_t add2 = vpadd_f32(add1, add1);
	float32x2_t res = vsqrt_f32(add2);
	return vget_lane_f32(res, 0);
}

template<>
void Vec3<float>::SetLength(const float l)
{
	(*this) *= l / Length();
}

template<>
Vec3<float> Vec3<float>::WithLength(const float l) const
{
	return (*this) * l / Length();
}

template<>
float Vec3<float>::Distance2To(const Vec3<float> &other) const {
	return Vec3<float>(other-(*this)).Length2();
}

template<>
Vec3<float> Vec3<float>::Normalized(bool useSSE4) const {
	float32x4_t sq = vsetq_lane_f32(0.0f, vmulq_f32(vec, vec), 3);
	float32x2_t add1 = vget_low_f32(vpaddq_f32(sq, sq));
	float32x2_t summed = vpadd_f32(add1, add1);

	float32x2_t e = vrsqrte_f32(summed);
	e = vmul_f32(vrsqrts_f32(vmul_f32(e, e), summed), e);
	e = vmul_f32(vrsqrts_f32(vmul_f32(e, e), summed), e);

	float32x4_t factor = vdupq_lane_f32(e, 0);
	return Vec3<float>(vmulq_f32(vec, factor));
}

template<>
Vec3<float> Vec3<float>::NormalizedOr001(bool useSSE4) const {
	float32x4_t sq = vsetq_lane_f32(0.0f, vmulq_f32(vec, vec), 3);
	float32x2_t add1 = vget_low_f32(vpaddq_f32(sq, sq));
	float32x2_t summed = vpadd_f32(add1, add1);
	if (vget_lane_f32(summed, 0) == 0.0f) {
		return Vec3<float>(vsetq_lane_f32(1.0f, vdupq_lane_f32(summed, 0), 2));
	}

	float32x2_t e = vrsqrte_f32(summed);
	e = vmul_f32(vrsqrts_f32(vmul_f32(e, e), summed), e);
	e = vmul_f32(vrsqrts_f32(vmul_f32(e, e), summed), e);

	float32x4_t factor = vdupq_lane_f32(e, 0);
	return Vec3<float>(vmulq_f32(vec, factor));
}

template<>
float Vec3<float>::Normalize()
{
	float len = Length();
	(*this) = (*this)/len;
	return len;
}

template<>
float Vec3<float>::NormalizeOr001() {
	float len = Length();
	if (len == 0.0f) {
		z = 1.0f;
	} else {
		*this /= len;
	}
	return len;
}

template<>
Vec3Packed<float> Vec3Packed<float>::FromRGB(unsigned int rgb)
{
	return Vec3Packed((rgb & 0xFF) * (1.0f/255.0f),
				((rgb >> 8) & 0xFF) * (1.0f/255.0f),
				((rgb >> 16) & 0xFF) * (1.0f/255.0f));
}

template<>
Vec3Packed<int> Vec3Packed<int>::FromRGB(unsigned int rgb)
{
	return Vec3Packed(rgb & 0xFF, (rgb >> 8) & 0xFF, (rgb >> 16) & 0xFF);
}

template<>
unsigned int Vec3Packed<float>::ToRGB() const
{
	return ((unsigned int)(r()*255.f)) +
			((unsigned int)(g()*255.f*256.f)) +
			((unsigned int)(b()*255.f*256.f*256.f));
}

template<>
unsigned int Vec3Packed<int>::ToRGB() const
{
	return (r()&0xFF) | ((g()&0xFF)<<8) | ((b()&0xFF)<<16);
}

template<>
float Vec3Packed<float>::Length() const
{
	return sqrtf(Length2());
}

template<>
void Vec3Packed<float>::SetLength(const float l)
{
	(*this) *= l / Length();
}

template<>
Vec3Packed<float> Vec3Packed<float>::WithLength(const float l) const
{
	return (*this) * l / Length();
}

template<>
float Vec3Packed<float>::Distance2To(const Vec3Packed<float> &other) const {
	return Vec3Packed<float>(other-(*this)).Length2();
}

template<>
Vec3Packed<float> Vec3Packed<float>::Normalized() const
{
	return (*this) / Length();
}

template<>
float Vec3Packed<float>::Normalize()
{
	float len = Length();
	(*this) = (*this)/len;
	return len;
}

template<>
float Vec4<float>::Length() const
{
	float32x4_t sq = vmulq_f32(vec, vec);
	float32x2_t add1 = vget_low_f32(vpaddq_f32(sq, sq));
	float32x2_t add2 = vpadd_f32(add1, add1);
	float32x2_t res = vsqrt_f32(add2);
	return vget_lane_f32(res, 0);
}

template<>
void Vec4<float>::SetLength(const float l)
{
	(*this) *= l / Length();
}

template<>
Vec4<float> Vec4<float>::WithLength(const float l) const
{
	return (*this) * l / Length();
}

template<>
float Vec4<float>::Distance2To(const Vec4<float> &other) const {
	return Vec4<float>(other-(*this)).Length2();
}

template<>
Vec4<float> Vec4<float>::Normalized() const
{
	return (*this) / Length();
}

template<>
float Vec4<float>::Normalize()
{
	float len = Length();
	(*this) = (*this)/len;
	return len;
}

}; // namespace Math3D
