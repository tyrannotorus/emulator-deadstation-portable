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

// The game's frame onto the page of the screen.
#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Common.h"

namespace Draw {
class Buffer;
class DrawContext;
class Framebuffer;
class Pipeline;
class SamplerState;
class ShaderModule;
class Texture;
}

struct FRect {
	float x;
	float y;
	float w;
	float h;
};

// Where the picture goes on a page of that size: its full width, centered, in the PSP's own shape or the taller one
FRect PictureRect(float pixelWidth, float pixelHeight);

enum class OutputFlags {
	DEFAULT = 0x0000,
	NEAREST = 0x0001,
	RB_SWIZZLE = 0x0002,
};
ENUM_CLASS_BITOPS(OutputFlags);

class PresentationCommon {
public:
	PresentationCommon(Draw::DrawContext *draw);
	~PresentationCommon();

	void UpdateDisplaySize(int w, int h) {
		pixelWidth_ = w;
		pixelHeight_ = h;
	}

	void BeginFrame() {
		presentedThisFrame_ = false;
	}
	bool PresentedThisFrame() const {
		return presentedThisFrame_;
	}
	void NotifyPresent() {
		presentedThisFrame_ = true;
	}

	void DeviceLost();
	void DeviceRestore(Draw::DrawContext *draw);

	// One of these must be called every frame.
	void SourceBlank();
	void SourceTexture(Draw::Texture *texture);
	void SourceFramebuffer(Draw::Framebuffer *fb);

	void PrepareOutput(OutputFlags flags, float u0, float v0, float u1, float v1);
	void CopyToOutput();

	void CalculateRenderResolution(int *width, int *height, int *scaleFactor) const;

protected:
	void CreateDeviceObjects();
	void DestroyDeviceObjects();

	Draw::Pipeline *CreatePipeline(std::vector<Draw::ShaderModule *> shaders) const;

	Draw::DrawContext *draw_;
	Draw::Pipeline *texColor_ = nullptr;
	Draw::Pipeline *texColorRBSwizzle_ = nullptr;
	Draw::SamplerState *samplerNearest_ = nullptr;
	Draw::SamplerState *samplerLinear_ = nullptr;
	Draw::Buffer *vdata_ = nullptr;

	Draw::Texture *srcTexture_ = nullptr;
	Draw::Framebuffer *srcFramebuffer_ = nullptr;

	int pixelWidth_ = 0;
	int pixelHeight_ = 0;

	bool presentedThisFrame_ = false;
	OutputFlags outputFlags_ = OutputFlags::DEFAULT;
};
