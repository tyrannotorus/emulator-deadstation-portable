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


#include "ppsspp_config.h"

#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Core/FileLoaders/LocalFileLoader.h"


#include <fcntl.h>



void LocalFileLoader::DetectSizeFd() {
#if PPSSPP_PLATFORM(ANDROID) || (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64)
	off64_t off = lseek64(fd_, 0, SEEK_END);
	filesize_ = off;
	lseek64(fd_, 0, SEEK_SET);
#else
	off_t off = lseek(fd_, 0, SEEK_END);
	filesize_ = off;
	lseek(fd_, 0, SEEK_SET);
#endif
}

LocalFileLoader::LocalFileLoader(const Path &filename)
	: filename_(filename) {
	if (filename.empty()) {
		ERROR_LOG(Log::FileSystem, "LocalFileLoader can't load empty filenames");
		return;
	}


	fd_ = open(filename.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd_ == -1) {
		ERROR_LOG(Log::FileSystem, "LocalFileLoader: failed to open file: '%s'", filename.c_str());
		return;
	}

	DetectSizeFd();
}

LocalFileLoader::~LocalFileLoader() {
	if (fd_ != -1) {
		close(fd_);
	}
}

bool LocalFileLoader::Exists() {
	// If we opened it for reading, it must exist.  Done.
	if (isOpenedByFd_) {
		// As an optimization, if we already tried and failed, quickly return.
		// This is used because Android Content URIs are so slow.
		return fd_ != -1;
	}
	if (fd_ != -1)
		return true;

	return File::Exists(filename_);
}

bool LocalFileLoader::IsDirectory() {
	File::FileInfo info;
	if (File::GetFileInfo(filename_, &info)) {
		return info.exists && info.isDirectory;
	}
	return false;
}

s64 LocalFileLoader::FileSize() {
	return filesize_;
}

size_t LocalFileLoader::ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags) {
	if (bytes == 0)
		return 0;

	if (filesize_ == 0) {
		ERROR_LOG(Log::FileSystem, "ReadAt from 0-sized file: %s", filename_.c_str());
		return 0;
	}

#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64
	return pread64(fd_, data, bytes * count, absolutePos) / bytes;
#else
	return pread(fd_, data, bytes * count, absolutePos) / bytes;
#endif
}
