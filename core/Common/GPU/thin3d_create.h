#pragma once

#include <string>
#include <vector>
#include "Common/GPU/thin3d.h"

// Separated this stuff into its own file so we don't get Windows.h included if all we want is the thin3d declarations.


class VulkanContext;

namespace Draw {

DrawContext *T3DCreateGLContext(bool canChangeSwapInterval);


DrawContext *T3DCreateVulkanContext(VulkanContext *context, bool useRenderThread);

}  // namespace Draw
