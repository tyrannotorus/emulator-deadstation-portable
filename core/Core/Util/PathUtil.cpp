#include <string_view>

#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/System/System.h"
#include "Common/Log.h"
#include "Core/Util/PathUtil.h"
#include "Core/Config.h"

Path FindConfigFile(const Path &searchPath, std::string_view baseFilename, bool *exists) {
	// Don't search for an absolute path.
	if (baseFilename.size() > 1 && baseFilename[0] == '/') {
		Path path(baseFilename);
		*exists = File::Exists(path);
		return path;
	}

	Path filename = searchPath / baseFilename;
	if (File::Exists(filename)) {
		*exists = true;
		return filename;
	}
	*exists = false;
	// Make sure at least the directory it's supposed to be in exists.
	Path parent = filename.NavigateUp();

	// We try to create the path and ignore if it fails (already exists).
	if (parent != GetSysDirectory(DIRECTORY_SYSTEM)) {
		File::CreateFullPath(parent);
	}
	return filename;
}

Path GetSysDirectory(PSPDirectories directoryType) {
	const Path &memStickDirectory = g_Config.memStickDirectory;
	Path pspDirectory;
	if (!strcasecmp(memStickDirectory.GetFilename().c_str(), "PSP")) {
		// Let's strip this off, to easily allow choosing a root directory named "PSP" on Android.
		pspDirectory = memStickDirectory;
	} else {
		pspDirectory = memStickDirectory / "PSP";
	}

	switch (directoryType) {
	case DIRECTORY_PSP:
		return pspDirectory;
	case DIRECTORY_CHEATS:
		return pspDirectory / "Cheats";
	case DIRECTORY_GAME:
		return pspDirectory / "GAME";
	case DIRECTORY_SAVEDATA:
		return pspDirectory / "SAVEDATA";
	case DIRECTORY_SCREENSHOT:
		return pspDirectory / "SCREENSHOT";
	case DIRECTORY_SYSTEM:
		return pspDirectory / "SYSTEM";
	case DIRECTORY_PAUTH:
		return memStickDirectory / "PAUTH";  // This one's at the root...
	case DIRECTORY_EXDATA:
		return memStickDirectory / "EXDATA";  // This one's traditionally at the root...
	case DIRECTORY_DUMP:
		return pspDirectory / "SYSTEM/DUMP";
	case DIRECTORY_SAVESTATE:
		return pspDirectory / "PPSSPP_STATE";
	case DIRECTORY_CACHE:
		return pspDirectory / "SYSTEM/CACHE";
	case DIRECTORY_TEXTURES:
		return pspDirectory / "TEXTURES";
	case DIRECTORY_PLUGINS:
		return pspDirectory / "PLUGINS";
	case DIRECTORY_APP_CACHE:
		if (!g_Config.appCacheDirectory.empty()) {
			return g_Config.appCacheDirectory;
		}
		return pspDirectory / "SYSTEM/CACHE";
	case DIRECTORY_VIDEO:
		return pspDirectory / "VIDEO";
	case DIRECTORY_AUDIO:
		return pspDirectory / "AUDIO";
	case DIRECTORY_CUSTOM_SHADERS:
		return pspDirectory / "shaders";
	case DIRECTORY_CUSTOM_THEMES:
		return pspDirectory / "themes";

	case DIRECTORY_MEMSTICK_ROOT:
		return g_Config.memStickDirectory;
		// Just return the memory stick root if we run into some sort of problem.
	default:
		ERROR_LOG(Log::FileSystem, "Unknown directory type.");
		return g_Config.memStickDirectory;
	}
}


std::string GetFriendlyPath(Path path, const Path &rootMatch, std::string_view rootDisplay) {
	return path.ToVisualString();
}
