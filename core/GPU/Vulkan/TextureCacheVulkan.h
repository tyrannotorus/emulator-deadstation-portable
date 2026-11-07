// Copyright (c) 2015- PPSSPP Project.

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

#include "Common/Data/Collections/Hashmaps.h"
#include "GPU/GPUState.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/TextureShaderCommon.h"
#include "GPU/Vulkan/VulkanUtil.h"

struct VirtualFramebuffer;
struct TextureShaderInfo;
class FramebufferManagerVulkan;
class DepalShaderCacheVulkan;
class ShaderManagerVulkan;
class DrawEngineVulkan;

class VulkanContext;
class VulkanTexture;

class SamplerCache {
public:
	SamplerCache(VulkanContext *vulkan) : vulkan_(vulkan), cache_(16) {}
	~SamplerCache();
	VkSampler GetOrCreateSampler(const SamplerCacheKey &key);

	void DeviceLost();
	void DeviceRestore(VulkanContext *vulkan);


private:
	VulkanContext *vulkan_;
	DenseHashMap<SamplerCacheKey, VkSampler> cache_;
};

class TextureCacheVulkan : public TextureCacheCommon {
public:
	TextureCacheVulkan(Draw::DrawContext *draw, Draw2D *draw2D, VulkanContext *vulkan);
	~TextureCacheVulkan();

	void StartFrame() override;

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

	void SetFramebufferManager(FramebufferManagerVulkan *fbManager);
	void SetDrawEngine(DrawEngineVulkan *td) {
		drawEngine_ = td;
	}

	void ForgetLastTexture() override {}

	void GetVulkanHandles(VkImageView &imageView, VkSampler &sampler) {
		imageView = imageView_;
		sampler = curSampler_;
	}




	void *GetNativeTextureView(const TexCacheEntry *entry, bool flat) const override;

protected:
	void BindTexture(TexCacheEntry *entry) override;
	void Unbind() override;
	void ReleaseTexture(TexCacheEntry *entry, bool delete_them) override;
	void BindAsClutTexture(Draw::Texture *tex, bool smooth) override;
	void ApplySamplingParams(const SamplerCacheKey &key) override;
	void BoundFramebufferTexture() override;

private:

	void LoadVulkanTextureLevel(TexCacheEntry &entry, uint8_t *writePtr, int rowPitch, int level, VkFormat dstFmt);
	static VkFormat GetDestFormat(GETextureFormat format, GEPaletteFormat clutFormat) ;
	void UpdateCurrentClut(GEPaletteFormat clutFormat, u32 clutBase, bool clutIndexIsSimple) override;

	void BuildTexture(TexCacheEntry *const entry) override;




	SamplerCache samplerCache_;

	DrawEngineVulkan *drawEngine_;


	// Bound state to emulate an API similar to the others
	VkImageView imageView_ = VK_NULL_HANDLE;
	VkSampler curSampler_ = VK_NULL_HANDLE;

	VkSampler samplerNearest_ = VK_NULL_HANDLE;

	Path cbufferPath_;
	bool cbufferInited_ = true;
	bool cbufferFailed_ = false;
};

VkFormat getClutDestFormatVulkan(GEPaletteFormat format);
