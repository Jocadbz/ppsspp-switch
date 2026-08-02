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

#if !defined(_WIN32) && !defined(ANDROID) && !defined(__APPLE__)

#ifndef HAVE_LIBNX
#include <sys/mman.h>
#endif // !HAVE_LIBNX
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

#ifndef MAP_NORESERVE
// Not implemented on BSDs
#define MAP_NORESERVE 0
#endif

#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Common/MemoryUtil.h"
#include "Common/MemArena.h"

static const std::string tmpfs_location = "/dev/shm";
static const std::string tmpfs_ram_temp_file = "/dev/shm/gc_mem.tmp";

#ifdef HAVE_LIBNX
#include <malloc.h> // memalign
static constexpr size_t SWITCH_MEMORY_SIZE = 0x10000000;
static constexpr size_t SWITCH_PAGE_SIZE = 0x1000;
static uintptr_t memoryBase = 0;
static uintptr_t memoryCodeBase = 0;
static uintptr_t memorySrcBase = 0;
static VirtmemReservation *memoryReservation = nullptr;
static VirtmemReservation *memoryCodeReservation = nullptr;
static bool memoryCodeMapped = false;
#endif

// do not make this "static"
std::string ram_temp_file = "/tmp/gc_mem.tmp";

size_t MemArena::roundup(size_t x) {
	return x;
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
#ifndef HAVE_LIBNX
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	// Try a few times in case multiple instances are started near each other.
	char ram_temp_filename[128]{};
	bool is_shm = false;
	for (int i = 0; i < 256; ++i) {
		snprintf(ram_temp_filename, sizeof(ram_temp_filename), "/ppsspp_%d.ram", i);
		// This opens atomically, so will fail if another process is starting.
		fd = shm_open(ram_temp_filename, O_RDWR | O_CREAT | O_EXCL, mode);
		if (fd >= 0) {
			INFO_LOG(MEMMAP, "Got shm file: %s", ram_temp_filename);
			is_shm = true;
			// Our handle persists per POSIX, so no need to keep it around.
			if (shm_unlink(ram_temp_filename) != 0) {
				WARN_LOG(MEMMAP, "Failed to shm_unlink %s", ram_temp_file.c_str());
			}
			break;
		}
	}

	// Fall back to old tmpfs behavior.
	if (fd < 0 && File::Exists(Path(tmpfs_location))) {
		fd = open(tmpfs_ram_temp_file.c_str(), O_RDWR | O_CREAT, mode);
		if (fd >= 0) {
			// Great, this definitely shouldn't flush to disk.
			ram_temp_file = tmpfs_ram_temp_file;
			INFO_LOG(MEMMAP, "Got tmpfs ram file: %s", tmpfs_ram_temp_file.c_str());
		}
	}

	if (fd < 0) {
		INFO_LOG(MEMMAP, "Trying '%s' as ram temp file", ram_temp_file.c_str());
		fd = open(ram_temp_file.c_str(), O_RDWR | O_CREAT, mode);
	}
	if (fd < 0) {
		ERROR_LOG(MEMMAP, "Failed to grab memory space as a file: %s of size: %08x. Error: %s", ram_temp_file.c_str(), (int)size, strerror(errno));
		return false;
	}
	// delete immediately, we keep the fd so it still lives
	if (!is_shm && unlink(ram_temp_file.c_str()) != 0) {
		WARN_LOG(MEMMAP, "Failed to unlink %s", ram_temp_file.c_str());
	}
	if (ftruncate(fd, size) != 0) {
		ERROR_LOG(MEMMAP, "Failed to ftruncate %d (%s) to size %08x", (int)fd, ram_temp_file.c_str(), (int)size);
		// Should this be a failure?
	}

	return true;
#else
	return true;
#endif
}

void MemArena::ReleaseSpace() {
#ifndef HAVE_LIBNX
	close(fd);
#else
	if (memoryCodeMapped) {
		Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
			static_cast<u64>(memoryCodeBase), static_cast<u64>(memorySrcBase), SWITCH_MEMORY_SIZE);
		if (R_FAILED(rc)) {
			printf("Failed to release code memory (result: 0x%08x)\n", static_cast<unsigned>(rc));
			return;
		}
		memoryCodeMapped = false;
	}

	if (memorySrcBase) {
		free(reinterpret_cast<void *>(memorySrcBase));
		memorySrcBase = 0;
	}

	if (memoryReservation || memoryCodeReservation) {
		virtmemLock();
		if (memoryCodeReservation) {
			virtmemRemoveReservation(memoryCodeReservation);
			memoryCodeReservation = nullptr;
		}
		if (memoryReservation) {
			virtmemRemoveReservation(memoryReservation);
			memoryReservation = nullptr;
		}
		virtmemUnlock();
	}

	memoryBase = 0;
	memoryCodeBase = 0;
#endif
}

void *MemArena::CreateView(s64 offset, size_t size, void *base)
{
#ifdef HAVE_LIBNX
	const u64 source = static_cast<u64>(memoryCodeBase) + static_cast<u64>(offset);
	Result rc = svcMapProcessMemory(base, envGetOwnProcessHandle(), source, size);
	if (R_FAILED(rc)) {
		printf("Failed to create view at %p from %p (size: 0x%zx, result: 0x%08x)\n",
			base, reinterpret_cast<void *>(source), size, static_cast<unsigned>(rc));
		return nullptr;
	}

	return base;
#else
	void *retval = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED |
// Do not sync memory to underlying file. Linux has this by default.
#if defined(__DragonFly__) || defined(__FreeBSD__)
		MAP_NOSYNC |
#endif
		((base == 0) ? 0 : MAP_FIXED), fd, offset);

	if (retval == MAP_FAILED) {
		NOTICE_LOG(MEMMAP, "mmap on %s (fd: %d) failed: %s", ram_temp_file.c_str(), (int)fd, strerror(errno));
		return 0;
	}
	return retval;
#endif
}

void MemArena::ReleaseView(s64 offset, void* view, size_t size) {
#ifndef HAVE_LIBNX
	munmap(view, size);
#else
	const u64 source = static_cast<u64>(memoryCodeBase) + static_cast<u64>(offset);
	Result rc = svcUnmapProcessMemory(view, envGetOwnProcessHandle(), source, size);
	if (R_FAILED(rc))
		printf("Failed to unmap view at %p (result: 0x%08x)\n", view, static_cast<unsigned>(rc));
#endif // HAVE_LIBNX
}

u8* MemArena::Find4GBBase() {
	// Now, create views in high memory where there's plenty of space.
#if defined(HAVE_LIBNX) && PPSSPP_ARCH(64BIT) && !defined(USE_ASAN)
	// Very precarious - mmap cannot return an error when trying to map already used pages.
	// This makes the Windows approach above unusable on Linux, so we will simply pray...
	memorySrcBase = reinterpret_cast<uintptr_t>(memalign(SWITCH_PAGE_SIZE, SWITCH_MEMORY_SIZE));
	if (!memorySrcBase) {
		printf("Failed to allocate code memory source\n");
		return nullptr;
	}

	virtmemLock();
	void *base = virtmemFindAslr(SWITCH_MEMORY_SIZE, 0);
	if (!base) {
		virtmemUnlock();
		printf("Failed to find address space for memory base\n");
		ReleaseSpace();
		return nullptr;
	}
	memoryReservation = virtmemAddReservation(base, SWITCH_MEMORY_SIZE);
	if (!memoryReservation) {
		virtmemUnlock();
		printf("Failed to reserve address space for memory base\n");
		ReleaseSpace();
		return nullptr;
	}
	memoryBase = reinterpret_cast<uintptr_t>(base);

	void *codeBase = virtmemFindCodeMemory(SWITCH_MEMORY_SIZE, 0);
	if (!codeBase) {
		virtmemUnlock();
		printf("Failed to find address space for code memory\n");
		ReleaseSpace();
		return nullptr;
	}
	memoryCodeReservation = virtmemAddReservation(codeBase, SWITCH_MEMORY_SIZE);
	if (!memoryCodeReservation) {
		virtmemUnlock();
		printf("Failed to reserve address space for code memory\n");
		ReleaseSpace();
		return nullptr;
	}
	memoryCodeBase = reinterpret_cast<uintptr_t>(codeBase);
	virtmemUnlock();

	Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
		static_cast<u64>(memoryCodeBase), static_cast<u64>(memorySrcBase), SWITCH_MEMORY_SIZE);
	if (R_FAILED(rc)) {
		printf("Failed to map code memory at %p from %p (result: 0x%08x)\n",
			reinterpret_cast<void *>(memoryCodeBase), reinterpret_cast<void *>(memorySrcBase), static_cast<unsigned>(rc));
		ReleaseSpace();
		return nullptr;
	}
	memoryCodeMapped = true;

	rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
		static_cast<u64>(memoryCodeBase), SWITCH_MEMORY_SIZE, Perm_Rx);
	if (R_FAILED(rc)) {
		printf("Failed to set code memory permissions at %p (result: 0x%08x)\n",
			reinterpret_cast<void *>(memoryCodeBase), static_cast<unsigned>(rc));
		ReleaseSpace();
		return nullptr;
	}

	return reinterpret_cast<u8 *>(memoryBase);
#else
	size_t size = 0x10000000;
	void* base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED | MAP_NORESERVE, -1, 0);
	_assert_msg_(base != MAP_FAILED, "Failed to map 256 MB of memory space: %s", strerror(errno));
	munmap(base, size);
	return static_cast<u8*>(base);
#endif
}

#endif
