#include "ppsspp_config.h"

#include <algorithm>  // for std::search
#include <cctype>
#include <cstring>

#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/Log.h"
#include "Common/Data/Encoding/Utf8.h"



#if HOST_IS_CASE_SENSITIVE
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

Path::Path(std::string_view str) {
	Init(str);
}


void Path::Init(std::string_view str) {
	if (str.empty()) {
		type_ = PathType::UNDEFINED;
		path_.clear();
	} else {
		type_ = PathType::NATIVE;
		path_ = str;
	}


	// Don't pop_back if it's just "/".
	if (type_ == PathType::NATIVE && path_.size() > 1 && path_.back() == '/') {
		path_.pop_back();
	}
}

// We always use forward slashes internally, we convert to backslash only when
// converted to a wstring.
Path Path::operator /(std::string_view subdir) const {

	// Direct string manipulation.

	if (subdir.empty()) {
		return Path(path_);
	}
	std::string fullPath = path_;
	if (subdir.front() != '/' && (fullPath.empty() || fullPath.back() != '/')) {
		fullPath += "/";
	}
	fullPath += subdir;
	// Prevent adding extra slashes.
	if (fullPath.back() == '/') {
		fullPath.pop_back();
	}
	return Path(fullPath);
}

void Path::operator /=(std::string_view subdir) {
	*this = *this / subdir;
}

Path Path::WithExtraExtension(std::string_view ext) const {

	_dbg_assert_(!ext.empty() && ext[0] == '.');
	return Path(path_ + std::string(ext));
}

Path Path::WithReplacedExtension(const std::string &oldExtension, const std::string &newExtension) const {

	_dbg_assert_(!oldExtension.empty() && oldExtension[0] == '.');
	_dbg_assert_(!newExtension.empty() && newExtension[0] == '.');
	if (endsWithNoCase(path_, oldExtension)) {
		std::string newPath = path_.substr(0, path_.size() - oldExtension.size());
		return Path(newPath + newExtension);
	} else {
		return Path(*this);
	}
}

Path Path::WithReplacedExtension(const std::string &newExtension) const {

	_dbg_assert_(!newExtension.empty() && newExtension[0] == '.');
	if (path_.empty()) {
		return Path(*this);
	}
	std::string extension = GetFileExtension();
	std::string newPath = path_.substr(0, path_.size() - extension.size()) + newExtension;
	return Path(newPath);
}

std::string Path::GetFilename() const {
	size_t pos = path_.rfind('/');
	if (pos != std::string::npos) {
		return path_.substr(pos + 1);
	}
	return path_;
}

static std::string GetExtFromString(std::string_view str) {
	size_t pos = str.rfind(".");
	if (pos == std::string::npos) {
		return "";
	}
	size_t slash_pos = str.rfind("/");
	if (slash_pos != std::string::npos && slash_pos > pos) {
		// Don't want to detect "df/file" from "/as.df/file"
		return "";
	}
	std::string ext(str.substr(pos));
	for (size_t i = 0; i < ext.size(); i++) {
		ext[i] = tolower(ext[i]);
	}
	return ext;
}

std::string Path::GetFileExtension() const {
	return GetExtFromString(path_);
}

std::string Path::GetDirectory() const {

	size_t pos = path_.rfind('/');

	if (pos != std::string::npos) {
		if (pos == 0) {
			return "/";  // We're at the root.
		}
		return path_.substr(0, pos);
	} else {
		// There could be a ':', too. Unlike the slash, let's include that
		// in the returned directory.
		size_t c_pos = path_.rfind(':');
		if (c_pos != std::string::npos) {
			return path_.substr(0, c_pos + 1);
		}
	}
	// No directory components, we're a relative path.
	return path_;
}

bool Path::FilePathContainsNoCase(std::string_view needle) const {
	std::string haystack;
	{
		haystack = path_;
	}
	auto pred = [](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); };
	auto found = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), pred);
	return found != haystack.end();
}

bool Path::StartsWith(const Path &other) const {
	if (other.empty()) {
		return true;
	}
	if (type_ != other.type_) {
		// Bad
		return false;
	}
	return startsWith(path_, other.path_);
}

bool Path::StartsWithGlobalAndNotEqual(const Path &other) const {
	if (other.empty()) {
		return true;
	}
	if (type_ != other.type_) {
		// Bad
		return false;
	}
	return *this != other && StartsWith(other);
}

const std::string &Path::ToString() const {
	return path_;
}


std::string Path::ToVisualString(const char *relativeRoot) const {
	{
		return path_;
	}
}

bool Path::CanNavigateUp() const {
	if (path_ == "/" || path_.empty()) {
		return false;
	}
	return true;
}

Path Path::NavigateUp() const {
	std::string dir = GetDirectory();
	return Path(dir);
}

Path Path::GetRootVolume() const {
	if (!IsAbsolute()) {
		// Can't do anything
		return Path(path_);
	}


	return Path("/");
}

bool Path::IsAbsolute() const {

	if (path_.empty())
		return true;
	else if (path_.front() == '/')
		return true;
	else
		return false;
}

bool Path::ComputePathTo(const Path &other, std::string &path) const {
	if (other == *this) {
		path.clear();
		return true;
	}

	if (!other.StartsWith(*this)) {
		// Can't do this. Should return an error.
		return false;
	}

	if (*this == other) {
		// Equal, the path is empty.
		path.clear();
		return true;
	}

	if (path_ == "/") {
		path = other.path_.substr(1);
		return true;
	} else {
		path = other.path_.substr(path_.size() + 1);
		return true;
	}
}

static bool FixFilenameCase(const std::string &path, std::string &filename) {

	// Are we lucky?
	if (File::Exists(Path(path + filename)))
		return true;

	size_t filenameSize = filename.size();  // size in bytes, not characters
	for (size_t i = 0; i < filenameSize; i++)
	{
		filename[i] = tolower(filename[i]);
	}

	//TODO: lookup filename in cache for "path"
	struct dirent *result = NULL;

	DIR *dirp = opendir(path.c_str());
	if (!dirp)
		return false;

	bool retValue = false;

	while ((result = readdir(dirp)))
	{
		if (strlen(result->d_name) != filenameSize)
			continue;

		size_t i;
		for (i = 0; i < filenameSize; i++)
		{
			if (filename[i] != tolower(result->d_name[i]))
				break;
		}

		if (i < filenameSize)
			continue;

		filename = result->d_name;
		retValue = true;
	}

	closedir(dirp);

	return retValue;
}

bool FixPathCase(const Path &realBasePath, std::string &path, FixPathCaseBehavior behavior) {


	std::string basePath = realBasePath.ToString();

	size_t len = path.size();

	if (len == 0)
		return true;

	if (path[len - 1] == '/') {
		len--;

		if (len == 0)
			return true;
	}

	std::string fullPath;
	fullPath.reserve(basePath.size() + len + 1);
	fullPath.append(basePath);

	size_t start = 0;
	while (start < len) {
		size_t i = path.find('/', start);
		if (i == std::string::npos)
			i = len;

		if (i > start) {
			std::string component = path.substr(start, i - start);

			// Fix case and stop on nonexistant path component
			if (FixFilenameCase(fullPath, component) == false) {
				// Still counts as success if partial matches allowed or if this
				// is the last component and only the ones before it are required
				return (behavior == FPC_PARTIAL_ALLOWED || (behavior == FPC_PATH_MUST_EXIST && i >= len));
			}

			path.replace(start, i - start, component);

			fullPath.append(1, '/');
			fullPath.append(component);
		}

		start = i + 1;
	}

	return true;
}
