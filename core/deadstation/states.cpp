#include "jitcache.h"
#include "states.h"
#include "log.h"
#include "dead_ui.h"

#include "Common/File/Path.h"
#include "Core/SaveState.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace deadstation::states
{
namespace
{

std::string game = "game";

// the picture's file: its width and height, its RGB24 rows
struct PictureHeader
{
	uint32_t width;
	uint32_t height;
};

}	// namespace

void setGame(const std::string& romPath)
{
	std::string name = romPath.substr(romPath.find_last_of('/') + 1);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot > 0)
		name.resize(dot);
	game = name;
}

std::string slotPath(int slot)
{
	return recomp::home() + "/" + game + (slot == 0 ? "" : "_" + std::to_string(slot)) + ".state";
}

bool save(const std::string& path, const std::vector<uint8_t>& rgb, int width, int height)
{
	std::string error;
	if (!SaveState::Save(Path(path + ".tmp"), &error))
	{
		DS_LOG("state: %s", error.c_str());
		unlink((path + ".tmp").c_str());
		return false;
	}
	if (std::rename((path + ".tmp").c_str(), path.c_str()) != 0)
		return false;
	const std::string picturePath = path + ".pic";
	if ((size_t)width * height * 3 == rgb.size() && width > 0)
	{
		std::ofstream out(picturePath, std::ios::binary | std::ios::trunc);
		const PictureHeader h { (uint32_t)width, (uint32_t)height };
		out.write((const char *)&h, sizeof(h));
		out.write((const char *)rgb.data(), (std::streamsize)rgb.size());
	}
	else
		unlink(picturePath.c_str());
	sync();
	return true;
}

bool load(const std::string& path)
{
	std::string error;
	if (!SaveState::Load(Path(path), &error))
	{
		DS_LOG("state: %s: %s", path.c_str(), error.c_str());
		return false;
	}
	jitcache::afterLoad();
	return true;
}

bool exists(int slot)
{
	return access(slotPath(slot).c_str(), R_OK) == 0;
}

bool picture(int slot, std::vector<uint8_t>& rgb, int& width, int& height)
{
	std::ifstream in(slotPath(slot) + ".pic", std::ios::binary);
	PictureHeader h;
	if (!in.read((char *)&h, sizeof(h)) || h.width == 0 || h.height == 0 || h.width > 1024 || h.height > 1024)
		return false;
	rgb.resize((size_t)h.width * h.height * 3);
	if (!in.read((char *)rgb.data(), (std::streamsize)rgb.size()))
		return false;
	width = (int)h.width;
	height = (int)h.height;
	return true;
}

}
