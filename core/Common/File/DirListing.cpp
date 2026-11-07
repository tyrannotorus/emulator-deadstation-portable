#include "ppsspp_config.h"

#include <strings.h>
#include <dirent.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <set>
#include <cstdio>
#include <sys/stat.h>
#include <cctype>
#include <algorithm>  // remove_if

#include "Common/Data/Encoding/Utf8.h"
#include "Common/StringUtils.h"
#include "Common/Net/URL.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/TimeUtil.h"
#include "Common/Log.h"



// NOTE: There's another one in FileUtil.cpp.
constexpr bool SIMULATE_SLOW_IO = false;
constexpr bool LOG_IO = false;

namespace File {


bool GetFileInfo(const Path &path, FileInfo * fileInfo) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "GetFileInfo %s", path.ToVisualString().c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(300, "slow-io-sim");
	}

	switch (path.Type()) {
	case PathType::NATIVE:
		break;  // OK
	default:
		return false;
	}

	// TODO: Expand relative paths?
	fileInfo->fullName = path;


	struct stat64 file_info;
	int result = stat64(path.c_str(), &file_info);
	if (result < 0) {
		fileInfo->exists = false;
		return false;
	}

	fileInfo->isDirectory = S_ISDIR(file_info.st_mode);
	fileInfo->isWritable = false;
	fileInfo->size = file_info.st_size;
	fileInfo->exists = true;
	fileInfo->atime = file_info.st_atime;
	fileInfo->mtime = file_info.st_mtime;
	fileInfo->ctime = file_info.st_ctime;

	// TODO: Use statx to provide higher precision timestamps on Linux.
	fileInfo->atimeUs = 0;
	fileInfo->mtimeUs = 0;
	fileInfo->ctimeUs = 0;

	fileInfo->access = file_info.st_mode & 0x1ff;
	// HACK: approximation
	if (file_info.st_mode & 0200) {
		fileInfo->isWritable = true;
	}
	return true;
}

bool GetModifTimeT(const Path &filename, time_t *return_time) {
	FileInfo info;
	if (GetFileInfo(filename, &info)) {
		*return_time = info.mtime;
		return true;
	} else {
		*return_time = 0;
		return false;
	}
}

bool GetModifTime(const Path &filename, tm & return_time) {
	memset(&return_time, 0, sizeof(return_time));
	FileInfo info;
	if (GetFileInfo(filename, &info)) {
		time_t t = info.mtime;
		localtime_r((time_t*)&t, &return_time);
		return true;
	} else {
		return false;
	}
}

bool FileInfo::operator <(const FileInfo & other) const {
	if (isDirectory && !other.isDirectory)
		return true;
	else if (!isDirectory && other.isDirectory)
		return false;
	if (strcasecmp(name.c_str(), other.name.c_str()) < 0)
		return true;
	else
		return false;
}

std::vector<File::FileInfo> ApplyFilter(std::vector<File::FileInfo> files, const char *extensionFilter, std::string_view prefix) {
	std::set<std::string> filters;
	if (extensionFilter) {
		std::string tmp;
		while (*extensionFilter) {
			if (*extensionFilter == ':') {
				filters.emplace("." + tmp);
				tmp.clear();
			} else {
				tmp.push_back(*extensionFilter);
			}
			extensionFilter++;
		}
		if (!tmp.empty())
			filters.emplace("." + tmp);
	}

	auto pred = [&](const File::FileInfo &info) {
		// WARNING: Keep in mind that if we return true here, the files is REMOVED from the list.
		// It's not retain_if.
		if (!startsWith(info.name, prefix)) {
			return true;
		}
		if (info.isDirectory || !extensionFilter)
			return false;
		std::string ext = info.fullName.GetFileExtension();
		return filters.find(ext) == filters.end();
	};
	files.erase(std::remove_if(files.begin(), files.end(), pred), files.end());
	return files;
}

bool GetFilesInDir(const Path &directory, std::vector<FileInfo> *files, const char *filter, int flags, std::string_view prefix) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "GetFilesInDir '%s' (ext %s, prefix %.*s)", directory.ToVisualString().c_str(), filter, (int)prefix.size(), prefix.data());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(300, "slow-io-sim");
	}


	std::set<std::string> filters;
	if (filter) {
		std::string tmp;
		while (*filter) {
			if (*filter == ':') {
				filters.insert(tmp);
				tmp.clear();
			} else {
				tmp.push_back(*filter);
			}
			filter++;
		}
		if (!tmp.empty())
			filters.insert(tmp);
	}

	struct dirent *result = NULL;
	DIR *dirp = opendir(directory.c_str());
	if (!dirp)
		return false;
	while ((result = readdir(dirp))) {
		const std::string virtualName(result->d_name);
		// check for "." and ".."
		if (!(flags & GETFILES_GET_NAVIGATION_ENTRIES) && (virtualName == "." || virtualName == ".."))
			continue;

		// Remove dotfiles (optional with flag.)
		if (!(flags & GETFILES_GETHIDDEN)) {
			if (virtualName[0] == '.')
				continue;
		}

		if (!startsWith(virtualName, prefix)) {
			continue;
		}

		// Let's just reuse GetFileInfo. We're calling stat anyway to get isDirectory information.
		Path fullName = directory / virtualName;

		FileInfo info;
		info.name = virtualName;
		if (!GetFileInfo(fullName, &info)) {
			continue;
		}
		if (!info.isDirectory) {
			std::string ext = info.fullName.GetFileExtension();
			if (!ext.empty()) {
				ext = ext.substr(1);  // Remove the dot.
				if (filter && filters.find(ext) == filters.end()) {
					continue;
				}
			}
		}
		files->push_back(info);
	}
	closedir(dirp);
	std::sort(files->begin(), files->end());
	if (LOG_IO) {
		INFO_LOG(Log::IO, "GetFilesInDir: Found %d files", (int)files->size());
	}
	return true;
}


}  // namespace File
