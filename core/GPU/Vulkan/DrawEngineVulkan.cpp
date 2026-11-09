#include "Core/Config.h"
#include "Core/MemMap.h"
#include "Core/System.h"
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

#include <algorithm>
#include <cmath>
#include "GPU/Common/ShaderId.h"
#include <cstdlib>
#include <cstdio>
#include "ppsspp_config.h"
#include <functional>

#include "Common/GPU/Vulkan/VulkanRenderManager.h"

#include "Common/Log.h"

#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Math3D.h"

#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"

#include "GPU/GPUCommon.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/ShaderUniforms.h"
#include "GPU/Vulkan/DrawEngineVulkan.h"
#include "GPU/Vulkan/TextureCacheVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"
#include "GPU/Vulkan/PipelineManagerVulkan.h"
#include "GPU/Vulkan/FramebufferManagerVulkan.h"

using namespace PPSSPP_VK;

enum {
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

DrawEngineVulkan::DrawEngineVulkan(Draw::DrawContext *draw)
	: draw_(draw) {
	decOptions_.expandAllWeightsToFloat = false;
	decOptions_.expand8BitNormalsToFloat = false;
}

// Does the draw leave depth and stencil exactly as they were?
static bool DrawKeepsDepthStencil() {
	if (gstate.isModeClear())
		return false;
	if (gstate.isDepthTestEnabled() && gstate.isDepthWriteEnabled())
		return false;
	if (gstate.isStencilTestEnabled() && (gstate.getStencilOpSFail() != GE_STENCILOP_KEEP || gstate.getStencilOpZFail() != GE_STENCILOP_KEEP || gstate.getStencilOpZPass() != GE_STENCILOP_KEEP))
		return false;
	return true;
}

void DrawEngineVulkan::InitDeviceObjects() {
	// All resources we need for PSP drawing. Usually only bindings 0 and 2-4 are populated.

	BindingType bindingTypes[VKRPipelineLayout::MAX_DESC_SET_BINDINGS] = {
		BindingType::COMBINED_IMAGE_SAMPLER,  // main
		BindingType::COMBINED_IMAGE_SAMPLER,  // framebuffer-read
		BindingType::COMBINED_IMAGE_SAMPLER,  // palette
		BindingType::UNIFORM_BUFFER_DYNAMIC_ALL,  // uniforms
		BindingType::UNIFORM_BUFFER_DYNAMIC_VERTEX,  // lights
		BindingType::UNIFORM_BUFFER_DYNAMIC_VERTEX,  // bones
		BindingType::STORAGE_BUFFER_VERTEX,  // tess
		BindingType::STORAGE_BUFFER_VERTEX,
		BindingType::STORAGE_BUFFER_VERTEX,
	};

	VulkanContext *vulkan = (VulkanContext *)draw_->GetNativeObject(Draw::NativeObject::CONTEXT);
	VkDevice device = vulkan->GetDevice();

	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	// Deadstation Portable: images and storage buffers pushed at each draw when the device has push descriptors (set 1 in the shaders)
	const bool push = vulkan->Extensions().KHR_push_descriptor;
	g_drawImageSet = push ? 1 : 0;
	pipelineLayout_ = renderManager->CreatePipelineLayout(bindingTypes, ARRAY_SIZE(bindingTypes), draw_->GetDeviceCaps().geometryShaderSupported, "drawengine_layout", push);

	pushUBO_ = (VulkanPushPool *)draw_->GetNativeObject(Draw::NativeObject::PUSH_POOL);
	pushVertex_ = new VulkanPushPool(vulkan, "pushVertex", 4 * 1024 * 1024, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	pushIndex_ = new VulkanPushPool(vulkan, "pushIndex", 512 * 1024, 64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

	VkSamplerCreateInfo samp{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.magFilter = VK_FILTER_LINEAR;
	samp.minFilter = VK_FILTER_LINEAR;
	samp.maxLod = VK_LOD_CLAMP_NONE;  // recommended by best practices, has no effect since we don't use mipmaps.
	VkResult res = vkCreateSampler(device, &samp, nullptr, &samplerSecondaryLinear_);
	samp.magFilter = VK_FILTER_NEAREST;
	samp.minFilter = VK_FILTER_NEAREST;
	res = vkCreateSampler(device, &samp, nullptr, &samplerSecondaryNearest_);
	_dbg_assert_(VK_SUCCESS == res);
	res = vkCreateSampler(device, &samp, nullptr, &nullSampler_);
	_dbg_assert_(VK_SUCCESS == res);

	tessDataTransferVulkan = new TessellationDataTransferVulkan(vulkan);
	tessDataTransfer = tessDataTransferVulkan;

	draw_->SetInvalidationCallback(std::bind(&DrawEngineVulkan::Invalidate, this, std::placeholders::_1));
}

DrawEngineVulkan::~DrawEngineVulkan() {
	DestroyDeviceObjects();
}

void DrawEngineVulkan::DestroyDeviceObjects() {
	if (!draw_) {
		// We've already done this from LostDevice.
		return;
	}

	VulkanContext *vulkan = (VulkanContext *)draw_->GetNativeObject(Draw::NativeObject::CONTEXT);
	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);

	draw_->SetInvalidationCallback(InvalidationCallback());

	delete tessDataTransferVulkan;
	tessDataTransfer = nullptr;
	tessDataTransferVulkan = nullptr;

	pushUBO_ = nullptr;

	if (pushVertex_) {
		pushVertex_->Destroy();
		delete pushVertex_;
		pushVertex_ = nullptr;
	}
	if (pushIndex_) {
		pushIndex_->Destroy();
		delete pushIndex_;
		pushIndex_ = nullptr;
	}

	if (samplerSecondaryNearest_ != VK_NULL_HANDLE)
		vulkan->Delete().QueueDeleteSampler(samplerSecondaryNearest_);
	if (samplerSecondaryLinear_ != VK_NULL_HANDLE)
		vulkan->Delete().QueueDeleteSampler(samplerSecondaryLinear_);
	if (nullSampler_ != VK_NULL_HANDLE)
		vulkan->Delete().QueueDeleteSampler(nullSampler_);

	renderManager->DestroyPipelineLayout(pipelineLayout_);
}

void DrawEngineVulkan::DeviceLost() {
	DestroyDeviceObjects();
	DirtyAllUBOs();
	draw_ = nullptr;
}

void DrawEngineVulkan::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	InitDeviceObjects();
}

void DrawEngineVulkan::BeginFrame() {
	DrawEngineCommon::BeginFrame();

	lastPipeline_ = nullptr;

	// These will be re-bound if needed, let's not let old bindings linger around too long.
	boundDepal_ = VK_NULL_HANDLE;
	boundSecondary_ = VK_NULL_HANDLE;

	// pushUBO is the thin3d push pool, don't need to BeginFrame again.
	pushVertex_->BeginFrame();
	pushIndex_->BeginFrame();

	tessDataTransferVulkan->SetPushPool(pushUBO_);

	DirtyAllUBOs();

	AssertEmpty();
}

void DrawEngineVulkan::EndFrame() {
	stats_.pushVertexSpaceUsed = (int)pushVertex_->GetUsedThisFrame();
	stats_.pushIndexSpaceUsed = (int)pushIndex_->GetUsedThisFrame();

	AssertEmpty();
}

void DrawEngineVulkan::DirtyAllUBOs() {
	baseUBOOffset = 0;
	lightUBOOffset = 0;
	boneUBOOffset = 0;
	baseBuf = VK_NULL_HANDLE;
	lightBuf = VK_NULL_HANDLE;
	boneBuf = VK_NULL_HANDLE;
	dirtyUniforms_ = DIRTY_BASE_UNIFORMS | DIRTY_LIGHT_UNIFORMS | DIRTY_BONE_UNIFORMS;
	imageView = VK_NULL_HANDLE;
	sampler = VK_NULL_HANDLE;
	gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
}

void DrawEngineVulkan::Invalidate(InvalidationCallbackFlags flags) {
	if (flags & InvalidationCallbackFlags::COMMAND_BUFFER_STATE) {
		// Nothing here anymore (removed the "frame descriptor set"
		// If we add back "seldomly-changing" descriptors, we might use this again.
	}
	if (flags & InvalidationCallbackFlags::RENDER_PASS_STATE) {
		// If have a new render pass, dirty our dynamic state so it gets re-set.
		//
		// Dirty everything that has dynamic state that will need re-recording.
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_BLEND_STATE | DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
		lastPipeline_ = nullptr;
	}
}

// God of War's effects, every frame: the scene is copied small into an effect buffer, the copy blended back over part of the scene, the buffer then
// overwritten from the scene and blurred, and that added to the scene. Each of those is a render pass of its own, and passes are what hurts on this
// hardware, however small: about 0.25 ms of the CPU's frame each. So the copy goes in the buffer's lower half, where what follows doesn't overwrite it,
// and the blend waits for the scene's last pass. What gets blurred is then the scene without the blend: a blur of nearly the same picture.
// Only once a frame has shown this is what's going on (the copy is only needed for the blend, and what follows overwrites it either way).
namespace {
struct EffectBlend {
	u32 buffer = 0, scene = 0;	// the last possible copy
	u32 knownBuffer = 0;
	u32 knownFlip = 0;
	int shiftY = 0;				// where this frame's copy went
	bool skipBlur = false;
	int copyShiftY = 0;			// where the last copy went
} effectBlend;
}

// The inline wrapper in the header checks for numDrawCalls_ == 0
// Deadstation Portable: a texture projected by the texture matrix from the model-space position, clamped in s or t: the
// fragment shader reads it at s = p.M0 / p.M2, t = p.M1 / p.M2 (M the texture matrix), clamped to [0.5 / w, 1 - 0.5 / w] (and
// h for t) in the texture's own texels. A triangle whose three corners have q = p.M2 > 0 and are all past the same clamped
// edge has every pixel past it too (s - edge * q has the corners' sign inside the triangle, q stays positive), so every pixel
// reads only that edge's texels. God of War projects its shadow pictures this way onto the ground and walls around each
// fighter: at 960x544, most of the GPU's frame went on triangles reading only the picture's empty border.
bool DrawEngineVulkan::projectedCullable() const {
	return !gstate.isModeThrough() && gstate.isTextureMapEnabled() && gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_MATRIX
		&& gstate.getUVProjMode() == GE_PROJMAP_POSITION && (gstate.isTexCoordClampedS() || gstate.isTexCoordClampedT())
		&& gstate_c.submitType != SubmitType::HW_BEZIER && gstate_c.submitType != SubmitType::HW_SPLINE;
}

// The triangles of decIndex_ (indices into decoded_) that read more than the edge; returns the indices left.
int DrawEngineVulkan::cullProjected(int indexCount) {
	const DecVtxFormat &fmt = dec_->GetDecVtxFmt();
	const float sLo = 0.5f / (float)gstate.getTextureWidth(0), sHi = 1.0f - sLo;
	const float tLo = 0.5f / (float)gstate.getTextureHeight(0), tHi = 1.0f - tLo;
	const bool clampS = gstate.isTexCoordClampedS(), clampT = gstate.isTexCoordClampedT();
	// per vertex: which edges it is past (bit 0 s low, 1 s high, 2 t low, 3 t high), 0 if q <= 0
	static u8 past[65536];
	const int verts = std::min(numDecodedVerts_, 65536);
	projVerts_ = verts;
	for (int i = 0; i < verts; i++) {
		const float *pos = (const float *)(decoded_ + i * fmt.stride + fmt.posoff);
		float *uvq = projUvq_ + i * 3;
		Vec3ByMatrix43(uvq, pos, gstate.tgenMatrix);
		memcpy(projPos_ + i * 3, pos, 12);
		u8 bits = 0;
		if (uvq[2] > 0.0f) {
			if (clampS) {
				bits |= uvq[0] < sLo * uvq[2] ? 1 : 0;
				bits |= uvq[0] > sHi * uvq[2] ? 2 : 0;
			}
			if (clampT) {
				bits |= uvq[1] < tLo * uvq[2] ? 4 : 0;
				bits |= uvq[1] > tHi * uvq[2] ? 8 : 0;
			}
		}
		past[i] = bits;
	}
	int kept = 0;
	for (int i = 0; i + 2 < indexCount; i += 3) {
		const u16 a = decIndex_[i], b = decIndex_[i + 1], c = decIndex_[i + 2];
		if (a >= verts || b >= verts || c >= verts || (past[a] & past[b] & past[c]) == 0) {
			decIndex_[kept++] = a;
			decIndex_[kept++] = b;
			decIndex_[kept++] = c;
		}
	}
	return kept;
}

// The screen box of the pixels of the kept triangles that read more than the texture's edge (cullProjected), as the GPU
// places them (the uploaded u_mvp and the draw's viewport): each triangle clipped to q > 0 and the clamped s and t ranges,
// and to w > 0 (the GPU's own clip), its corners projected. False when it can't say (a corner behind the projector).
bool DrawEngineVulkan::projectedFootprint(int indexCount, ScissorRect &box) {
	const UB_VS_FS_Base &ub = shaderManager_->BaseUniforms();
	const VkViewport &vp = dynState_.viewport;
	const float sLo = 0.5f / (float)gstate.getTextureWidth(0), sHi = 1.0f - sLo;
	const float tLo = 0.5f / (float)gstate.getTextureHeight(0), tHi = 1.0f - tLo;
	const bool clampS = gstate.isTexCoordClampedS(), clampT = gstate.isTexCoordClampedT();
	struct V { float c[4]; float u, v, q; };
	// each vertex once: its clip position, its screen position, and the planes it is outside (bits 0-3 the clamped s and t
	// ranges, 4 w <= 0; 32: behind the projector, q <= 0)
	static float clip[65536][4];
	static float screen[65536][2];
	static u8 outside[65536];
	const int verts = projVerts_;
	for (int i = 0; i < verts; i++) {
		const float *p = projPos_ + i * 3, *t = projUvq_ + i * 3;
		float *c = clip[i];
		for (int r = 0; r < 4; r++)
			c[r] = ub.mvp[r] * p[0] + ub.mvp[4 + r] * p[1] + ub.mvp[8 + r] * p[2] + ub.mvp[12 + r];
		u8 bits = 0;
		if (clampS) {
			bits |= t[0] - sLo * t[2] < 0.0f ? 1 : 0;
			bits |= sHi * t[2] - t[0] < 0.0f ? 2 : 0;
		}
		if (clampT) {
			bits |= t[1] - tLo * t[2] < 0.0f ? 4 : 0;
			bits |= tHi * t[2] - t[1] < 0.0f ? 8 : 0;
		}
		bits |= c[3] - 1e-5f < 0.0f ? 16 : 0;
		bits |= t[2] <= 0.0f ? 32 : 0;
		outside[i] = bits;
		if ((bits & 16) == 0) {
			screen[i][0] = vp.x + (c[0] / c[3] + 1.0f) * 0.5f * vp.width;
			screen[i][1] = vp.y + (c[1] / c[3] + 1.0f) * 0.5f * vp.height;
		}
	}
	float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
	for (int i = 0; i + 2 < indexCount; i += 3) {
		const u16 ia = decIndex_[i], ib = decIndex_[i + 1], ic = decIndex_[i + 2];
		if (ia >= verts || ib >= verts || ic >= verts)
			return false;
		const u8 oa = outside[ia], ob = outside[ib], oc = outside[ic];
		if ((oa | ob | oc) & 32)
			return false;
		// inside every plane: its corners as they are; all outside one plane: no pixels
		if ((oa | ob | oc) == 0) {
			for (const u16 idx : { ia, ib, ic }) {
				minX = std::min(minX, screen[idx][0]);
				maxX = std::max(maxX, screen[idx][0]);
				minY = std::min(minY, screen[idx][1]);
				maxY = std::max(maxY, screen[idx][1]);
			}
			continue;
		}
		if (oa & ob & oc)
			continue;
		V poly[12], next[12];
		int n = 3;
		const u16 corner[3] = { ia, ib, ic };
		for (int k = 0; k < 3; k++) {
			const float *t = projUvq_ + corner[k] * 3;
			memcpy(poly[k].c, clip[corner[k]], sizeof(poly[k].c));
			poly[k].u = t[0];
			poly[k].v = t[1];
			poly[k].q = t[2];
		}
		// the planes, as linear functions of a corner: kept where >= 0
		for (int plane = 0; plane < 5 && n > 0; plane++) {
			if ((plane < 2 && !clampS) || (plane >= 2 && plane < 4 && !clampT))
				continue;
			auto f = [&](const V &a) {
				switch (plane) {
				case 0: return a.u - sLo * a.q;
				case 1: return sHi * a.q - a.u;
				case 2: return a.v - tLo * a.q;
				case 3: return tHi * a.q - a.v;
				default: return a.c[3] - 1e-5f;
				}
			};
			int m = 0;
			for (int k = 0; k < n; k++) {
				const V &a = poly[k], &b = poly[(k + 1) % n];
				const float fa = f(a), fb = f(b);
				if (fa >= 0.0f)
					next[m++] = a;
				if ((fa >= 0.0f) != (fb >= 0.0f)) {
					const float t = fa / (fa - fb);
					V x;
					for (int r = 0; r < 4; r++)
						x.c[r] = a.c[r] + (b.c[r] - a.c[r]) * t;
					x.u = a.u + (b.u - a.u) * t;
					x.v = a.v + (b.v - a.v) * t;
					x.q = a.q + (b.q - a.q) * t;
					next[m++] = x;
				}
				if (m >= 11)
					return false;
			}
			memcpy(poly, next, sizeof(V) * m);
			n = m;
		}
		for (int k = 0; k < n; k++) {
			const float x = vp.x + (poly[k].c[0] / poly[k].c[3] + 1.0f) * 0.5f * vp.width;
			const float y = vp.y + (poly[k].c[1] / poly[k].c[3] + 1.0f) * 0.5f * vp.height;
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}
	}
	const ScissorRect &cur = dynState_.scissor;
	int x1 = std::max((int)cur.x, (int)floorf(minX) - 2), y1 = std::max((int)cur.y, (int)floorf(minY) - 2);
	int x2 = std::min((int)cur.x + (int)cur.width, (int)ceilf(maxX) + 2), y2 = std::min((int)cur.y + (int)cur.height, (int)ceilf(maxY) + 2);
	if (x2 <= x1 || y2 <= y1)
		x1 = y1 = x2 = y2 = 0;
	box.x = x1;
	box.y = y1;
	box.width = x2 - x1;
	box.height = y2 - y1;
	return true;
}

void DrawEngineVulkan::Flush() {
	if (!numDrawVerts_) {
		return;
	}
	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);

	struct EffectCopyEnd {
		bool active = false;
		~EffectCopyEnd() {
			if (active)
				gstate_c.SetCurRTOffset(gstate_c.curRTOffsetX, 0);
		}
	} effectCopyEnd;
	bool effectDeferrable = false, effectCopy = false;
	gstate_c.framebufferTextureShiftY = 0;
	if (PSP_CoreParameter().compat.flags().DeferEffectBlend && gstate.isModeThrough() && gstate.isTextureMapEnabled() && !gstate.isModeClear() && gstate.FrameBufFormat() == GE_FORMAT_8888) {
		const u32 target = gstate.getFrameBufRawAddress() & 0x00FFFFFF;
		const u32 texture = gstate.getTextureAddress(0) & 0x00FFFFFF;
		const bool isVram = Memory::IsVRAMAddress(gstate.getTextureAddress(0));
		const bool blends = gstate.isAlphaBlendEnabled(), stencils = gstate.isStencilTestEnabled();
		// Deadstation Portable: Motion Blur off (God of War's own option, settings.cpp): once the effect is known (below), none of it is
		// drawn: the copies and the blur into its buffer, and its blends back over the scene. The shadows then draw every frame.
		if (!g_Config.bMotionBlur && effectBlend.knownBuffer != 0 && (u32)gpuStats.numFlips - effectBlend.knownFlip <= 2) {
			const bool intoBuffer = target >= effectBlend.knownBuffer && target < effectBlend.knownBuffer + 0x800;
			const bool fromBuffer = !intoBuffer && isVram && blends && texture >= effectBlend.knownBuffer && texture < effectBlend.knownBuffer + 0x800;
			if (intoBuffer || fromBuffer) {
				// still known while it is skipped
				if (fromBuffer)
					effectBlend.knownFlip = gpuStats.numFlips;
				gstate_c.effectSkipOffscreen = false;
				ResetAfterDraw();
				return;
			}
		}
		// At 60, the blur takes its turn every other frame: the result stays in the buffer, it's a soft glow at the game's own 30 a second, and it's
		// 2.5 fps in a fight. Decided at the copy, once a frame, so a frame never gets half of it.
		// All of this only while the game is doing this: anything else it draws to that buffer gets drawn.
		if (g_Config.iGodOfWarFrameRate == 60 && effectBlend.knownBuffer != 0 && (u32)gpuStats.numFlips - effectBlend.knownFlip <= 2 && target >= effectBlend.knownBuffer && target < effectBlend.knownBuffer + 0x800) {
			if (target == effectBlend.knownBuffer && !blends) {
				// Next frame's small off-screen pictures, the shadows, come before its copy: they take turns with the blur, so no frame has both.
				// Four passes on every other frame, and half a millisecond of the CPU's frame.
				gstate_c.effectSkipOffscreen = !effectBlend.skipBlur && gstate_c.effectSceneB != 0;
				gstate_c.effectBuffer = effectBlend.knownBuffer;
				gstate_c.effectFlip = (u32)gpuStats.numFlips;
				if (texture != gstate_c.effectSceneA) {
					gstate_c.effectSceneB = gstate_c.effectSceneA;
					gstate_c.effectSceneA = texture;
				}
				effectBlend.skipBlur = !effectBlend.skipBlur;
			} else if (effectBlend.skipBlur) {
				ResetAfterDraw();
				return;
			}
		}
		if (isVram && !blends && !stencils && target != texture) {
			effectBlend.buffer = target;
			effectBlend.scene = texture;
			effectBlend.shiftY = 0;
			effectCopy = target == effectBlend.knownBuffer && (u32)gpuStats.numFlips - effectBlend.knownFlip <= 2;
			// The copy too: nothing reads the scene then until it's finished, so its passes become one. It's a blur, a frame older at worst.
			if (effectCopy && effectBlend.skipBlur && effectBlend.copyShiftY != 0) {
				effectBlend.shiftY = effectBlend.copyShiftY;
				ResetAfterDraw();
				return;
			}
		} else if (isVram && blends && stencils && target == effectBlend.scene && texture == effectBlend.buffer) {
			effectBlend.knownBuffer = effectBlend.buffer;
			effectBlend.knownFlip = gpuStats.numFlips;
			if (effectBlend.shiftY != 0) {
				gstate_c.framebufferTextureShiftY = effectBlend.shiftY;
				effectDeferrable = true;
			}
		}
	}

	renderManager->AssertInRenderPass();

	if (!gstate.isModeThrough())
		framebufferManager_->NoteDrawReach(gstate.getScissorX2() + 1 + std::max(gstate_c.curRTOffsetX, 0), gstate.getScissorY2() + 1 + std::max(gstate_c.curRTOffsetY, 0));

	if ((gstate.isDepthTestEnabled() && gstate.getDepthTestFunction() != GE_COMP_ALWAYS) || (gstate.isStencilTestEnabled() && gstate.getStencilTestFunction() != GE_COMP_ALWAYS))
		renderManager->NoteDepthStencilRead();
	if (gstate.isModeClear() ? gstate.isClearModeDepthMask() : (gstate.isDepthTestEnabled() && (gstate.getDepthTestFunction() != GE_COMP_ALWAYS || gstate.isDepthWriteEnabled())))
		renderManager->NoteDepthUsed();


	bool tess = gstate_c.submitType == SubmitType::HW_BEZIER || gstate_c.submitType == SubmitType::HW_SPLINE;

	bool textureNeedsApply = false;
	if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
		textureCache_->SetTexture();
		gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
		// NOTE: After this is set, we MUST call ApplyTexture before returning.
		textureNeedsApply = true;
	} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
		// This catches the case of clearing a texture.
		gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
	}

	GEPrimitiveType prim = prevPrim_;

	// Always use software for flat shading to fix the provoking index
	// if the provoking vertex extension is not available.
	bool provokingVertexOk = (tess || gstate.getShadeMode() != GE_SHADE_FLAT);
	if (renderManager->GetVulkanContext()->GetDeviceFeatures().enabled.provokingVertex.provokingVertexLast) {
		provokingVertexOk = true;
	}
	bool useHWTransform = CanUseHardwareTransform(prim) && provokingVertexOk;

	// The optimization to avoid indexing isn't really worth it on Vulkan since it means creating more pipelines.
	// This could be avoided with the new dynamic state extensions, but not available enough on mobile.
	const bool forceIndexed = draw_->GetDeviceCaps().verySlowShaderCompiler;

	if (useHWTransform) {
		uint32_t vbOffset;

		VkBuffer vbuf = VK_NULL_HANDLE;
		// Projected texture, clamped: triangles whose every pixel reads only the texture's edge culled, the rest drawn
		// within the screen box of what reads more (cullProjected, projectedFootprint). Decoded where the CPU reads back fast.
		const bool projCull = projectedCullable();
		if (applySkinInDecode_ && (lastVType_ & GE_VTYPE_WEIGHT_MASK)) {
			// If software skinning, we're predecoding into "decoded". So make sure we're done, then push that content.
			DecodeVerts(dec_, decoded_);
			VkDeviceSize size = numDecodedVerts_ * dec_->GetDecVtxFmt().stride;
			u8 *dest = (u8 *)pushVertex_->Allocate(size, 4, &vbuf, &vbOffset);
			memcpy(dest, decoded_, size);
		} else {
			// Figure out how much pushbuffer space we need to allocate.
			int vertsToDecode = ComputeNumVertsToDecode();
			// Decode directly into the pushbuffer
			u8 *dest = pushVertex_->Allocate(vertsToDecode * dec_->GetDecVtxFmt().stride, 4, &vbuf, &vbOffset);
			if (projCull) {
				DecodeVerts(dec_, decoded_);
				memcpy(dest, decoded_, numDecodedVerts_ * dec_->GetDecVtxFmt().stride);
			} else {
				DecodeVerts(dec_, dest);
			}
		}

		int vertexCount;
		int maxIndex;
		bool useElements;
		DecodeIndsAndGetData(&prim, &vertexCount, &maxIndex, &useElements, false);
		if (projCull && useElements && prim == GE_PRIM_TRIANGLES)
			vertexCount = cullProjected(vertexCount);

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		if (textureNeedsApply) {
			textureCache_->ApplyTexture();
			textureCache_->GetVulkanHandles(imageView, sampler);
			if (imageView == VK_NULL_HANDLE)
				imageView = (VkImageView)draw_->GetNativeObject(gstate_c.textureIsArray ? Draw::NativeObject::NULL_IMAGEVIEW_ARRAY : Draw::NativeObject::NULL_IMAGEVIEW);
			if (sampler == VK_NULL_HANDLE)
				sampler = nullSampler_;
		}

		if (!lastPipeline_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE) || prim != lastPrim_) {
			if (prim != lastPrim_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
				ConvertStateToVulkanKey(*framebufferManager_, shaderManager_, prim, pipelineKey_, dynState_);
			}

			VulkanVertexShader *vshader = nullptr;
			VulkanFragmentShader *fshader = nullptr;
			VulkanGeometryShader *gshader = nullptr;

			shaderManager_->GetShaders(prim, dec_->VertexType(), &vshader, &fshader, &gshader, pipelineState_, true, useHWTessellation_, decOptions_.expandAllWeightsToFloat, applySkinInDecode_);
			_dbg_assert_msg_(vshader->UseHWTransform(), "Bad vshader");
			VulkanPipeline *pipeline = pipelineManager_->GetOrCreatePipeline(renderManager, pipelineLayout_, pipelineKey_, &dec_->decFmt, vshader, fshader, gshader, true, 0, framebufferManager_->GetMSAALevel(), false);
			if (!pipeline || !pipeline->pipeline) {
				// Already logged, let's bail out.
				ResetAfterDraw();
				return;
			}
			BindShaderBlendTex();  // This might cause copies so important to do before BindPipeline.

			if (!renderManager->BindPipeline(pipeline->pipeline, pipeline->pipelineFlags, pipelineLayout_)) {
				renderManager->ReportBadStateForDraw();
				ResetAfterDraw();
				return;
			}
			if (pipeline != lastPipeline_) {
				if (lastPipeline_ && !(lastPipeline_->UsesBlendConstant() && pipeline->UsesBlendConstant())) {
					gstate_c.Dirty(DIRTY_BLEND_STATE);
				}
				lastPipeline_ = pipeline;
			}
			ApplyDrawStateLate(renderManager, false, 0, pipeline->UsesBlendConstant());
			gstate_c.Clean(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
			gstate_c.Dirty(dirtyRequiresRecheck_);
			dirtyRequiresRecheck_ = 0;
			lastPipeline_ = pipeline;
		}
		lastPrim_ = prim;

		dirtyUniforms_ |= shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering());
		UpdateUBOs();

		int descCount = 6;
		if (tess)
			descCount = 9;
		int descSetIndex;
		PackedDescriptor *descriptors = renderManager->PushDescriptorSet(descCount, &descSetIndex);
		descriptors[0].image.view = imageView;
		descriptors[0].image.sampler = sampler;

		descriptors[1].image.view = boundSecondary_;
		descriptors[1].image.sampler = samplerSecondaryNearest_;

		descriptors[2].image.view = boundDepal_;
		descriptors[2].image.sampler = (boundDepal_ && boundDepalSmoothed_) ? samplerSecondaryLinear_ : samplerSecondaryNearest_;

		descriptors[3].buffer.buffer = baseBuf;
		descriptors[3].buffer.range = sizeof(UB_VS_FS_Base);
		descriptors[3].buffer.offset = 0;

		descriptors[4].buffer.buffer = lightBuf;
		descriptors[4].buffer.range = sizeof(UB_VS_Lights);
		descriptors[4].buffer.offset = 0;

		descriptors[5].buffer.buffer = boneBuf;
		descriptors[5].buffer.range = sizeof(UB_VS_Bones);
		descriptors[5].buffer.offset = 0;
		if (tess) {
			const VkDescriptorBufferInfo *bufInfo = tessDataTransferVulkan->GetBufferInfo();
			for (int j = 0; j < 3; j++) {
				descriptors[j + 6].buffer.buffer = bufInfo[j].buffer;
				descriptors[j + 6].buffer.range = bufInfo[j].range;
				descriptors[j + 6].buffer.offset = bufInfo[j].offset;
			}
		}
		// TODO: Can we avoid binding all three when not needed? Same below for hardware transform.
		// Think this will require different descriptor set layouts.
		const uint32_t dynamicUBOOffsets[3] = {
			baseUBOOffset, lightUBOOffset, boneUBOOffset,
		};
		bool footprint = false;
		if (projCull && useElements && prim == GE_PRIM_TRIANGLES && vertexCount > 0) {
			ScissorRect box;
			footprint = projectedFootprint(vertexCount, box);
			if (footprint && box.width == 0)
				vertexCount = 0;
			else if (footprint)
				renderManager->SetScissor(box.x, box.y, box.width, box.height);
		}
		if (vertexCount == 0) {
			// every triangle culled
		} else if (useElements) {
			VkBuffer ibuf;
			u32 ibOffset = (uint32_t)pushIndex_->Push(decIndex_, sizeof(uint16_t) * vertexCount, 4, &ibuf);
			renderManager->DrawIndexed(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, ibuf, ibOffset, vertexCount, 1);
		} else {
			renderManager->Draw(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, vertexCount);
		}
		if (footprint)
			renderManager->SetScissor(dynState_.scissor.x, dynState_.scissor.y, dynState_.scissor.width, dynState_.scissor.height);
		if (DrawKeepsDepthStencil())
			renderManager->NoteDrawKeptDepthStencil();
		if (useDepthRaster_) {
			DepthRasterSubmitRaw(prim, dec_, dec_->VertexType(), vertexCount);
		}
	} else {
		const VertexDecoder *swDec = dec_;
		if (swDec->nweights != 0) {
			u32 withSkinning = lastVType_ | (1 << 26);
			if (withSkinning != lastVType_) {
				swDec = GetVertexDecoder(withSkinning);
			}
		}
		int prevDecodedVerts = numDecodedVerts_;

		DecodeVerts(swDec, decoded_);
		int vertexCount = DecodeInds();

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		gpuStats.numUncachedVertsDrawn += vertexCount;
		prim = IndexGenerator::GeneralPrim((GEPrimitiveType)drawInds_[0].prim);

		// At this point, the output is always an index triangle/line/point list, no strips/fans.

		u16 *inds = decIndex_;
		SoftwareTransformResult result{};
		SoftwareTransformParams params{};
		params.decoded = decoded_;
		params.transformed = transformed_;
		params.transformedExpanded = transformedExpanded_;
		params.fbman = framebufferManager_;
		params.texCache = textureCache_;
		// In Vulkan, we have to force drawing of primitives if !framebufferManager_->UseBufferedRendering() because Vulkan clears
		// do not respect scissor rects.
		params.allowClear = framebufferManager_->UseBufferedRendering();
		params.allowSeparateAlphaClear = false;

		if (gstate.getShadeMode() == GE_SHADE_FLAT) {
			if (!renderManager->GetVulkanContext()->GetDeviceFeatures().enabled.provokingVertex.provokingVertexLast) {
				// If we can't have the hardware do it, we need to rotate the index buffer to simulate a different provoking vertex.
				// We do this before line expansion etc.
				IndexBufferProvokingLastToFirst(prim, inds, vertexCount);
			}
		}
		params.flippedY = true;
		params.usesHalfZ = true;

		// We need to update the viewport early because it's checked for flipping in SoftwareTransform.
		// We don't have a "DrawStateEarly" in vulkan, so...
		// TODO: Probably should eventually refactor this and feed the vp size into SoftwareTransform directly (Unknown's idea).
		if (gstate_c.IsDirty(DIRTY_VIEWPORTSCISSOR_STATE)) {
			ViewportAndScissor vpAndScissor;
			ConvertViewportAndScissor(
				framebufferManager_->UseBufferedRendering(),
				framebufferManager_->GetRenderWidth(), framebufferManager_->GetRenderHeight(),
				framebufferManager_->GetTargetBufferWidth(), framebufferManager_->GetTargetBufferHeight(),
				vpAndScissor);
			UpdateCachedViewportState(vpAndScissor);
		}

		// At this point, rect and line primitives are still preserved as such. So, it's the best time to do software depth raster.
		// We could piggyback on the viewport transform below, but it gets complicated since it's different per-backend. Which we really
		// should clean up one day...
		if (useDepthRaster_) {
			DepthRasterPredecoded(prim, decoded_, numDecodedVerts_, swDec, vertexCount);
		}

		SoftwareTransform swTransform(params);

		const Lin::Vec3 trans(gstate_c.vpXOffset, gstate_c.vpYOffset, gstate_c.vpZOffset * 0.5f + 0.5f);
		const Lin::Vec3 scale(gstate_c.vpWidthScale, gstate_c.vpHeightScale, gstate_c.vpDepthScale * 0.5f);
		swTransform.SetProjMatrix(gstate.projMatrix, gstate_c.vpWidth < 0, gstate_c.vpHeight < 0, trans, scale);

		swTransform.Transform(prim, swDec->VertexType(), swDec->GetDecVtxFmt(), numDecodedVerts_, &result);

		// Non-zero depth clears are unusual, but some drivers don't match drawn depth values to cleared values.
		// Games sometimes expect exact matches (see #12626, for example) for equal comparisons.
		if (result.action == SW_CLEAR && everUsedEqualDepth_ && gstate.isClearModeDepthMask() && result.depth > 0.0f && result.depth < 1.0f)
			result.action = SW_NOT_READY;

		if (result.action == SW_NOT_READY) {
			// decIndex_ here is always equal to inds currently, but it may not be in the future.
			swTransform.BuildDrawingParams(prim, vertexCount, swDec->VertexType(), inds, RemainingIndices(inds), numDecodedVerts_, VERTEX_BUFFER_MAX, &result);
		}

		if (result.setSafeSize)
			framebufferManager_->SetSafeSize(result.safeWidth, result.safeHeight);

		// Only here, where we know whether to clear or to draw primitives, should we actually set the current framebuffer! Because that gives use the opportunity
		// to use a "pre-clear" render pass, for high efficiency on tilers.
		if (result.action == SW_DRAW_INDEXED) {
			// A 2D draw's own bounds: what it can touch, here and for the render pass's area (VulkanRenderManager::SetDrawBounds).
			const bool boundsKnown = gstate.isModeThrough() && numDecodedVerts_ <= 4096 && gstate_c.curRTWidth > 0 && gstate_c.curRTHeight > 0;
			float boundsMinX = 1e30f, boundsMinY = 1e30f, boundsMaxX = -1e30f, boundsMaxY = -1e30f;
			if (boundsKnown) {
				for (int i = 0; i < numDecodedVerts_; i++) {
					const TransformedVertex &v = result.drawBuffer[i];
					boundsMinX = std::min(boundsMinX, v.x);
					boundsMaxX = std::max(boundsMaxX, v.x);
					boundsMinY = std::min(boundsMinY, v.y);
					boundsMaxY = std::max(boundsMaxY, v.y);
				}
			}
			// The copy fits in the upper half of the buffer: it goes in the lower.
			if (effectCopy && boundsKnown && boundsMinY >= 0.0f && ceilf(boundsMaxY) * 2 <= gstate_c.curRTHeight) {
				effectBlend.shiftY = (int)ceilf(boundsMaxY);
				effectBlend.copyShiftY = effectBlend.shiftY;
				gstate_c.SetCurRTOffset(gstate_c.curRTOffsetX, effectBlend.shiftY);
				effectCopyEnd.active = true;
			}
			if (boundsKnown) {
				framebufferManager_->NoteDrawReach(std::min((int)ceilf(boundsMaxX), gstate.getScissorX2() + 1) + std::max(gstate_c.curRTOffsetX, 0), std::min((int)ceilf(boundsMaxY), gstate.getScissorY2() + 1) + std::max(gstate_c.curRTOffsetY, 0));
			} else {
				framebufferManager_->NoteDrawReach(gstate.getScissorX2() + 1 + std::max(gstate_c.curRTOffsetX, 0), gstate.getScissorY2() + 1 + std::max(gstate_c.curRTOffsetY, 0));
			}
			if (textureNeedsApply) {
				gstate_c.pixelMapped = result.pixelMapped;
				gstate_c.dstSquared = false;
				if (boundsKnown)
					framebufferManager_->SetDrawRect((int)floorf(boundsMinX) + gstate_c.curRTOffsetX, (int)floorf(boundsMinY) + std::max(gstate_c.curRTOffsetY, 0), (int)ceilf(boundsMaxX) + gstate_c.curRTOffsetX, (int)ceilf(boundsMaxY) + std::max(gstate_c.curRTOffsetY, 0));
				textureCache_->ApplyTexture();
				framebufferManager_->ForgetDrawRect();
				gstate_c.pixelMapped = false;
				textureCache_->GetVulkanHandles(imageView, sampler);
				if (imageView == VK_NULL_HANDLE)
					imageView = (VkImageView)draw_->GetNativeObject(gstate_c.textureIsArray ? Draw::NativeObject::NULL_IMAGEVIEW_ARRAY : Draw::NativeObject::NULL_IMAGEVIEW);
				if (sampler == VK_NULL_HANDLE)
					sampler = nullSampler_;
				if (gstate_c.dstSquared) {
					gstate_c.Dirty(DIRTY_BLEND_STATE);
				}
			}
			// A framebuffer texture read through a clamp in the shader is a dependent read, for every pixel of the screen-sized draws of a
			// game's effects. A 2D draw's UVs are known: if they can't reach past the rectangle, the sampler's own clamp does as well.
			bool clampDropped = false;
			if (gstate.isModeThrough() && gstate_c.needShaderTexClamp && gstate_c.textureIsFramebuffer && gstate.isTexCoordClampedS() && gstate.isTexCoordClampedT() && gstate_c.shaderDepalMode == ShaderDepalMode::OFF && numDecodedVerts_ <= 4096) {
				const float imgW = (float)gstate_c.curTextureWidth, imgH = (float)gstate_c.curTextureHeight;
				const float xo = (float)gstate_c.curTextureXOffset, yo = (float)gstate_c.curTextureYOffset;
				const float tw = (float)gstate.getTextureWidth(0), th = (float)gstate.getTextureHeight(0);
				float minU = 1e30f, minV = 1e30f, maxU = -1e30f, maxV = -1e30f;
				for (int i = 0; i < numDecodedVerts_; i++) {
					const TransformedVertex &v = result.drawBuffer[i];
					minU = std::min(minU, v.u);
					maxU = std::max(maxU, v.u);
					minV = std::min(minV, v.v);
					maxV = std::max(maxV, v.v);
				}
				const bool leftOk = xo <= 0.0f || minU * imgW >= 0.5f;
				const bool rightOk = xo + tw >= imgW || maxU * imgW <= tw - 0.5f;
				const bool topOk = yo <= 0.0f || minV * imgH >= 0.5f;
				const bool bottomOk = yo + th >= imgH || maxV * imgH <= th - 0.5f;
				if (leftOk && rightOk && topOk && bottomOk && xo >= 0.0f && yo >= 0.0f) {
					const float du = xo / imgW, dv = yo / imgH;
					for (int i = 0; i < numDecodedVerts_; i++) {
						result.drawBuffer[i].u += du;
						result.drawBuffer[i].v += dv;
					}
					gstate_c.SetNeedShaderTexclamp(false);
					clampDropped = true;
				}
			}

			if (!lastPipeline_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE) || prim != lastPrim_) {
				if (prim != lastPrim_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
					ConvertStateToVulkanKey(*framebufferManager_, shaderManager_, prim, pipelineKey_, dynState_);
				}

				VulkanVertexShader *vshader = nullptr;
				VulkanFragmentShader *fshader = nullptr;
				VulkanGeometryShader *gshader = nullptr;

				shaderManager_->GetShaders(prim, swDec->VertexType(), &vshader, &fshader, &gshader, pipelineState_, false, false, decOptions_.expandAllWeightsToFloat, true);
				_dbg_assert_msg_(!vshader->UseHWTransform(), "Bad vshader");
				VulkanPipeline *pipeline = pipelineManager_->GetOrCreatePipeline(renderManager, pipelineLayout_, pipelineKey_, &swDec->decFmt, vshader, fshader, gshader, false, 0, framebufferManager_->GetMSAALevel(), false);
				if (!pipeline || !pipeline->pipeline) {
					// Already logged, let's bail out.
					ResetAfterDraw();
					return;
				}
				BindShaderBlendTex();  // This might cause copies so super important to do before BindPipeline.

				if (!renderManager->BindPipeline(pipeline->pipeline, pipeline->pipelineFlags, pipelineLayout_)) {
					renderManager->ReportBadStateForDraw();
					ResetAfterDraw();
					return;
				}
				if (pipeline != lastPipeline_) {
					if (lastPipeline_ && !lastPipeline_->UsesBlendConstant() && pipeline->UsesBlendConstant()) {
						gstate_c.Dirty(DIRTY_BLEND_STATE);
					}
					lastPipeline_ = pipeline;
				}
				ApplyDrawStateLate(renderManager, result.setStencil, result.stencilValue, pipeline->UsesBlendConstant());
				gstate_c.Clean(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
				gstate_c.Dirty(dirtyRequiresRecheck_);
				dirtyRequiresRecheck_ = 0;
				lastPipeline_ = pipeline;
			}

			lastPrim_ = prim;

			dirtyUniforms_ |= shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering());

			// Even if the first draw is through-mode, make sure we at least have one copy of these uniforms buffered
			UpdateUBOs();

			int descCount = 6;
			int descSetIndex;
			PackedDescriptor *descriptors = renderManager->PushDescriptorSet(descCount, &descSetIndex);
			descriptors[0].image.view = imageView;
			descriptors[0].image.sampler = sampler;
			descriptors[1].image.view = boundSecondary_;
			descriptors[1].image.sampler = samplerSecondaryNearest_;
			descriptors[2].image.view = boundDepal_;
			descriptors[2].image.sampler = (boundDepal_ && boundDepalSmoothed_) ? samplerSecondaryLinear_ : samplerSecondaryNearest_;
			descriptors[3].buffer.buffer = baseBuf;
			descriptors[3].buffer.range = sizeof(UB_VS_FS_Base);
			descriptors[3].buffer.offset = 0;
			descriptors[4].buffer.buffer = lightBuf;
			descriptors[4].buffer.range = sizeof(UB_VS_Lights);
			descriptors[4].buffer.offset = 0;
			descriptors[5].buffer.buffer = boneBuf;
			descriptors[5].buffer.range = sizeof(UB_VS_Bones);
			descriptors[5].buffer.offset = 0;

			const uint32_t dynamicUBOOffsets[3] = {
				baseUBOOffset, lightUBOOffset, boneUBOOffset,
			};


			if (boundsKnown) {
				const float minX = boundsMinX, minY = boundsMinY, maxX = boundsMaxX, maxY = boundsMaxY;
				const VkViewport &vp = dynState_.viewport;
				const float sx = vp.width / gstate_c.curRTWidth, sy = vp.height / gstate_c.curRTHeight;
				const int x1 = (int)floorf(vp.x + minX * sx), y1 = (int)floorf(vp.y + minY * sy), x2 = (int)ceilf(vp.x + maxX * sx), y2 = (int)ceilf(vp.y + maxY * sy);
				renderManager->SetDrawBounds(x1, y1, x2, y2);
				// A first draw that replaces all it covers, with triangles that fill the rectangle: nothing that was there is ever seen.
				if (prim == GE_PRIM_TRIANGLES && result.drawNumTrans >= 3 && !gstate.isAlphaTestEnabled() && !gstate.isColorTestEnabled() && !gstate.isStencilTestEnabled() && !gstate.isDepthTestEnabled() &&
					!gstate.isLogicOpEnabled() && gstate.getColorMask() == 0 &&
					(!gstate.isAlphaBlendEnabled() || (gstate.getBlendFuncA() == GE_SRCBLEND_FIXA && gstate.getBlendFuncB() == GE_DSTBLEND_FIXB && gstate.getFixB() == 0 && gstate.getBlendEq() == GE_BLENDMODE_MUL_AND_ADD))) {
					float area = 0.0f;
					for (int i = 0; i + 2 < result.drawNumTrans; i += 3) {
						const TransformedVertex &a = result.drawBuffer[inds[i]], &b = result.drawBuffer[inds[i + 1]], &c = result.drawBuffer[inds[i + 2]];
						area += fabsf((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5f;
					}
					const float boxArea = (maxX - minX) * (maxY - minY);
					if (boxArea > 0.0f && fabsf(area - boxArea) <= boxArea * 0.001f && minX == floorf(minX) && minY == floorf(minY) && maxX == floorf(maxX) && maxY == floorf(maxY))
						renderManager->NoteOverwritingDraw(x1, y1, x2, y2);
				}
			}

			VkBuffer vbuf, ibuf;
			u32 vbOffset = (uint32_t)pushVertex_->Push(result.drawBuffer, numDecodedVerts_ * sizeof(TransformedVertex), 4, &vbuf);
			u32 ibOffset = (uint32_t)pushIndex_->Push(inds, sizeof(short) * result.drawNumTrans, 4, &ibuf);
			renderManager->DrawIndexed(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, ibuf, ibOffset, result.drawNumTrans, 1);
			if (DrawKeepsDepthStencil())
				renderManager->NoteDrawKeptDepthStencil();
			if (effectDeferrable)
				renderManager->NoteDrawDeferrable();
			if (clampDropped) {
				gstate_c.SetNeedShaderTexclamp(true);
			}
		} else if (result.action == SW_CLEAR) {
			framebufferManager_->NoteDrawReach(gstate_c.curRTWidth, gstate_c.curRTHeight);
			// Note: we won't get here if the clear is alpha but not color, or color but not alpha.
			bool clearColor = gstate.isClearModeColorMask();
			bool clearAlpha = gstate.isClearModeAlphaMask();  // and stencil
			bool clearDepth = gstate.isClearModeDepthMask();
			Draw::Aspect mask = Draw::Aspect::NO_BIT;
			// The Clear detection takes care of doing a regular draw instead if separate masking
			// of color and alpha is needed, so we can just treat them as the same.
			if (clearColor || clearAlpha) mask |= Draw::Aspect::COLOR_BIT;
			if (clearDepth) mask |= Draw::Aspect::DEPTH_BIT;
			if (clearAlpha) mask |= Draw::Aspect::STENCIL_BIT;
			// Note that since the alpha channel and the stencil channel are shared on the PSP,
			// when we clear alpha, we also clear stencil to the same value.
			draw_->Clear(mask, result.color, result.depth, result.color >> 24);
			if (gstate_c.Use(GPU_USE_CLEAR_RAM_HACK) && gstate.isClearModeColorMask() && (gstate.isClearModeAlphaMask() || gstate.FrameBufFormat() == GE_FORMAT_565)) {
				int scissorX1 = gstate.getScissorX1();
				int scissorY1 = gstate.getScissorY1();
				int scissorX2 = gstate.getScissorX2() + 1;
				int scissorY2 = gstate.getScissorY2() + 1;
				framebufferManager_->ApplyClearToMemory(scissorX1, scissorY1, scissorX2, scissorY2, result.color);
			}
		}
	}

	ResetAfterDrawInline();

	framebufferManager_->SetColorUpdated(gstate_c.skipDrawReason);

}

void DrawEngineVulkan::ResetAfterDraw() {
	indexGen.Reset();
	numDecodedVerts_ = 0;
	numDrawVerts_ = 0;
	numDrawInds_ = 0;
	vertexCountInDrawCalls_ = 0;
	decodeIndsCounter_ = 0;
	decodeVertsCounter_ = 0;
	gstate_c.vertexFullAlpha = true;
}

void DrawEngineVulkan::UpdateUBOs() {
	if ((dirtyUniforms_ & DIRTY_BASE_UNIFORMS) || baseBuf == VK_NULL_HANDLE) {
		baseUBOOffset = shaderManager_->PushBaseBuffer(pushUBO_, &baseBuf);
		dirtyUniforms_ &= ~DIRTY_BASE_UNIFORMS;
	}
	if ((dirtyUniforms_ & DIRTY_LIGHT_UNIFORMS) || lightBuf == VK_NULL_HANDLE) {
		lightUBOOffset = shaderManager_->PushLightBuffer(pushUBO_, &lightBuf);
		dirtyUniforms_ &= ~DIRTY_LIGHT_UNIFORMS;
	}
	if ((dirtyUniforms_ & DIRTY_BONE_UNIFORMS) || boneBuf == VK_NULL_HANDLE) {
		boneUBOOffset = shaderManager_->PushBoneBuffer(pushUBO_, &boneBuf);
		dirtyUniforms_ &= ~DIRTY_BONE_UNIFORMS;
	}
}

void TessellationDataTransferVulkan::SendDataToShader(const SimpleVertex *const *points, int size_u, int size_v, u32 vertType, const Spline::Weight2D &weights) {
	// SSBOs that are not simply float1 or float2 need to be padded up to a float4 size. vec3 members
	// also need to be 16-byte aligned, hence the padding.
	struct TessData {
		float pos[3]; float pad1;
		float uv[2]; float pad2[2];
		float color[4];
	};

	int size = size_u * size_v;

	int ssboAlignment = vulkan_->GetPhysicalDeviceProperties().properties.limits.minStorageBufferOffsetAlignment;
	uint8_t *data = (uint8_t *)push_->Allocate(size * sizeof(TessData), ssboAlignment, &bufInfo_[0].buffer, (uint32_t *)&bufInfo_[0].offset);
	bufInfo_[0].range = size * sizeof(TessData);

	float *pos = (float *)(data);
	float *tex = (float *)(data + offsetof(TessData, uv));
	float *col = (float *)(data + offsetof(TessData, color));
	int stride = sizeof(TessData) / sizeof(float);

	CopyControlPoints(pos, tex, col, stride, stride, stride, points, size, vertType);

	using Spline::Weight;

	// Weights U
	data = (uint8_t *)push_->Allocate(weights.size_u * sizeof(Weight), ssboAlignment, &bufInfo_[1].buffer, (uint32_t *)&bufInfo_[1].offset);
	memcpy(data, weights.u, weights.size_u * sizeof(Weight));
	bufInfo_[1].range = weights.size_u * sizeof(Weight);

	// Weights V
	data = (uint8_t *)push_->Allocate(weights.size_v * sizeof(Weight), ssboAlignment, &bufInfo_[2].buffer, (uint32_t *)&bufInfo_[2].offset);
	memcpy(data, weights.v, weights.size_v * sizeof(Weight));
	bufInfo_[2].range = weights.size_v * sizeof(Weight);
}
