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
#include <cstring>
#include <memory>

#include "ext/xxhash.h"

#include "Common/File/VFS/VFS.h"
#include "Common/Data/Text/I18n.h"
#include "Common/LogReporting.h"
#include "Common/Math/math_util.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"
#include "Common/System/OSD.h"
#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"
#include "Common/GPU/Vulkan/VulkanBarrier.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanImage.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"

#include "Core/Config.h"

#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/GPUDefinitions.h"
#include "GPU/Common/TextureShaderCommon.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Vulkan/VulkanContext.h"
#include "GPU/Vulkan/TextureCacheVulkan.h"
#include "GPU/Vulkan/FramebufferManagerVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"
#include "GPU/Vulkan/DrawEngineVulkan.h"

using namespace PPSSPP_VK;

#define TEXCACHE_MIN_SLAB_SIZE (8 * 1024 * 1024)
#define TEXCACHE_MAX_SLAB_SIZE (32 * 1024 * 1024)
#define TEXCACHE_SLAB_PRESSURE 4

static int VkFormatBytesPerPixel(VkFormat format) {
	switch (format) {
	case VULKAN_8888_FORMAT: return 4;
	case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
	case VULKAN_CLUT8_FORMAT: return 1;
	default: break;
	}
	return 2;
}

SamplerCache::~SamplerCache() {
	DeviceLost();
}

VkSampler SamplerCache::GetOrCreateSampler(const SamplerCacheKey &key) {
	VkSampler sampler;
	if (cache_.Get(key, &sampler)) {
		return sampler;
	}

	VkSamplerCreateInfo samp = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = key.sClamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeV = key.tClamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	// W addressing is irrelevant for 2d textures, but Mali recommends that all clamp modes are the same if possible so just copy from U.
	samp.addressModeW = key.texture3d ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : samp.addressModeU;
	samp.compareOp = VK_COMPARE_OP_ALWAYS;
	samp.flags = 0;
	samp.magFilter = key.magFilt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	samp.minFilter = key.minFilt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	samp.mipmapMode = key.mipFilt ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

	if (key.aniso) {
		// Docs say the min of this value and the supported max are used.
		samp.maxAnisotropy = 1 << g_Config.iAnisotropyLevel;
		samp.anisotropyEnable = true;
	} else {
		samp.maxAnisotropy = 1.0f;
		samp.anisotropyEnable = false;
	}
	if (key.maxLevel == 9 * 256) {
		// No max level needed. Better for performance on some archs like ARM Mali.
		samp.maxLod = VK_LOD_CLAMP_NONE;
	} else {
		samp.maxLod = (float)(int32_t)key.maxLevel * (1.0f / 256.0f);
	}
	samp.minLod = (float)(int32_t)key.minLevel * (1.0f / 256.0f);
	samp.mipLodBias = (float)(int32_t)key.lodBias * (1.0f / 256.0f);

	VkResult res = vkCreateSampler(vulkan_->GetDevice(), &samp, nullptr, &sampler);
	_assert_(res == VK_SUCCESS);
	cache_.Insert(key, sampler);
	return sampler;
}


void SamplerCache::DeviceLost() {
	cache_.Iterate([&](const SamplerCacheKey &key, VkSampler sampler) {
		vulkan_->Delete().QueueDeleteSampler(sampler);
	});
	cache_.Clear();
	vulkan_ = nullptr;
}

void SamplerCache::DeviceRestore(VulkanContext *vulkan) {
	vulkan_ = vulkan;
}


TextureCacheVulkan::TextureCacheVulkan(Draw::DrawContext *draw, Draw2D *draw2D, VulkanContext *vulkan)
	: TextureCacheCommon(draw, draw2D),
		samplerCache_(vulkan) {
	DeviceRestore(draw);
}

TextureCacheVulkan::~TextureCacheVulkan() {
	DeviceLost();
}

void TextureCacheVulkan::SetFramebufferManager(FramebufferManagerVulkan *fbManager) {
	framebufferManager_ = fbManager;
}

void TextureCacheVulkan::DeviceLost() {
	textureShaderCache_->DeviceLost();
	
	VulkanContext *vulkan = draw_ ? (VulkanContext *)draw_->GetNativeObject(Draw::NativeObject::CONTEXT) : nullptr;

	Clear(true);

	samplerCache_.DeviceLost();
	if (samplerNearest_)
		vulkan->Delete().QueueDeleteSampler(samplerNearest_);



	nextTexture_ = nullptr;
	draw_ = nullptr;
	Unbind();
}

void TextureCacheVulkan::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;

	VulkanContext *vulkan = (VulkanContext *)draw->GetNativeObject(Draw::NativeObject::CONTEXT);
	_assert_(vulkan);

	samplerCache_.DeviceRestore(vulkan);
	textureShaderCache_->DeviceRestore(draw);

	VkSamplerCreateInfo samp{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samp.magFilter = VK_FILTER_NEAREST;
	samp.minFilter = VK_FILTER_NEAREST;
	samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	VkResult res = vkCreateSampler(vulkan->GetDevice(), &samp, nullptr, &samplerNearest_);
	_assert_(res == VK_SUCCESS);


}


static std::string ReadShaderSrc(const Path &filename) {
	size_t sz = 0;
	char *data = (char *)g_VFS.ReadFile(filename.c_str(), &sz);
	if (!data)
		return std::string();

	std::string src(data, sz);
	delete[] data;
	return src;
}









void TextureCacheVulkan::ReleaseTexture(TexCacheEntry *entry, bool delete_them) {
	delete entry->vkTex;
	entry->vkTex = nullptr;
}

VkFormat getClutDestFormatVulkan(GEPaletteFormat format) {
	switch (format) {
	case GE_CMODE_16BIT_ABGR4444:
		return VULKAN_4444_FORMAT;
	case GE_CMODE_16BIT_ABGR5551:
		return VULKAN_1555_FORMAT;
	case GE_CMODE_16BIT_BGR5650:
		return VULKAN_565_FORMAT;
	case GE_CMODE_32BIT_ABGR8888:
		return VULKAN_8888_FORMAT;
	}
	return VK_FORMAT_UNDEFINED;
}

static const VkFilter MagFiltVK[2] = {
	VK_FILTER_NEAREST,
	VK_FILTER_LINEAR
};

void TextureCacheVulkan::StartFrame() {
	TextureCacheCommon::StartFrame();
	// TODO: For low memory detection, maybe use some indication from VMA.
	// Maybe see https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/staying_within_budget.html#staying_within_budget_querying_for_budget .
}

void TextureCacheVulkan::UpdateCurrentClut(GEPaletteFormat clutFormat, u32 clutBase, bool clutIndexIsSimple) {
	const u32 clutBaseBytes = clutFormat == GE_CMODE_32BIT_ABGR8888 ? (clutBase * sizeof(u32)) : (clutBase * sizeof(u16));
	// Technically, these extra bytes weren't loaded, but hopefully it was loaded earlier.
	// If not, we're going to hash random data, which hopefully doesn't cause a performance issue.
	//
	// TODO: Actually, this seems like a hack.  The game can upload part of a CLUT and reference other data.
	// clutTotalBytes_ is the last amount uploaded.  We should hash clutMaxBytes_, but this will often hash
	// unrelated old entries for small palettes.
	// Adding clutBaseBytes may just be mitigating this for some usage patterns.
	const u32 clutExtendedBytes = std::min(clutTotalBytes_ + clutBaseBytes, clutMaxBytes_);

	clutHash_ = XXH3_64bits((const char *)clutBufRaw_, clutExtendedBytes) & 0xFFFFFFFF;
	clutBuf_ = clutBufRaw_;

	// Special optimization: fonts typically draw clut4 with just alpha values in a single color.
	clutAlphaLinear_ = false;
	clutAlphaLinearColor_ = 0;
	if (clutFormat == GE_CMODE_16BIT_ABGR4444 && clutIndexIsSimple) {
		const u16_le *clut = GetCurrentClut<u16_le>();
		clutAlphaLinear_ = true;
		clutAlphaLinearColor_ = clut[15] & 0x0FFF;
		for (int i = 0; i < 16; ++i) {
			u16 step = clutAlphaLinearColor_ | (i << 12);
			if (clut[i] != step) {
				clutAlphaLinear_ = false;
				break;
			}
		}
	}

	clutLastFormat_ = gstate.clutformat;
}

void TextureCacheVulkan::BindTexture(TexCacheEntry *entry) {
	if (!entry || !entry->vkTex) {
		Unbind();
		return;
	}

	int maxLevel = (entry->status & TexCacheEntry::STATUS_NO_MIPS) ? 0 : entry->maxLevel;
	SamplerCacheKey samplerKey = GetSamplingParams(maxLevel, entry);
	curSampler_ = samplerCache_.GetOrCreateSampler(samplerKey);
	imageView_ = entry->vkTex->GetImageView();
	drawEngine_->SetDepalTexture(VK_NULL_HANDLE, false);
	gstate_c.SetUseShaderDepal(ShaderDepalMode::OFF);
}

void TextureCacheVulkan::ApplySamplingParams(const SamplerCacheKey &key) {
	curSampler_ = samplerCache_.GetOrCreateSampler(key);
}

void TextureCacheVulkan::Unbind() {
	imageView_ = VK_NULL_HANDLE;
	curSampler_ = VK_NULL_HANDLE;
}

void TextureCacheVulkan::BindAsClutTexture(Draw::Texture *tex, bool smooth) {
	VkImageView clutTexture = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::TEXTURE_VIEW, tex);
	drawEngine_->SetDepalTexture(clutTexture, smooth);
}

static Draw::DataFormat FromVulkanFormat(VkFormat fmt) {
	switch (fmt) {
	case VULKAN_8888_FORMAT: default: return Draw::DataFormat::R8G8B8A8_UNORM;
	}
}

static VkFormat ToVulkanFormat(Draw::DataFormat fmt) {
	switch (fmt) {
	case Draw::DataFormat::BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case Draw::DataFormat::BC2_UNORM_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
	case Draw::DataFormat::BC3_UNORM_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
	case Draw::DataFormat::BC4_UNORM_BLOCK: return VK_FORMAT_BC4_UNORM_BLOCK;
	case Draw::DataFormat::BC5_UNORM_BLOCK: return VK_FORMAT_BC5_UNORM_BLOCK;
	case Draw::DataFormat::BC7_UNORM_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
	case Draw::DataFormat::ASTC_4x4_UNORM_BLOCK: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
	case Draw::DataFormat::ETC2_R8G8B8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
	case Draw::DataFormat::ETC2_R8G8B8A1_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
	case Draw::DataFormat::ETC2_R8G8B8A8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;

	case Draw::DataFormat::R8G8B8A8_UNORM: return VULKAN_8888_FORMAT;
	default: _assert_msg_(false, "Bad texture pixel format"); return VULKAN_8888_FORMAT;
	}
}

void TextureCacheVulkan::BuildTexture(TexCacheEntry *const entry) {
	VulkanContext *vulkan = (VulkanContext *)draw_->GetNativeObject(Draw::NativeObject::CONTEXT);

	BuildTexturePlan plan;
	if (!PrepareBuildTexture(plan, entry)) {
		// We're screwed (invalid size or something, corrupt display list), let's just zap it.
		if (entry->vkTex) {
			delete entry->vkTex;
			entry->vkTex = nullptr;
		}
		return;
	}

	VkFormat dstFmt = GetDestFormat(GETextureFormat(entry->format), gstate.getClutPaletteFormat());

	if (plan.decodeToClut8) {
		dstFmt = VULKAN_CLUT8_FORMAT;
	}

	_dbg_assert_(plan.levelsToLoad <= plan.maxPossibleLevels);

	// We don't generate mipmaps for 512x512 textures because they're almost exclusively used for menu backgrounds
	// and similar, which don't really need it.
	// Also, if using replacements, check that we really can generate mips for this format - that's not possible for compressed ones.
	if (g_Config.iTexFiltering == TEX_FILTER_AUTO_MAX_QUALITY && plan.w <= 256 && plan.h <= 256) {
		// Boost the number of mipmaps.
		if (plan.maxPossibleLevels > plan.levelsToCreate) { // TODO: Should check against levelsToLoad, no?
			// We have to generate mips with a shader. This requires decoding to R8G8B8A8_UNORM format to avoid extra complications.
			dstFmt = VULKAN_8888_FORMAT;
		}
		plan.levelsToCreate = plan.maxPossibleLevels;
	}

	_dbg_assert_(plan.levelsToCreate >= plan.levelsToLoad);

	VkFormat actualFmt = dstFmt;
	VkCommandBuffer cmdInit = (VkCommandBuffer)draw_->GetNativeObject(Draw::NativeObject::INIT_COMMANDBUFFER);

	delete entry->vkTex;

	VkImageLayout imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	const VkComponentMapping *mapping;
	switch (actualFmt) {
	case VULKAN_4444_FORMAT: mapping = &VULKAN_4444_SWIZZLE; break;
	case VULKAN_1555_FORMAT: mapping = &VULKAN_1555_SWIZZLE; break;
	case VULKAN_565_FORMAT:  mapping = &VULKAN_565_SWIZZLE;  break;
	default:                 mapping = &VULKAN_8888_SWIZZLE; break;  // no channel swizzle
	}

	char texName[64];
	snprintf(texName, sizeof(texName), "tex_%08x_%s_%s", entry->addr, GeTextureFormatToString((GETextureFormat)entry->format, gstate.getClutPaletteFormat()), gstate.isTextureSwizzled() ? "swz" : "lin");
	entry->vkTex = new VulkanTexture(vulkan, texName);
	VulkanTexture *image = entry->vkTex;

	VulkanBarrierBatch barrier;
	bool allocSuccess = image->CreateDirect(plan.createW, plan.createH, plan.depth, plan.levelsToCreate, actualFmt, imageLayout, usage, &barrier, mapping);
	barrier.Flush(cmdInit);
	if (!allocSuccess && !lowMemoryMode_) {
		WARN_LOG_REPORT(Log::G3D, "Texture cache ran out of GPU memory; switching to low memory mode");
		lowMemoryMode_ = true;
		decimationCounter_ = 0;
		Decimate(entry, true);

		// TODO: We should stall the GPU here and wipe things out of memory.
		// As is, it will almost definitely fail the second time, but next frame it may recover.

		allocSuccess = image->CreateDirect(plan.createW, plan.createH, plan.depth, plan.levelsToCreate, actualFmt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &barrier, mapping);
		barrier.Flush(cmdInit);
	}

	if (!allocSuccess) {
		ERROR_LOG(Log::G3D, "Failed to create texture (%dx%d)", plan.w, plan.h);
		delete entry->vkTex;
		entry->vkTex = nullptr;
	}

	if (!entry->vkTex) {
		return;
	}

	VK_PROFILE_BEGIN(vulkan, cmdInit, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		"Texture Upload (%08x) video=%d", entry->addr, plan.isVideo);

	// Upload the texture data. We simply reuse the same loop for 3D texture slices instead of mips, if we have those.
	int levels;
	if (plan.depth > 1) {
		levels = plan.depth;
	} else {
		levels = plan.levelsToLoad;
	}

	VulkanPushPool *pushBuffer = drawEngine_->GetPushBufferForTextureData();

	// Batch the copies.
	TextureCopyBatch copyBatch;
	copyBatch.reserve(levels);

	for (int i = 0; i < levels; i++) {
		int mipUnscaledWidth = gstate.getTextureWidth(i);
		int mipUnscaledHeight = gstate.getTextureHeight(i);

		int mipWidth;
		int mipHeight;
		plan.GetMipSize(i, &mipWidth, &mipHeight);

		int bpp = VkFormatBytesPerPixel(actualFmt);
		// RoundToNextPowerOf2 is probably not necessary as the optimal alignment is gonna be a power of 2.
		int optimalStrideAlignment = RoundToNextPowerOf2(std::max(4, (int)vulkan->GetPhysicalDeviceProperties().properties.limits.optimalBufferCopyRowPitchAlignment));
		int byteStride = RoundUpToMultipleOf(mipWidth * bpp, optimalStrideAlignment);  // output stride
		int pixelStride = byteStride / bpp;
		int uploadSize = byteStride * mipHeight;

		uint32_t bufferOffset;
		VkBuffer texBuf;
		// NVIDIA reports a min alignment of 1 but that can't be healthy... let's align by 16 as a minimum.
		int pushAlignment = std::max(16, (int)vulkan->GetPhysicalDeviceProperties().properties.limits.optimalBufferCopyOffsetAlignment);
		void *data;

		data = pushBuffer->Allocate(uploadSize, pushAlignment, &texBuf, &bufferOffset);
		if (plan.depth != 1) {
			// 3D texturing.
			LoadVulkanTextureLevel(*entry, (uint8_t *)data, byteStride, i, actualFmt);
			entry->vkTex->CopyBufferToMipLevel(cmdInit, &copyBatch, 0, mipWidth, mipHeight, i, texBuf, bufferOffset, pixelStride);
		} else {
			LoadVulkanTextureLevel(*entry, (uint8_t *)data, byteStride, i == 0 ? plan.baseLevelSrc : i, actualFmt);
			entry->vkTex->CopyBufferToMipLevel(cmdInit, &copyBatch, i, mipWidth, mipHeight, 0, texBuf, bufferOffset, pixelStride);
		}
	}

	if (!copyBatch.empty()) {
		VK_PROFILE_BEGIN(vulkan, cmdInit, VK_PIPELINE_STAGE_TRANSFER_BIT, "Copy Upload");
		// Submit the whole batch of mip uploads.
		entry->vkTex->FinishCopyBatch(cmdInit, &copyBatch);
		VK_PROFILE_END(vulkan, cmdInit, VK_PIPELINE_STAGE_TRANSFER_BIT);
	}

	VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	VkPipelineStageFlags prevStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

	// Generate any additional mipmap levels.
	// This will transition the whole stack to GENERAL if it wasn't already.
	if (plan.levelsToLoad < plan.levelsToCreate) {
		VK_PROFILE_BEGIN(vulkan, cmdInit, VK_PIPELINE_STAGE_TRANSFER_BIT, "Mipgen up to level %d", plan.levelsToCreate);
		entry->vkTex->GenerateMips(cmdInit, plan.levelsToLoad, false);
		layout = VK_IMAGE_LAYOUT_GENERAL;
		prevStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VK_PROFILE_END(vulkan, cmdInit, VK_PIPELINE_STAGE_TRANSFER_BIT);
	}

	entry->vkTex->EndCreate(cmdInit, false, prevStage, layout);
	VK_PROFILE_END(vulkan, cmdInit, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	// Signal that we support depth textures so use it as one.
	if (plan.depth > 1) {
		entry->status |= TexCacheEntry::STATUS_3D;
	}

}

VkFormat TextureCacheVulkan::GetDestFormat(GETextureFormat format, GEPaletteFormat clutFormat) {
	if (!gstate_c.Use(GPU_USE_16BIT_FORMATS)) {
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
	switch (format) {
	case GE_TFMT_CLUT4:
	case GE_TFMT_CLUT8:
	case GE_TFMT_CLUT16:
	case GE_TFMT_CLUT32:
		return getClutDestFormatVulkan(clutFormat);
	case GE_TFMT_4444:
		return VULKAN_4444_FORMAT;
	case GE_TFMT_5551:
		return VULKAN_1555_FORMAT;
	case GE_TFMT_5650:
		return VULKAN_565_FORMAT;
	case GE_TFMT_8888:
	case GE_TFMT_DXT1:
	case GE_TFMT_DXT3:
	case GE_TFMT_DXT5:
	default:
		return VULKAN_8888_FORMAT;
	}
}

void TextureCacheVulkan::LoadVulkanTextureLevel(TexCacheEntry &entry, uint8_t *writePtr, int rowPitch, int level, VkFormat dstFmt) {
	GETextureFormat tfmt = (GETextureFormat)entry.format;
	GEPaletteFormat clutformat = gstate.getClutPaletteFormat();
	u32 texaddr = gstate.getTextureAddress(level);

	_assert_msg_(texaddr != 0, "Can't load a texture from address null")

	int bufw = GetTextureBufw(level, texaddr, tfmt);

	TexDecodeFlags texDecFlags{};
	if (!gstate_c.Use(GPU_USE_16BIT_FORMATS) || dstFmt == VULKAN_8888_FORMAT) {
		texDecFlags |= TexDecodeFlags::EXPAND32;
	}
	if (entry.status & TexCacheEntry::STATUS_CLUT_GPU) {
		texDecFlags |= TexDecodeFlags::TO_CLUT8;
	}

	CheckAlphaResult alphaResult = DecodeTextureLevel(writePtr, rowPitch, tfmt, clutformat, texaddr, level, bufw, texDecFlags);
	entry.SetAlphaStatus(alphaResult, level);
}

void TextureCacheVulkan::BoundFramebufferTexture() {
	imageView_ = (VkImageView)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE0_IMAGEVIEW);
}





void *TextureCacheVulkan::GetNativeTextureView(const TexCacheEntry *entry, bool flat) const {
	VkImageView view = flat ? entry->vkTex->GetImageView() : entry->vkTex->GetImageArrayView();
	return (void *)view;
}
