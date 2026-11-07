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

#include <cstdint>

#include "Common/GPU/thin3d.h"
#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Core/Config.h"
#include "Core/CoreParameter.h"
#include "Core/System.h"
#include "GPU/Common/PresentationCommon.h"

struct Vertex {
	float x, y, z;
	float u, v;
	uint32_t rgba;
};

FRect PictureRect(float pixelWidth, float pixelHeight) {
	switch (g_Config.displayLayout.picture) {
	case PictureShape::Fill:
		return FRect{ 0.0f, 0.0f, pixelWidth, pixelHeight };
	case PictureShape::Crop: {
		// The PSP's shape from the top of the screen to the bottom: what's past the sides is clipped off.
		const float w = pixelHeight * 480.0f / 272.0f;
		return FRect{ (pixelWidth - w) * 0.5f, 0.0f, w, pixelHeight };
	}
	case PictureShape::Original:
		break;
	}
	const float h = pixelWidth * 272.0f / 480.0f;
	return FRect{ 0.0f, (pixelHeight - h) * 0.5f, pixelWidth, h };
}

PresentationCommon::PresentationCommon(Draw::DrawContext *draw) : draw_(draw) {
	CreateDeviceObjects();
}

PresentationCommon::~PresentationCommon() {
	DestroyDeviceObjects();
}

void PresentationCommon::DeviceLost() {
	DestroyDeviceObjects();
	draw_ = nullptr;
}

void PresentationCommon::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	CreateDeviceObjects();
}

Draw::Pipeline *PresentationCommon::CreatePipeline(std::vector<Draw::ShaderModule *> shaders) const {
	using namespace Draw;

	InputLayoutDesc inputDesc = {
		sizeof(Vertex),
		{
			{ SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 },
			{ SEM_TEXCOORD0, DataFormat::R32G32_FLOAT, 12 },
			{ SEM_COLOR0, DataFormat::R8G8B8A8_UNORM, 20 },
		},
	};

	InputLayout *inputLayout = draw_->CreateInputLayout(inputDesc);
	DepthStencilState *depth = draw_->CreateDepthStencilState({ false, false, Comparison::LESS });
	// colour only: the page keeps its opaque clear, as the display blends by alpha and the game's alpha is its stencil
	BlendState *blendstateOff = draw_->CreateBlendState({ false, 0x7 });
	RasterState *rasterNoCull = draw_->CreateRasterState({});

	PipelineDesc pipelineDesc{ Primitive::TRIANGLE_STRIP, shaders, inputLayout, depth, blendstateOff, rasterNoCull, &vsTexColBufDesc };
	Pipeline *pipeline = draw_->CreateGraphicsPipeline(pipelineDesc, "presentation");

	inputLayout->Release();
	depth->Release();
	blendstateOff->Release();
	rasterNoCull->Release();

	return pipeline;
}

void PresentationCommon::CreateDeviceObjects() {
	using namespace Draw;

	vdata_ = draw_->CreateBuffer(sizeof(Vertex) * 4, BufferUsageFlag::DYNAMIC | BufferUsageFlag::VERTEXDATA);

	samplerNearest_ = draw_->CreateSamplerState({ TextureFilter::NEAREST, TextureFilter::NEAREST, TextureFilter::NEAREST, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });
	samplerLinear_ = draw_->CreateSamplerState({ TextureFilter::LINEAR, TextureFilter::LINEAR, TextureFilter::LINEAR, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });

	texColor_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D) });
	texColorRBSwizzle_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D_RB_SWIZZLE) });
}

template <typename T>
static void DoRelease(T *&obj) {
	if (obj)
		obj->Release();
	obj = nullptr;
}

void PresentationCommon::DestroyDeviceObjects() {
	DoRelease(texColor_);
	DoRelease(texColorRBSwizzle_);
	DoRelease(samplerNearest_);
	DoRelease(samplerLinear_);
	DoRelease(vdata_);
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);
}

void PresentationCommon::SourceBlank() {
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);
}

// If texture == null, that means there's nothing to display, so we should show a black screen in CopyToOutput
void PresentationCommon::SourceTexture(Draw::Texture *texture) {
	// AddRef before release and assign in case it's the same.
	texture->AddRef();

	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcTexture_ = texture;
}

void PresentationCommon::SourceFramebuffer(Draw::Framebuffer *fb) {
	fb->AddRef();

	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcFramebuffer_ = fb;
}

void PresentationCommon::PrepareOutput(OutputFlags flags, float u0, float v0, float u1, float v1) {
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	outputFlags_ = flags;

	if (!srcTexture_ && !srcFramebuffer_) {
		return;
	}

	const FRect rc = PictureRect((float)pixelWidth_, (float)pixelHeight_);
	const float x = rc.x, y = rc.y, w = rc.w, h = rc.h;

	const float invW = 2.0f / (float)pixelWidth_;
	const float invH = 2.0f / (float)pixelHeight_;
	Vertex verts[4] = {
		{ x * invW - 1.0f, y * invH - 1.0f, 0.0f, u0, v0, 0xFFFFFFFF },
		{ (x + w) * invW - 1.0f, y * invH - 1.0f, 0.0f, u1, v0, 0xFFFFFFFF },
		{ x * invW - 1.0f, (y + h) * invH - 1.0f, 0.0f, u0, v1, 0xFFFFFFFF },
		{ (x + w) * invW - 1.0f, (y + h) * invH - 1.0f, 0.0f, u1, v1, 0xFFFFFFFF },
	};
	draw_->UpdateBuffer(vdata_, (const uint8_t *)verts, 0, sizeof(verts), Draw::UPDATE_DISCARD);
}

void PresentationCommon::CopyToOutput() {
	const bool useNearest = outputFlags_ & OutputFlags::NEAREST;

	draw_->SetScissorRect(0, 0, pixelWidth_, pixelHeight_);

	if (!srcFramebuffer_ && !srcTexture_) {
		// Bound blank. We're done.
		presentedThisFrame_ = true;
		return;
	}

	draw_->BindPipeline((outputFlags_ & OutputFlags::RB_SWIZZLE) ? texColorRBSwizzle_ : texColor_);
	if (srcTexture_) {
		draw_->BindTexture(0, srcTexture_);
	} else {
		draw_->BindFramebufferAsTexture(srcFramebuffer_, 0, Draw::Aspect::COLOR_BIT, 0);
	}

	Draw::VsTexColUB ub{};
	memcpy(ub.WorldViewProj, g_display.rot_matrix.m, sizeof(float) * 16);
	draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	draw_->BindVertexBuffer(vdata_, 0);

	Draw::SamplerState *sampler = useNearest ? samplerNearest_ : samplerLinear_;
	draw_->BindSamplerStates(0, 1, &sampler);

	Draw::Viewport viewport{ 0.0f, 0.0f, (float)pixelWidth_, (float)pixelHeight_, 0.0f, 1.0f };
	draw_->SetViewport(viewport);
	draw_->Draw(4, 0);

	DoRelease(srcFramebuffer_);
	DoRelease(srcTexture_);

	// Unbinds all textures and samplers too, needed since sometimes a MakePixelTexture is deleted etc.
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	presentedThisFrame_ = true;
}

void PresentationCommon::CalculateRenderResolution(int *width, int *height, int *scaleFactor) const {
	int zoom = g_Config.iInternalResolution;
	if (zoom <= 1)
		zoom = 1;
	*width = 480 * zoom;
	*height = 272 * zoom;
	*scaleFactor = zoom;
}
