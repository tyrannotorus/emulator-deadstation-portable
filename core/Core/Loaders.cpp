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

#include <algorithm>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/StringUtils.h"
#include "Common/Data/Text/I18n.h"
#include "Core/FileLoaders/LocalFileLoader.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/PSPLoaders.h"
#include "Core/MemMap.h"
#include "Core/Loaders.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/ELF/PBPReader.h"
#include "Core/ELF/ParamSFO.h"

// TODO: This is just part of a VolDescriptor, see ISOFileSystem.cpp.
struct PVD {
	u8 type;
	char identifier[5];
	char version;
	char pad0;
	char systemId[32];  // PSP GAME normally
	char volumeId[32];  // In PSP games, this sometimes has the name of the game but far from always.
};

FileLoader *ConstructFileLoader(const Path &filename) {
	return new LocalFileLoader(filename);
}

// TODO : improve, look in the file more
// Does not take ownership.
IdentifiedFileType Identify_File(FileLoader *fileLoader, std::string *errorString) {
	errorString->clear();
	if (fileLoader == nullptr) {
		*errorString = "Invalid fileLoader";
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}
	if (fileLoader->GetPath().size() == 0) {
		*errorString = "Invalid filename " + fileLoader->GetPath().ToString();
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}

	if (!fileLoader->Exists()) {
		*errorString = "IdentifyFile: File doesn't exist: " + fileLoader->GetPath().ToString();
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}

	std::string extension = fileLoader->GetFileExtension();

	// First, check if it's a directory with an EBOOT.PBP in it.
	if (fileLoader->IsDirectory()) {
		Path filename = fileLoader->GetPath();
		if (filename.size() > 4) {
			// Check for existence of EBOOT.PBP, as required for "Directory games".
			if (File::Exists(filename / "EBOOT.PBP")) {
				return IdentifiedFileType::PSP_PBP_DIRECTORY;
			}

			// check if it's a disc directory
			if (File::Exists(filename / "PSP_GAME")) {
				return IdentifiedFileType::PSP_DISC_DIRECTORY;
			}

			// Not that, okay, let's guess it's a savedata directory if it has a param.sfo...
			if (File::Exists(filename / "PARAM.SFO")) {
				return IdentifiedFileType::PSP_SAVEDATA_DIRECTORY;
			}
		}

		return IdentifiedFileType::NORMAL_DIRECTORY;
	}

	bool isDiscImage = false;
	if (extension == ".iso" || extension == ".cso" || extension == ".chd") {
		isDiscImage = true;
	} else if (extension == ".ppst") {
		return IdentifiedFileType::PPSSPP_SAVESTATE;
	} else if (extension == ".ppdmp") {
		char data[8]{};
		fileLoader->ReadAt(0, 8, data);
		if (memcmp(data, "PPSSPPGE", 8) == 0) {
			return IdentifiedFileType::PPSSPP_GE_DUMP;
		}
	}

	// OK, quick methods of identification for common types failed. Moving on to more expensive methods,
	// starting by reading the first few bytes.
	// This can be necessary for weird Android content storage path types, see issue #17462
	if (isDiscImage || fileLoader->FileSize() >= 0x8800) {
		// Do the quick check for PSP ISOs here.
		std::string bdError;
		std::shared_ptr<BlockDevice> bd(ConstructBlockDevice(fileLoader, &bdError));
		if (bd) {
			u8 block16[2048]{};
			bd->ReadBlock(16, (u8 *)block16);
			PVD pvd;
			memcpy(&pvd, block16, sizeof(PVD));
			if (!memcmp(pvd.identifier, "CD001", 5)) {
				// It's a valid DVD-style ISO file. Let's see which type.
				if (!memcmp(pvd.systemId, "PSP GAME", 8) || !memcmp(pvd.systemId, "\"PSP GAME\"", 10)) {
					// Yes, a known proper PSP game, let's get it going.
					return bd->IsDisc() ? IdentifiedFileType::PSP_ISO : IdentifiedFileType::PSP_ISO_NP;
				} else if (!memcmp(pvd.systemId, "UMD VIDEO", 9) || !memcmp(pvd.systemId, "UMD AUDIO", 9)) {
					// This is rare so being slightly slow here shouldn't be a problem. Let's go check for the presence of
					// actual game data.
					SequentialHandleAllocator hAlloc;
					ISOFileSystem umd(&hAlloc, bd);
					if (umd.GetFileInfo("/PSP_GAME").exists) {
						INFO_LOG(Log::Loader, "Found an UMD VIDEO disc with game data. Treating as game.");
						*errorString = "UMD Video with PSP GAME data";
						return IdentifiedFileType::PSP_ISO;
					}

					// UMD AUDIO exists technically, but in reality, not really? Let's map it to VIDEO since we support neither.
					return IdentifiedFileType::PSP_UMD_VIDEO_ISO;
				} else if (!memcmp(pvd.systemId, "PS3", 3)) {
					*errorString = "PS3 ISO";
					return IdentifiedFileType::PS3_ISO;
				} else if (!memcmp(pvd.systemId, "PLAYSTATION", 11)) {
					// Just do a size heuristic here to differentiate. There are better ways but slower.
					if (bd->GetUncompressedSize() > 800LL * 1024LL * 1024LL) {
						*errorString = "PS2 ISO";
						return IdentifiedFileType::PS2_ISO;
					}
					*errorString = "PSX ISO?";
					return IdentifiedFileType::PSX_ISO;
				} else {
					// Let's go check for PSP game data.
					SequentialHandleAllocator hAlloc;
					ISOFileSystem umd(&hAlloc, bd);
					if (umd.GetFileInfo("/PSP_GAME").exists) {
						INFO_LOG(Log::Loader, "PSP ISO with unknown system ID: %.32s: %s", pvd.systemId, fileLoader->GetPath().c_str());
						return bd->IsDisc() ? IdentifiedFileType::PSP_ISO : IdentifiedFileType::PSP_ISO_NP;
					}

					INFO_LOG(Log::Loader, "Unknown ISO with unknown system ID: %.32s: %s", pvd.systemId, fileLoader->GetPath().c_str());
					*errorString = StringFromFormat("ISO with unknown system ID: %.32s", pvd.systemId);
					return IdentifiedFileType::UNKNOWN_ISO;
				}
			}

			// Do extra check for PSX ISO
			// may be a psx iso, they have 2352 byte sectors. You never know what some people try to open
			if ((fileLoader->FileSize() % 2352) == 0) {
				unsigned char sync[12];
				fileLoader->ReadAt(0, 12, sync);

				// each sector in a mode2 image starts with these 12 bytes
				if (memcmp(sync, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", 12) == 0) {
					*errorString = "ISO is a CD - likely PSX";  // Mode 2 CDs are used for PSX games
					return IdentifiedFileType::PSX_ISO;
				}
			}
		}

		if (isDiscImage) {
			if (!bdError.empty()) {
				*errorString = bdError;
			} else {
				auto sy = GetI18NCategory(I18NCat::SYSTEM);
				*errorString = sy->T("Not a PSP game");
			}
			return IdentifiedFileType::UNKNOWN_ISO;
		}
	}

	u32 id;

	size_t readSize = fileLoader->ReadAt(0, 4, 1, &id);
	if (readSize != 1) {
		*errorString = "Failed to read identification bytes";
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}

	u32_le psar_offset = 0, psar_id = 0;
	if (!memcmp(&id, "PK\x03\x04", 4) || !memcmp(&id, "PK\x05\x06", 4) || !memcmp(&id, "PK\x07\x08", 4)) {
		return IdentifiedFileType::ARCHIVE_ZIP;
	} else if (!memcmp(&id, "\x00PBP", 4)) {
		fileLoader->ReadAt(0x24, 4, 1, &psar_offset);
		fileLoader->ReadAt(psar_offset, 4, 1, &psar_id);
		// Fall through to the below if chain.
	} else if (!memcmp(&id, "Rar!", 4)) {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (!memcmp(&id, "\x37\x7A\xBC\xAF", 4)) {
		return IdentifiedFileType::ARCHIVE_7Z;
	}

	if (id == 'FLE\x7F') {
		Path filename = fileLoader->GetPath();
		// There are a few elfs misnamed as pbp (like Trig Wars), accept that. Also accept extension-less paths.
		if (extension == ".plf" || strstr(filename.GetFilename().c_str(), "BOOT.BIN") ||
			extension == ".elf" || extension == ".prx" || extension == ".pbp" || extension == "") {
			return IdentifiedFileType::PSP_ELF;
		}
		return IdentifiedFileType::UNKNOWN_ELF;
	} else if (id == 'PBP\x00') {
		// Do this PS1 eboot check FIRST before checking other eboot types.
		// It seems like some are malformed and slip through the PSAR check below.
		PBPReader pbp(fileLoader);
		if (pbp.IsValid() && !pbp.IsELF()) {
			std::vector<u8> sfoData;
			if (pbp.GetSubFile(PBP_PARAM_SFO, &sfoData)) {
				ParamSFOData paramSFO;
				paramSFO.ReadSFO(sfoData);
				// PS1 Eboots are supposed to use "ME" as their PARAM SFO category.
				// If they don't, and they're still malformed (e.g. PSISOIMG0000 isn't found), there's nothing we can do.
				if (paramSFO.GetValueString("CATEGORY") == "ME") {
					return IdentifiedFileType::PSP_PS1_PBP;
				}
			}
		}

		if (psar_id == 'MUPN') {
			return IdentifiedFileType::PSP_ISO_NP;
		}
		// PS1 PSAR begins with "PSISOIMG0000"
		if (psar_id == 'SISP') {
			return IdentifiedFileType::PSP_PS1_PBP;
		}

		// Let's check if we got pointed to a PBP within such a directory.
		// If so we just move up and return the directory itself as the game.
		// If loading from memstick...
		if (fileLoader->GetPath().FilePathContainsNoCase("PSP/GAME/")) {
			return IdentifiedFileType::PSP_PBP_DIRECTORY;
		}
		return IdentifiedFileType::PSP_PBP;
	} else if (extension == ".pbp") {
		ERROR_LOG(Log::Loader, "A PBP with the wrong magic number?");
		return IdentifiedFileType::PSP_PBP;
	} else if (extension == ".bin") {
		return IdentifiedFileType::UNKNOWN_BIN;
	} else if (extension == ".zip") {
		return IdentifiedFileType::ARCHIVE_ZIP;
	} else if (extension == ".rar") {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (extension == ".r00") {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (extension == ".r01") {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (extension == ".7z") {
		return IdentifiedFileType::ARCHIVE_7Z;
	}
	return IdentifiedFileType::UNKNOWN;
}

FileLoader *ResolveFileLoaderTarget(FileLoader *fileLoader, IdentifiedFileType *fileType, std::string *errorString) {
	*fileType = Identify_File(fileLoader, errorString);
	if (*fileType == IdentifiedFileType::PSP_PBP_DIRECTORY) {
		const Path ebootFilename = ResolvePBPFile(fileLoader->GetPath());
		if (ebootFilename != fileLoader->GetPath()) {
			// Switch fileLoader to the actual EBOOT.
			delete fileLoader;
			fileLoader = ConstructFileLoader(ebootFilename);
			// Re-identify the file.
			*fileType = Identify_File(fileLoader, errorString);
		}
	}
	return fileLoader;
}

Path ResolvePBPDirectory(const Path &filename) {
	if (filename.GetFilename() == "EBOOT.PBP") {
		return filename.NavigateUp();
	} else {
		return filename;
	}
}

Path ResolvePBPFile(const Path &filename) {
	if (filename.GetFilename() != "EBOOT.PBP") {
		return filename / "EBOOT.PBP";
	} else {
		return filename;
	}
}

bool UmdReplace(const Path &filepath, FileLoader **fileLoader, std::string &error) {
	IFileSystem *currentUMD = pspFileSystem.GetSystem("disc0:");

	if (!currentUMD) {
		error = "has no disc";
		return false;
	}

	FileLoader *loadedFile = ConstructFileLoader(filepath);

	if (!loadedFile || !loadedFile->Exists()) {
		error = loadedFile ? (GetFriendlyPath(loadedFile->GetPath()) + " doesn't exist") : "no loaded file";
		delete loadedFile;
		return false;
	}
	UpdateLoadedFile(loadedFile);

	std::string errorString;
	IdentifiedFileType fileType;
	loadedFile = ResolveFileLoaderTarget(loadedFile, &fileType, &errorString);

	*fileLoader = loadedFile;

	switch (fileType) {
	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:
		if (!MountGameISO(loadedFile, &error)) {
			error = "mounting the replaced ISO failed: " + error;
			return false;
		}
		break;
	default:
		error = "Unsupported file type: " + std::string(IdentifiedFileTypeToString(fileType)) + " " + errorString;
		return false;
		break;
	}
	return true;
}

const char *IdentifiedFileTypeToString(IdentifiedFileType type) {
	switch (type) {
	case IdentifiedFileType::ERROR_IDENTIFYING: return "ERROR_IDENTIFYING";
	case IdentifiedFileType::PSP_PBP_DIRECTORY: return "PSP_PBP_DIRECTORY";
	case IdentifiedFileType::PSP_PBP: return "PSP_PBP";
	case IdentifiedFileType::PSP_ELF: return "PSP_ELF";
	case IdentifiedFileType::PSP_ISO: return "PSP_ISO";
	case IdentifiedFileType::PSP_ISO_NP: return "PSP_ISO_NP";
	case IdentifiedFileType::PSP_DISC_DIRECTORY: return "PSP_DISC_DIRECTORY";
	case IdentifiedFileType::UNKNOWN_BIN: return "UNKNOWN_BIN";
	case IdentifiedFileType::UNKNOWN_ELF: return "UNKNOWN_ELF";
	case IdentifiedFileType::UNKNOWN_ISO: return "UNKNOWN_ISO";
	case IdentifiedFileType::ARCHIVE_RAR: return "ARCHIVE_RAR";
	case IdentifiedFileType::ARCHIVE_ZIP: return "ARCHIVE_ZIP";
	case IdentifiedFileType::ARCHIVE_7Z: return "ARCHIVE_7Z";
	case IdentifiedFileType::PSP_PS1_PBP: return "PSP_PS1_PBP";
	case IdentifiedFileType::PSX_ISO: return "PSX_ISO";
	case IdentifiedFileType::PS2_ISO: return "PS2_ISO";
	case IdentifiedFileType::PS3_ISO: return "PS3_ISO";
	case IdentifiedFileType::PSP_UMD_VIDEO_ISO: return "UMD_VIDEO";
	case IdentifiedFileType::NORMAL_DIRECTORY: return "NORMAL_DIRECTORY";
	case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY: return "PSP_SAVEDATA_DIRECTORY";
	case IdentifiedFileType::PPSSPP_SAVESTATE: return "PPSSPP_SAVESTATE";
	case IdentifiedFileType::PPSSPP_GE_DUMP: return "PPSSPP_GE_DUMP";
	case IdentifiedFileType::UNKNOWN: return "UNKNOWN";
	default: return "INVALID_TYPE";
	}
}
