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

#include "ppsspp_config.h"

#include "Common/TimeUtil.h"
#include "Common/GraphicsContext.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"

#include "GPU/Vulkan/GPU_Vulkan.h"


GPUStatistics gpuStats;
GPUCommon *gpu;

bool GPU_Init(GraphicsContext *ctx, Draw::DrawContext *draw) {
	gpu = new GPU_Vulkan(ctx, draw);
	return gpu != nullptr;
}

void GPU_Shutdown() {
	delete gpu;
	gpu = nullptr;
}

const char *RasterChannelToString(RasterChannel channel) {
	return channel == RASTER_COLOR ? "COLOR" : "DEPTH";
}
