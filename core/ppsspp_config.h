#pragma once

// This file is included by C, C++ and ASM files
// So do not output any token!

// One target: the TrimUI Brick, aarch64 Linux, Vulkan.
#define PPSSPP_PLATFORM(PPSSPP_FEATURE) (PPSSPP_PLATFORM_##PPSSPP_FEATURE)
#define PPSSPP_API(PPSSPP_FEATURE) (PPSSPP_API_##PPSSPP_FEATURE)
#define PPSSPP_ARCH(PPSSPP_FEATURE) (PPSSPP_ARCH_##PPSSPP_FEATURE)

#define PPSSPP_PLATFORM_LINUX 1
#define PPSSPP_ARCH_ARM64 1
#define PPSSPP_ARCH_64BIT 1
#define PPSSPP_ARCH_ARM_NEON 1
#define PPSSPP_ARCH_ARM64_NEON 1
