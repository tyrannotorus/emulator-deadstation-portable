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

#define _POSIX_SOURCE
#define _LARGE_TIME_API

#include "ppsspp_config.h"


#include <cstring>
#include <ctime>
#include <memory>

#include <sys/types.h>

#include "Common/Log.h"
#include "Common/LogReporting.h"
#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"
#include "Common/SysError.h"

#include <sys/param.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>




#include "Common/Data/Encoding/Utf8.h"

#include <sys/stat.h>

// NOTE: There's another one in DirListing.cpp.
constexpr bool SIMULATE_SLOW_IO = false;
constexpr bool LOG_IO = false;

#ifndef S_ISDIR
#define S_ISDIR(m)  (((m)&S_IFMT) == S_IFDIR)
#endif


#define DIR_SEP "/"
#define DIR_SEP_CHRS "/"


// This namespace has various generic functions related to files and paths.
// The code still needs a ton of cleanup.
// REMEMBER: strdup considered harmful!
namespace File {


FILE *OpenCFile(const Path &path, const char *mode) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "OpenCFile %s, %s", path.c_str(), mode);
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(300, "slow-io-sim");
	}
	switch (path.Type()) {
	case PathType::NATIVE:
		break;
	default:
		ERROR_LOG(Log::IO, "OpenCFile(%s): PathType not yet supported", path.c_str());
		return nullptr;
	}

	return fopen(path.c_str(), mode);
}

static std::string OpenFlagToString(OpenFlag flags) {
	std::string s;
	if (flags & OPEN_READ)
		s += "READ|";
	if (flags & OPEN_WRITE)
		s += "WRITE|";
	if (flags & OPEN_APPEND)
		s += "APPEND|";
	if (flags & OPEN_CREATE)
		s += "CREATE|";
	if (flags & OPEN_TRUNCATE)
		s += "TRUNCATE|";
	if (!s.empty()) {
		s.pop_back();  // Remove trailing separator.
	}
	return s;
}



void CloseFD(int fd) {
}



// Canonicalize the given path, resolving symlinks, relative paths, etc.
std::string ResolvePath(std::string_view path) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "ResolvePath %.*s", (int)path.size(), path.data());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
	}



	std::unique_ptr<char[]> buf(new char[PATH_MAX + 32768]);
	std::string spath(path);
	if (realpath(spath.c_str(), buf.get()) == nullptr)
		return spath;
	return std::string(buf.get());
}

static int64_t RecursiveSize(const Path &path) {
	// TODO: Some file systems can optimize this.
	std::vector<FileInfo> fileInfo;
	if (!GetFilesInDir(path, &fileInfo, nullptr, GETFILES_GETHIDDEN)) {
		return -1;
	}
	int64_t sizeSum = 0;
	for (const auto &file : fileInfo) {
		if (file.isDirectory) {
			sizeSum += RecursiveSize(file.fullName);
		} else {
			sizeSum += file.size;
		}
	}
	return sizeSum;
}

uint64_t ComputeRecursiveDirectorySize(const Path &path) {

	// Generic solution.
	return RecursiveSize(path);
}

// Returns true if file filename exists. Will return true on directories.
bool ExistsInDir(const Path &path, std::string_view filename) {
	return Exists(path / filename);
}

bool Exists(const Path &path) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "Exists %s", path.ToVisualString().c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(200, "slow-io-sim");
	}


	struct stat file_info{};
	return stat(path.c_str(), &file_info) == 0;
}

// Returns true if filename exists and is a directory
bool IsDirectory(const Path &path) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "IsDirectory %s", path.c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
	}

	switch (path.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	std::string copy = path.ToString();
	struct stat file_info{};
	int result = stat(copy.c_str(), &file_info);
	if (result < 0) {
		WARN_LOG(Log::IO, "IsDirectory: stat failed on %s: %s", copy.c_str(), GetLastErrorMsg().c_str());
		return false;
	}
	return S_ISDIR(file_info.st_mode);
}

// Deletes a given filename, return true on success
// Doesn't supports deleting a directory
bool Delete(const Path &filename, bool quiet) {
	if (SIMULATE_SLOW_IO) {
		sleep_ms(200, "slow-io-sim");
	}
	switch (filename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	// Return true because we care about the file no
	// being there, not the actual delete.
	if (!Exists(filename)) {
		if (!quiet) {
			WARN_LOG(Log::IO, "Delete: '%s' already does not exist", filename.c_str());
		}
		return true;
	}

	// We can't delete a directory
	if (IsDirectory(filename)) {
		WARN_LOG(Log::IO, "Delete failed: '%s' is a directory", filename.c_str());
		return false;
	}

	if (unlink(filename.c_str()) == -1) {
		WARN_LOG(Log::IO, "Delete: unlink failed on %s: %s",
				 filename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}

	INFO_LOG(Log::IO, "Delete: file %s was deleted.", filename.c_str());
	return true;
}

// Returns true if successful, or path already exists.
bool CreateDir(const Path &path) {
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
		INFO_LOG(Log::IO, "CreateDir %s", path.c_str());
	}
	switch (path.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	DEBUG_LOG(Log::IO, "CreateDir('%s')", path.c_str());
	if (mkdir(path.ToString().c_str(), 0755) == 0) {
		return true;
	}

	int err = errno;
	if (err == EEXIST) {
		DEBUG_LOG(Log::IO, "CreateDir: mkdir failed on %s: already exists", path.c_str());
		return true;
	}

	ERROR_LOG(Log::IO, "CreateDir: mkdir failed on %s: %s", path.c_str(), strerror(err));
	return false;
}

// Creates the full path of fullPath returns true on success
bool CreateFullPath(const Path &path) {
	if (File::Exists(path)) {
		DEBUG_LOG(Log::IO, "CreateFullPath: path exists %s", path.ToVisualString().c_str());
		return true;
	}

	switch (path.Type()) {
	case PathType::NATIVE:
		break;
	default:
		ERROR_LOG(Log::IO, "CreateFullPath(%s): Not yet supported", path.ToVisualString().c_str());
		return false;
	}

	// The below code is entirely agnostic of path format.

	Path root = path.GetRootVolume();

	std::string diff;
	if (!root.ComputePathTo(path, diff)) {
		return false;
	}

	std::vector<std::string_view> parts;
	if (!diff.empty()) {
		SplitString(diff, '/', parts);
	}

	// Probably not necessary sanity check, ported from the old code.
	if (parts.size() > 100) {
		ERROR_LOG(Log::IO, "CreateFullPath: directory structure too deep");
		return false;
	}

	Path curPath = root;
	for (auto part : parts) {
		curPath /= part;
		File::CreateDir(curPath);
	}

	return true;
}

// renames file srcFilename to destFilename, returns true on success
bool Rename(const Path &srcFilename, const Path &destFilename) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "Rename %s -> %s", srcFilename.c_str(), destFilename.c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
	}

	if (srcFilename.Type() != destFilename.Type()) {
		// Impossible. You're gonna need to make a copy, and delete the original. Not the responsibility
		// of Rename.
		return false;
	}

	// We've already asserted that they're the same Type, so only need to check either src or dest.
	switch (srcFilename.Type()) {
	case PathType::NATIVE:
		// OK, proceed with the regular code.
		break;
	default:
		return false;
	}

	INFO_LOG(Log::IO, "Rename: %s --> %s", srcFilename.c_str(), destFilename.c_str());

	if (rename(srcFilename.c_str(), destFilename.c_str()) == 0)
		return true;

	ERROR_LOG(Log::IO, "Rename: failed %s --> %s: %s",
			  srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
	return false;
}

// copies file srcFilename to destFilename, returns true on success
bool Copy(const Path &srcFilename, const Path &destFilename) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "Copy %s -> %s", srcFilename.c_str(), destFilename.c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
	}
	switch (srcFilename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	INFO_LOG(Log::IO, "Copy by OpenCFile: %s --> %s", srcFilename.c_str(), destFilename.c_str());

	// buffer size
#define BSIZE 16384

	char buffer[BSIZE];

	// Open input file
	FILE *input = OpenCFile(srcFilename, "rb");
	if (!input) {
		ERROR_LOG(Log::IO, "Copy: input failed %s --> %s: %s",
				srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}

	// open output file
	FILE *output = OpenCFile(destFilename, "wb");
	if (!output) {
		fclose(input);
		ERROR_LOG(Log::IO, "Copy: output failed %s --> %s: %s",
				srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}

	int bytesWritten = 0;

	// copy loop
	while (!feof(input)) {
		// read input
		int rnum = fread(buffer, sizeof(char), BSIZE, input);
		if (rnum != BSIZE) {
			if (ferror(input) != 0) {
				ERROR_LOG(Log::IO,
						"Copy: failed reading from source, %s --> %s: %s",
						srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
				fclose(input);
				fclose(output);
				return false;
			}
		}

		// write output
		int wnum = fwrite(buffer, sizeof(char), rnum, output);
		if (wnum != rnum) {
			ERROR_LOG(Log::IO,
					"Copy: failed writing to output, %s --> %s: %s",
					srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
			fclose(input);
			fclose(output);
			return false;
		}

		bytesWritten += wnum;
	}

	if (bytesWritten == 0) {
		WARN_LOG(Log::IO, "Copy: No bytes written (must mean that input was empty)");
	}

	// close flushes
	fclose(input);
	fclose(output);
	return true;
}

// Will overwrite the target.
bool Move(const Path &srcFilename, const Path &destFilename) {
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
		INFO_LOG(Log::IO, "Move %s -> %s", srcFilename.c_str(), destFilename.c_str());
	}
	bool fast = MoveIfFast(srcFilename, destFilename);
	if (fast) {
		return true;
	}
	// OK, that failed, so fall back on a copy.
	if (Copy(srcFilename, destFilename)) {
		return Delete(srcFilename);
	} else {
		return false;
	}
}

bool MoveIfFast(const Path &srcFilename, const Path &destFilename) {
	if (srcFilename.Type() != destFilename.Type()) {
		// No way it's gonna work.
		return false;
	}

	// Only need to check one type here, due to the above check.

	// Try a traditional rename operation.
	return Rename(srcFilename, destFilename);
}

// Returns the size of file (64bit)
// TODO: Add a way to return an error.
uint64_t GetFileSize(const Path &filename) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "GetFileSize %s", filename.c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
	}
	switch (filename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	struct stat64 file_info;
	int result = stat64(filename.c_str(), &file_info);
	if (result != 0) {
		WARN_LOG(Log::IO, "GetSize: failed %s: No such file", filename.ToVisualString().c_str());
		return 0;
	}
	if (S_ISDIR(file_info.st_mode)) {
		WARN_LOG(Log::IO, "GetSize: failed %s: is a directory", filename.ToVisualString().c_str());
		return 0;
	}
	DEBUG_LOG(Log::IO, "GetSize: %s: %lld", filename.ToVisualString().c_str(), (long long)file_info.st_size);
	return file_info.st_size;
}

uint64_t GetFileSize(FILE *f) {
	uint64_t pos = Ftell(f);
	if (Fseek(f, 0, SEEK_END) != 0) {
		return 0;
	}
	uint64_t size = Ftell(f);
	// Reset the seek position to where it was when we started.
	if (size != pos && Fseek(f, pos, SEEK_SET) != 0) {
		// Should error here.
		return 0;
	}
	if (size == -1)
		return 0;
	return size;
}

// creates an empty file filename, returns true on success
bool CreateEmptyFile(const Path &filename) {
	INFO_LOG(Log::IO, "CreateEmptyFile: %s", filename.c_str());
	FILE *pFile = OpenCFile(filename, "wb");
	if (!pFile) {
		ERROR_LOG(Log::IO, "CreateEmptyFile: failed to create '%s': %s", filename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}
	fclose(pFile);
	return true;
}

// Deletes an empty directory, returns true on success
// WARNING: On Android with content URIs, it will delete recursively!
bool DeleteDir(const Path &path) {
	if (LOG_IO) {
		INFO_LOG(Log::IO, "DeleteDir %s", path.c_str());
	}
	if (SIMULATE_SLOW_IO) {
		sleep_ms(100, "slow-io-sim");
	}
	switch (path.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}
	INFO_LOG(Log::IO, "DeleteDir: directory %s", path.c_str());

	// check if a directory
	if (!File::IsDirectory(path)) {
		ERROR_LOG(Log::IO, "DeleteDir: Not a directory %s", path.c_str());
		return false;
	}

	if (rmdir(path.c_str()) == 0)
		return true;
	ERROR_LOG(Log::IO, "DeleteDir: %s: %s", path.c_str(), GetLastErrorMsg().c_str());

	return false;
}

// Deletes the given directory and anything under it. Returns true on success.
bool DeleteDirRecursively(const Path &path) {
	switch (path.Type()) {
	case PathType::NATIVE:
		break;
	default:
		ERROR_LOG(Log::IO, "DeleteDirRecursively: Path type not supported");
		return false;
	}

	std::vector<FileInfo> files;
	GetFilesInDir(path, &files, nullptr, GETFILES_GETHIDDEN);
	for (const auto &file : files) {
		if (file.isDirectory) {
			DeleteDirRecursively(file.fullName);
		} else {
			Delete(file.fullName);
		}
	}
	return DeleteDir(path);
}

bool OpenFileInEditor(const Path &fileName) {
	switch (fileName.Type()) {
	case PathType::NATIVE:
		break;  // OK
	default:
		ERROR_LOG(Log::IO, "OpenFileInEditor(%s): Path type not supported", fileName.c_str());
		return false;
	}

	std::string iniFile;
	iniFile = "xdg-open ";
	iniFile.append(fileName.ToString());
	NOTICE_LOG(Log::Boot, "Launching %s", iniFile.c_str());
	int retval = system(iniFile.c_str());
	if (retval != 0) {
		ERROR_LOG(Log::IO, "Failed to launch ini file");
	}
	return true;
}

const Path GetCurDirectory() {
	char temp[4096]{};
	return Path(getcwd(temp, 4096));
}

const Path &GetExeDirectory() {
	static Path ExePath;

	if (ExePath.empty()) {
		char program_path[4096]{};
		uint32_t program_path_size = sizeof(program_path) - 1;

		if (readlink("/proc/self/exe", program_path, program_path_size) > 0)
		{
			program_path[sizeof(program_path) - 1] = '\0';
			char *last_slash = strrchr(program_path, '/');
			if (last_slash != nullptr)
				*last_slash = '\0';
			ExePath = Path(program_path);
		}
	}

	return ExePath;
}

int Fseek(FILE *file, int64_t offset, int whence) {
#if (defined(__ANDROID__) && __ANDROID_API__ < 24) || (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64)
	return fseek(file, offset, whence);
#else
	return fseeko(file, offset, whence);
#endif
}

int64_t Fseektell(FILE *file, int64_t offset, int whence) {
#if (defined(__ANDROID__) && __ANDROID_API__ < 24) || (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64)
	return fseek(file, offset, whence) != 0 ? -1 : ftell(file);
#else
	return fseeko(file, offset, whence) != 0 ? -1 : ftello(file);
#endif
}

int64_t Ftell(FILE *file) {
#if defined(__ANDROID__) || (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64)
	return ftell(file);
#else
	return ftello(file);
#endif
}


IOFile::IOFile(const Path &filename, const char openmode[]) {
	Open(filename, openmode);
}

IOFile::~IOFile() {
	Close();
}

bool IOFile::Open(const Path& filename, const char openmode[])
{
	Close();
	m_file = File::OpenCFile(filename, openmode);
	m_good = IsOpen();
	return m_good;
}

bool IOFile::Close()
{
	if (!IsOpen() || 0 != fclose(m_file))
		m_good = false;

	m_file = NULL;
	return m_good;
}

FILE* IOFile::ReleaseHandle()
{
	FILE* const ret = m_file;
	m_file = NULL;
	return ret;
}

void IOFile::SetHandle(FILE* file)
{
	Close();
	Clear();
	m_file = file;
}

uint64_t IOFile::GetSize()
{
	if (IsOpen())
		return File::GetFileSize(m_file);
	else
		return 0;
}

bool IOFile::Seek(int64_t off, int origin)
{
	if (!IsOpen() || 0 != Fseek(m_file, off, origin))
		m_good = false;

	return m_good;
}

uint64_t IOFile::Tell()
{
	if (IsOpen())
		return Ftell(m_file);
	else
		return -1;
}

bool IOFile::Flush()
{
	if (!IsOpen() || 0 != fflush(m_file))
		m_good = false;

	return m_good;
}

bool IOFile::Resize(uint64_t size)
{
	if (!IsOpen() || 0 !=
		// TODO: handle 64bit and growing
		ftruncate(fileno(m_file), size)
	)
		m_good = false;

	return m_good;
}

bool ReadFileToStringOptions(bool textFile, bool allowShort, const Path &filename, std::string *str) {
	FILE *f = File::OpenCFile(filename, textFile ? "r" : "rb");
	if (!f)
		return false;
	// Warning: some files, like in /sys/, may return a fixed size like 4096.
	size_t len = (size_t)File::GetFileSize(f);
	bool success;
	if (len == 0) {
		// Just read until we can't read anymore.
		size_t totalSize = 1024;
		size_t totalRead = 0;
		do {
			totalSize *= 2;
			str->resize(totalSize);
			totalRead += fread(&(*str)[totalRead], 1, totalSize - totalRead, f);
		} while (totalRead == totalSize);
		str->resize(totalRead);
		success = true;
	} else {
		str->resize(len);
		size_t totalRead = fread(&(*str)[0], 1, len, f);
		str->resize(totalRead);
		// Allow less, because some system files will report incorrect lengths.
		// Also, when reading text with CRLF, the read length may be shorter.
		if (textFile) {
			// totalRead doesn't take \r into account since they might be skipped in this mode.
			// So let's just ask how far the cursor got.
			totalRead = Ftell(f);
		}
		success = allowShort ? (totalRead <= len) : (totalRead == len);
	}
	fclose(f);
	return success;
}

uint8_t *ReadLocalFile(const Path &filename, size_t *size) {
	FILE *file = File::OpenCFile(filename, "rb");
	if (!file) {
		*size = 0;
		return nullptr;
	}
	int64_t f_size = Fseektell(file, 0, SEEK_END);
	if (f_size < 0) {
		*size = 0;
		fclose(file);
		return nullptr;
	}
	Fseek(file, 0, SEEK_SET);
	// NOTE: If you find ~10 memory leaks from here, with very varying sizes, it might be the VFPU LUTs.
	uint8_t *contents = new uint8_t[f_size + 1];
	if (fread(contents, 1, f_size, file) != f_size) {
		delete[] contents;
		contents = nullptr;
		*size = 0;
	} else {
		contents[f_size] = 0;
		*size = f_size;
	}
	fclose(file);
	return contents;
}

bool WriteStringToFile(bool text_file, std::string_view str, const Path &filename) {
	FILE *f = File::OpenCFile(filename, text_file ? "w" : "wb");
	if (!f)
		return false;
	size_t len = str.size();
	if (len != fwrite(str.data(), 1, str.size(), f))
	{
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

bool WriteDataToFile(bool text_file, const void* data, size_t size, const Path &filename) {
	FILE *f = File::OpenCFile(filename, text_file ? "w" : "wb");
	if (!f)
		return false;
	if (size != fwrite(data, 1, size, f))
	{
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

void ChangeMTime(const Path &path, time_t mtime) {

	utimbuf buf{};
	buf.actime = mtime;
	buf.modtime = mtime;
	utime(path.c_str(), &buf);
}

bool IsProbablyInDownloadsFolder(const Path &filename) {
	INFO_LOG(Log::IO, "IsProbablyInDownloadsFolder: Looking at %s (%s)...", filename.c_str(), filename.ToVisualString().c_str());
	switch (filename.Type()) {
	default:
		break;
	}

	return filename.FilePathContainsNoCase("download");
}

}  // namespace File
