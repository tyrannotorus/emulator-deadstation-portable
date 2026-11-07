// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "ppsspp_config.h"

#include <cstring>
#include <cstdlib>

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/StringUtils.h"
#include "Common/SysError.h"
#include "Common/Data/Text/Parsers.h"

#include <cerrno>
#include <cstdio>


#include <unistd.h>
static int hint_location;
#define MEM_PAGE_SIZE (getpagesize())

#define MEM_PAGE_MASK ((MEM_PAGE_SIZE)-1)
#define ppsspp_round_page(x) ((((uintptr_t)(x)) + MEM_PAGE_MASK) & ~(MEM_PAGE_MASK))


static uint32_t ConvertProtFlagsUnix(uint32_t flags) {
	uint32_t protect = 0;
	if (flags & MEM_PROT_READ)
		protect |= PROT_READ;
	if (flags & MEM_PROT_WRITE)
		protect |= PROT_WRITE;
	if (flags & MEM_PROT_EXEC)
		protect |= PROT_EXEC;
	return protect;
}




void *AllocateExecutableMemory(size_t size) {
	static char *map_hint = nullptr;


	int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
	if (PlatformIsWXExclusive())
		prot = PROT_READ | PROT_WRITE;  // POST_EXEC is added later in this case.

	void* ptr = mmap(map_hint, size, prot, MAP_ANON | MAP_PRIVATE, -1, 0);

	if (ptr == MAP_FAILED) {
		ptr = nullptr;
		ERROR_LOG(Log::MemMap, "Failed to allocate executable memory (%d) errno=%d", (int)size, errno);
	}


	return ptr;
}


void *AllocateMemoryPages(size_t size, uint32_t memProtFlags) {
	size = ppsspp_round_page(size);
	uint32_t protect = ConvertProtFlagsUnix(memProtFlags);
	void *ptr = mmap(0, size, protect, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (ptr == MAP_FAILED) {
		ERROR_LOG(Log::MemMap, "Failed to allocate raw memory pages: errno=%d", errno);
		return nullptr;
	}

	// printf("Mapped memory at %p (size %ld)\n", ptr,
	//	(unsigned long)size);
	return ptr;
}

void *AllocateAlignedMemory(size_t size, size_t alignment) {
	void* ptr = NULL;
	if (posix_memalign(&ptr, alignment, size) != 0) {
		ptr = nullptr;
	}
	return ptr;
}

void FreeMemoryPages(void *ptr, size_t size) {
	if (!ptr)
		return;
	uintptr_t page_size = GetMemoryProtectPageSize();
	size = (size + page_size - 1) & (~(page_size - 1));
	munmap(ptr, size);
}

void FreeExecutableMemory(void *ptr, size_t size) {
	FreeMemoryPages(ptr, size);
}

void FreeAlignedMemory(void* ptr) {
	if (!ptr)
		return;
	free(ptr);
}

bool PlatformIsWXExclusive() {
	// Needed on platforms that disable W^X pages for security. Even without block linking, still should be much faster than IR JIT.
	// This might also come in useful for UWP (Universal Windows Platform) if I'm understanding things correctly.
	// Returning true here lets you test the W^X path on Windows and other non-W^X platforms.
	return false;
}

bool ProtectMemoryPages(const void* ptr, size_t size, uint32_t memProtFlags) {
	VERBOSE_LOG(Log::JIT, "ProtectMemoryPages: %p (%d) : r%d w%d x%d", ptr, (int)size,
			(memProtFlags & MEM_PROT_READ) != 0, (memProtFlags & MEM_PROT_WRITE) != 0, (memProtFlags & MEM_PROT_EXEC) != 0);

	if (PlatformIsWXExclusive()) {
		if ((memProtFlags & (MEM_PROT_WRITE | MEM_PROT_EXEC)) == (MEM_PROT_WRITE | MEM_PROT_EXEC)) {
			_assert_msg_(false, "Bad memory protect flags %d: W^X is in effect, can't both write and exec", memProtFlags);
		}
	}
	// Note - VirtualProtect will affect the full pages containing the requested range.
	// mprotect does not seem to, at least not on Android unless I made a mistake somewhere, so we manually round.
	uint32_t protect = ConvertProtFlagsUnix(memProtFlags);
	uintptr_t page_size = GetMemoryProtectPageSize();

	uintptr_t start = (uintptr_t)ptr;
	uintptr_t end = (uintptr_t)ptr + size;
	start &= ~(page_size - 1);
	end = (end + page_size - 1) & ~(page_size - 1);
	int retval = mprotect((void *)start, end - start, protect);
	if (retval != 0) {
		ERROR_LOG(Log::MemMap, "mprotect failed (%p)! errno=%d (%s)", (void *)start, errno, strerror(errno));
		return false;
	}
	return true;
}

int GetMemoryProtectPageSize() {
	static int pageSize = 0;
	if (!pageSize) {
		pageSize = sysconf(_SC_PAGE_SIZE);
	}
	return pageSize;
}
