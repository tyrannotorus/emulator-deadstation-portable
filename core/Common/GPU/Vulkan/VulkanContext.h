#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <functional>

#include "Common/Common.h"
#include "Common/Log.h"
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/VulkanDebug.h"
#include "Common/GPU/Vulkan/VulkanAlloc.h"
#include "Common/GPU/Vulkan/VulkanProfiler.h"

// Enable or disable a simple logging profiler for Vulkan.
// Mostly useful for profiling texture uploads currently, but could be useful for
// other things as well. We also have a nice integrated render pass profiler in the queue
// runner, but this one is more convenient for transient events.

#define VK_PROFILE_BEGIN(vulkan, cmd, stage, ...) vulkan->GetProfiler()->Begin(cmd, stage, __VA_ARGS__);
#define VK_PROFILE_END(vulkan, cmd, stage) vulkan->GetProfiler()->End(cmd, stage);

enum class VulkanInitFlags : uint32_t {
	VALIDATE = (1 << 0),
	DISABLE_IMPLICIT_LAYERS = (1 << 5),
};
ENUM_CLASS_BITOPS(VulkanInitFlags);

enum {
	VULKAN_VENDOR_NVIDIA = 0x000010de,
	VULKAN_VENDOR_INTEL = 0x00008086,   // Haha!
	VULKAN_VENDOR_AMD = 0x00001002,
	VULKAN_VENDOR_ARM = 0x000013B5,  // Mali
	VULKAN_VENDOR_QUALCOMM = 0x00005143,
	VULKAN_VENDOR_IMGTEC = 0x00001010,  // PowerVR
	VULKAN_VENDOR_APPLE = 0x0000106b,  // Apple through MoltenVK
	VULKAN_VENDOR_MESA = 0x00010005, // lavapipe
};

VK_DEFINE_HANDLE(VmaAllocator);
VK_DEFINE_HANDLE(VmaAllocation);

std::string VulkanVendorString(uint32_t vendorId);

template<class R, class T> inline void ChainStruct(R &root, T *newStruct) {
	newStruct->pNext = root.pNext;
	root.pNext = newStruct;
}

// The screen: frames are drawn straight into its images, with no surface or swapchain (core/deadstation/panel.h).
class VulkanScreen {
public:
	virtual ~VulkanScreen() = default;
	virtual uint32_t ImageCount() const = 0;
	virtual VkImage Image(uint32_t i) const = 0;
	virtual uint32_t Width() const = 0;
	virtual uint32_t Height() const = 0;
	// The next image to draw into, when it may be written. On the render thread.
	virtual uint32_t Acquire() = 0;
	// Everything for that image submitted to the queue: to be shown once it's done. On the render thread.
	virtual void Present(uint32_t image, VkQueue queue) = 0;
};

struct VulkanPhysicalDeviceInfo {
	VkFormat preferredDepthStencilFormat;
	bool canBlitToPreferredDepthStencilFormat;
};

class VulkanProfiler;
class VulkanContext;

// Extremely rough split of capabilities.
enum class PerfClass {
	SLOW,
	FAST,
};

// This is a bit repetitive...
class VulkanDeleteList {
	struct BufferWithAlloc {
		VkBuffer buffer;
		VmaAllocation alloc;
	};
	struct ImageWithAlloc {
		VkImage image;
		VmaAllocation alloc;
	};

	struct Callback {
		explicit Callback(void(*f)(VulkanContext *vulkan, void *userdata), void *u)
			: func(f), userdata(u) {
		}

		void (*func)(VulkanContext *vulkan, void *userdata);
		void *userdata;
	};

public:
	// NOTE: These all take reference handles so they can zero the input value.
	void QueueDeleteCommandPool(VkCommandPool &pool) { _dbg_assert_(pool != VK_NULL_HANDLE); cmdPools_.push_back(pool); pool = VK_NULL_HANDLE; }
	void QueueDeleteDescriptorPool(VkDescriptorPool &pool) { _dbg_assert_(pool != VK_NULL_HANDLE); descPools_.push_back(pool); pool = VK_NULL_HANDLE; }
	void QueueDeleteShaderModule(VkShaderModule &module) { _dbg_assert_(module != VK_NULL_HANDLE); modules_.push_back(module); module = VK_NULL_HANDLE; }
	void QueueDeleteBuffer(VkBuffer &buffer) { _dbg_assert_(buffer != VK_NULL_HANDLE); buffers_.push_back(buffer); buffer = VK_NULL_HANDLE; }
	void QueueDeleteBufferView(VkBufferView &bufferView) { _dbg_assert_(bufferView != VK_NULL_HANDLE); bufferViews_.push_back(bufferView); bufferView = VK_NULL_HANDLE; }
	void QueueDeleteImageView(VkImageView &imageView) { _dbg_assert_(imageView != VK_NULL_HANDLE); imageViews_.push_back(imageView); imageView = VK_NULL_HANDLE; }
	void QueueDeleteDeviceMemory(VkDeviceMemory &deviceMemory) { _dbg_assert_(deviceMemory != VK_NULL_HANDLE); deviceMemory_.push_back(deviceMemory); deviceMemory = VK_NULL_HANDLE; }
	void QueueDeleteSampler(VkSampler &sampler) { _dbg_assert_(sampler != VK_NULL_HANDLE); samplers_.push_back(sampler); sampler = VK_NULL_HANDLE; }
	void QueueDeletePipeline(VkPipeline &pipeline) { _dbg_assert_(pipeline != VK_NULL_HANDLE); pipelines_.push_back(pipeline); pipeline = VK_NULL_HANDLE; }
	void QueueDeletePipelineCache(VkPipelineCache &pipelineCache) { _dbg_assert_(pipelineCache != VK_NULL_HANDLE); pipelineCaches_.push_back(pipelineCache); pipelineCache = VK_NULL_HANDLE; }
	void QueueDeleteRenderPass(VkRenderPass &renderPass) { _dbg_assert_(renderPass != VK_NULL_HANDLE); renderPasses_.push_back(renderPass); renderPass = VK_NULL_HANDLE; }
	void QueueDeleteFramebuffer(VkFramebuffer &framebuffer) { _dbg_assert_(framebuffer != VK_NULL_HANDLE); framebuffers_.push_back(framebuffer); framebuffer = VK_NULL_HANDLE; }
	void QueueDeletePipelineLayout(VkPipelineLayout &pipelineLayout) { _dbg_assert_(pipelineLayout != VK_NULL_HANDLE); pipelineLayouts_.push_back(pipelineLayout); pipelineLayout = VK_NULL_HANDLE; }
	void QueueDeleteDescriptorSetLayout(VkDescriptorSetLayout &descSetLayout) { _dbg_assert_(descSetLayout != VK_NULL_HANDLE); descSetLayouts_.push_back(descSetLayout); descSetLayout = VK_NULL_HANDLE; }
	void QueueDeleteQueryPool(VkQueryPool &queryPool) { _dbg_assert_(queryPool != VK_NULL_HANDLE); queryPools_.push_back(queryPool); queryPool = VK_NULL_HANDLE; }
	void QueueCallback(void (*func)(VulkanContext *vulkan, void *userdata), void *userdata) { callbacks_.push_back(Callback(func, userdata)); }

	void QueueDeleteBufferAllocation(VkBuffer &buffer, VmaAllocation &alloc) { 
		_dbg_assert_(buffer != VK_NULL_HANDLE); 
		buffersWithAllocs_.push_back(BufferWithAlloc{ buffer, alloc });
		buffer = VK_NULL_HANDLE;
		alloc = VK_NULL_HANDLE;
	}
	void QueueDeleteImageAllocation(VkImage &image, VmaAllocation &alloc) {
		_dbg_assert_(image != VK_NULL_HANDLE && alloc != VK_NULL_HANDLE);
		imagesWithAllocs_.push_back(ImageWithAlloc{ image, alloc });
		image = VK_NULL_HANDLE;
		alloc = VK_NULL_HANDLE;
	}

	void Take(VulkanDeleteList &del);
	void PerformDeletes(VulkanContext *vulkan, VmaAllocator allocator);

	int GetLastDeleteCount() const {
		return deleteCount_;
	}

private:
	std::vector<VkCommandPool> cmdPools_;
	std::vector<VkDescriptorPool> descPools_;
	std::vector<VkShaderModule> modules_;
	std::vector<VkBuffer> buffers_;
	std::vector<BufferWithAlloc> buffersWithAllocs_;
	std::vector<VkBufferView> bufferViews_;
	std::vector<ImageWithAlloc> imagesWithAllocs_;
	std::vector<VkImageView> imageViews_;
	std::vector<VkDeviceMemory> deviceMemory_;
	std::vector<VkSampler> samplers_;
	std::vector<VkPipeline> pipelines_;
	std::vector<VkPipelineCache> pipelineCaches_;
	std::vector<VkRenderPass> renderPasses_;
	std::vector<VkFramebuffer> framebuffers_;
	std::vector<VkPipelineLayout> pipelineLayouts_;
	std::vector<VkDescriptorSetLayout> descSetLayouts_;
	std::vector<VkQueryPool> queryPools_;
	std::vector<Callback> callbacks_;
	int deleteCount_ = 0;
};

// VulkanContext manages the device and swapchain, and deferred deletion of objects.
class VulkanContext {
public:
	VulkanContext();
	~VulkanContext();

	struct CreateInfo {
		const char *app_name;
		int app_ver;
		VulkanInitFlags flags;
		std::string customDriver;
	};

	VkResult CreateInstance(const CreateInfo &info);
	void DestroyInstance();

	int GetBestPhysicalDevice() const;
	int GetPhysicalDeviceByName(std::string_view name) const;

	// Convenience method to avoid code duplication.
	// If it returns false, delete the context.
	bool CreateInstanceAndDevice(const CreateInfo &info);

	// The coreVersion is to avoid enabling extensions that are merged into core Vulkan from a certain version.
	bool EnableInstanceExtension(const char *extension, uint32_t coreVersion);
	bool EnableDeviceExtension(const char *extension, uint32_t coreVersion);

	// Was previously two functions, ChooseDevice and CreateDevice.
	VkResult CreateDevice(int physical_device);

	const std::string &InitError() const { return init_error_; }

	VkDevice GetDevice() const { return device_; }
	VkInstance GetInstance() const { return instance_; }
	VulkanInitFlags GetInitFlags() const { return createInfo_.flags; }

	// Of course, this won't update things that can only change on first init.
	void UpdateCreateInfo(const VulkanContext::CreateInfo &info) { createInfo_ = info; }

	VulkanDeleteList &Delete() { return globalDeleteList_; }

	// The screen frames go to, once the device exists. Stays the caller's.
	void InitScreen(VulkanScreen *screen);
	VulkanScreen *Screen() const { return screen_; }

	void DestroyDevice();

	void PerformPendingDeletes();
	void WaitUntilQueueIdle();

	// Utility functions for shorter code
	VkFence CreateFence(bool presignalled);
	bool CreateShaderModule(const std::vector<uint32_t> &spirv, VkShaderModule *shaderModule, const char *tag);

	void BeginFrame(VkCommandBuffer firstCommandBuffer);
	void EndFrame();

	VulkanProfiler *GetProfiler() {
		return &frame_[curFrame_].profiler;
	}

	// Simple workaround for the casting warning.
	template <class T>
	void SetDebugName(T handle, VkObjectType type, const char *name) {
		if (extensionsLookup_.EXT_debug_utils && handle != VK_NULL_HANDLE) {
			_dbg_assert_(handle != VK_NULL_HANDLE);
			SetDebugNameImpl((uint64_t)handle, type, name);
		}
	}
	bool DebugLayerEnabled() const {
		return extensionsLookup_.EXT_debug_utils;
	}

	bool MemoryTypeFromProperties(uint32_t typeBits, VkFlags requirements_mask, uint32_t *typeIndex);

	VkPhysicalDevice GetPhysicalDevice(int n) const {
		return physical_devices_[n];
	}
	VkPhysicalDevice GetCurrentPhysicalDevice() const {
		return physical_devices_[physical_device_];
	}
	int GetCurrentPhysicalDeviceIndex() const {
		return physical_device_;
	}
	int GetNumPhysicalDevices() const {
		return (int)physical_devices_.size();
	}

	VkQueue GetGraphicsQueue() const {
		return gfx_queue_;
	}

	int GetGraphicsQueueFamilyIndex() const {
		return graphics_queue_family_index_;
	}

	struct PhysicalDeviceProps {
		VkPhysicalDeviceProperties properties;
		VkPhysicalDevicePushDescriptorPropertiesKHR pushDescriptorProperties;
		VkPhysicalDeviceExternalMemoryHostPropertiesEXT externalMemoryHostProperties;
		VkPhysicalDeviceDepthStencilResolveProperties depthStencilResolve;
	};

	struct AllPhysicalDeviceFeatures {
		VkPhysicalDeviceFeatures standard;
		VkPhysicalDeviceMultiviewFeatures multiview;
		VkPhysicalDeviceProvokingVertexFeaturesEXT provokingVertex;
		VkPhysicalDeviceScalarBlockLayoutFeatures scalarBlockLayout;
	};

	const PhysicalDeviceProps &GetPhysicalDeviceProperties(int i = -1) const {
		if (i < 0)
			i = GetCurrentPhysicalDeviceIndex();
		return physicalDeviceProperties_[i];
	}

	const VkQueueFamilyProperties &GetQueueFamilyProperties(int family) const {
		return queueFamilyProperties_[family];
	}

	VkResult GetInstanceLayerExtensionList(const char *layerName, std::vector<VkExtensionProperties> *extensions);
	VkResult GetInstanceLayerProperties();

	VkResult GetDeviceExtensionList(std::vector<VkExtensionProperties> *extensions);

	const std::vector<VkExtensionProperties> &GetDeviceExtensionsAvailable() const {
		return device_extension_properties_;
	}
	const std::vector<const char *> &GetDeviceExtensionsEnabled() const {
		return device_extensions_enabled_;
	}

	const std::vector<VkExtensionProperties> &GetInstanceExtensionsAvailable() const {
		return instance_extension_properties_;
	}
	const std::vector<const char *> &GetInstanceExtensionsEnabled() const {
		return instance_extensions_enabled_;
	}

	const VkPhysicalDeviceMemoryProperties &GetMemoryProperties() const {
		return memory_properties_;
	}

	struct PhysicalDeviceFeatures {
		AllPhysicalDeviceFeatures available{};
		AllPhysicalDeviceFeatures enabled{};
	};

	const PhysicalDeviceFeatures &GetDeviceFeatures() const { return deviceFeatures_; }
	const VulkanPhysicalDeviceInfo &GetDeviceInfo() const { return deviceInfo_; }

	bool IsInstanceExtensionAvailable(const char *extensionName) const {
		for (const auto &iter : instance_extension_properties_) {
			if (!strcmp(extensionName, iter.extensionName))
				return true;
		}

		// Also search through the layers, one of them might carry the extension (especially DEBUG_utils)
		for (const auto &iter : instance_layer_properties_) {
			for (const auto &ext : iter.extensions) {
				if (!strcmp(extensionName, ext.extensionName)) {
					return true;
				}
			}
		}

		return false;
	}

	bool IsDeviceExtensionAvailable(const char *name) const {
		for (auto &iter : device_extension_properties_) {
			if (!strcmp(name, iter.extensionName))
				return true;
		}
		return false;
	}

	int GetInflightFrames() const {
		// out of MAX_INFLIGHT_FRAMES.
		return inflightFrames_;
	}

	// Don't call while a frame is in progress.
	void UpdateInflightFrames(int n);

	int GetCurFrame() const {
		return curFrame_;
	}

	VkFormat GetSwapchainFormat() const { return VK_FORMAT_B8G8R8A8_UNORM; }
	bool IsSwapchainInited() const { return screen_ != nullptr; }

	int GetBackbufferWidth() { return screen_ ? (int)screen_->Width() : 0; }
	int GetBackbufferHeight() { return screen_ ? (int)screen_->Height() : 0; }

	void SetProfilerEnabledPtr(bool *enabled) {
		for (auto &frame : frame_) {
			frame.profiler.SetEnabledPtr(enabled);
		}
	}

	// 1 for no frame overlap and thus minimal latency but worst performance.
	// 2 is an OK compromise, while 3 performs best but risks slightly higher latency.
	enum {
		MAX_INFLIGHT_FRAMES = 3,
	};

	const VulkanExtensions &Extensions() { return extensionsLookup_; }

	PerfClass DevicePerfClass() const {
		return devicePerfClass_;
	}

	void GetImageMemoryRequirements(VkImage image, VkMemoryRequirements *mem_reqs, bool *dedicatedAllocation);

	VmaAllocator Allocator() const {
		return allocator_;
	}





	int GetLastDeleteCount() const {
		return frame_[curFrame_].deleteList.GetLastDeleteCount();
	}

	u32 InstanceApiVersion() const {
		return vulkanInstanceApiVersion_;
	}

	u32 DeviceApiVersion() const {
		return vulkanDeviceApiVersion_;
	}


private:

	void SetDebugNameImpl(uint64_t handle, VkObjectType type, const char *name);

	VkResult InitDebugUtilsCallback();

	// A layer can expose extensions, keep track of those extensions here.
	struct LayerProperties {
		VkLayerProperties properties;
		std::vector<VkExtensionProperties> extensions;
	};

	bool CheckLayers(const std::vector<LayerProperties> &layer_props, const std::vector<const char *> &layer_names) const;


	// Don't use the real types here to avoid having to include platform-specific stuff
	// that we really don't want in everything that uses VulkanContext.

	VkInstance instance_ = VK_NULL_HANDLE;
	VulkanScreen *screen_ = nullptr;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue gfx_queue_ = VK_NULL_HANDLE;
	u32 vulkanInstanceApiVersion_ = 0;
	u32 vulkanDeviceApiVersion_ = 0;

	std::string init_error_;
	std::vector<const char *> instance_layer_names_;
	std::vector<LayerProperties> instance_layer_properties_;

	std::vector<const char *> instance_extensions_enabled_;
	std::vector<VkExtensionProperties> instance_extension_properties_;

	std::vector<const char *> device_extensions_enabled_;
	std::vector<VkExtensionProperties> device_extension_properties_;
	VulkanExtensions extensionsLookup_{};

	std::vector<VkPhysicalDevice> physical_devices_;

	int physical_device_ = -1;

	uint32_t graphics_queue_family_index_ = -1;
	std::vector<PhysicalDeviceProps> physicalDeviceProperties_;
	std::vector<VkQueueFamilyProperties> queueFamilyProperties_;

	VkPhysicalDeviceMemoryProperties memory_properties_{};

	// Custom collection of things that are good to know
	VulkanPhysicalDeviceInfo deviceInfo_{};

	// Swap chain extent

	VulkanContext::CreateInfo createInfo_{};

	PerfClass devicePerfClass_ = PerfClass::SLOW;

	int inflightFrames_ = MAX_INFLIGHT_FRAMES;

	struct FrameData {
		FrameData() {}
		VulkanDeleteList deleteList;
		VulkanProfiler profiler;
	};
	FrameData frame_[MAX_INFLIGHT_FRAMES];
	int curFrame_ = 0;

	// At the end of the frame, this is copied into the frame's delete list, so it can be processed
	// the next time the frame comes around again.
	VulkanDeleteList globalDeleteList_;

	std::vector<VkDebugUtilsMessengerEXT> utils_callbacks;


	uint32_t queue_count = 0;

	PhysicalDeviceFeatures deviceFeatures_;



	std::vector<VkCommandBuffer> cmdQueue_;

	VmaAllocator allocator_ = VK_NULL_HANDLE;
};

// GLSL compiler
void init_glslang();
void finalize_glslang();

enum class GLSLVariant {
	VULKAN,
	GL140,
	GLES300,
};

bool GLSLtoSPV(const VkShaderStageFlagBits shader_type, const char *sourceCode, GLSLVariant variant, std::vector<uint32_t> &spirv, std::string *errorMessage);

const char *VulkanFormatToString(VkFormat format);
const char *VulkanImageLayoutToString(VkImageLayout imageLayout);

std::string FormatDriverVersion(const VkPhysicalDeviceProperties &props);
std::string FormatAPIVersion(u32 version);

// Simple heuristic.
bool IsHashMaliDriverVersion(const VkPhysicalDeviceProperties &props);

extern VulkanLogOptions g_LogOptions;
