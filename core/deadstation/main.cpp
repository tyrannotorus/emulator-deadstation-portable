#include <sys/resource.h>
#include "jitcache.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/System.h"
#include <time.h>
#include <pthread.h>
#include <thread>
#include <mutex>
#include <unistd.h>
// Deadstation Portable: PPSSPP's emulation run on the Brick, with the shared menu (ui/). The picture goes through Vulkan
// into fb0's pages (panel.h); SDL2 reads the pad and plays the sound; nothing else of SDL runs (its video would take fb0).
//   deadstation-portable <game>        an .iso, .cso, .chd or .pbp
// Environment (launch.sh): DEADSTATION_PORTABLE_HOME (settings, saves, save states), DEADSTATION_PORTABLE_ROM (the file NextUI
// started, for the menu's title).
// Tests: DEADSTATION_PORTABLE_STATE=<file> loads that save state once the game is running; DEADSTATION_PORTABLE_PERF=1 logs a
// perf line a second; DEADSTATION_PORTABLE_RESOLUTION=1|2, DEADSTATION_PORTABLE_PICTURE=0|1|2 (settings.cpp); _PROF (profiler.h);
// _SHOT_AT a picture at a moment of the game's clock; SIGUSR1 puts the page on show into /tmp/dsp-shot.ppm.
#include "audio.h"
#include "clock.h"
#include "controls.h"
#include "dead_ui.h"
#include "log.h"
#include "menu.h"
#include "osd.h"
#include "panel.h"
#include "profiler.h"
#include "settings.h"
#include "states.h"

#include "Common/CPUDetect.h"
#include "Common/Data/Text/I18n.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/File/VFS/DirectoryReader.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/GraphicsContext.h"
#include "Common/Log/LogManager.h"
#include "Common/Net/Resolve.h"
#include "Common/System/Display.h"
#include "Common/System/OSD.h"
#include "Common/Thread/ThreadManager.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FrameTiming.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HW/Display.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "Core/Loaders.h"
#include "Core/System.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "GPU/GeThread.h"

#include <SDL.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace deadstation;


OnScreenDisplay g_OSD;

void OnScreenDisplay::Show(OSDType type, std::string_view text, float duration_s, const char *)
{
	const std::string message(text);
	DS_LOG("%s", message.c_str());
	osd::notify(message, (int)(1000.f * (duration_s > 0.f ? duration_s : 3.f)));
}

void OnScreenDisplay::Show(OSDType type, std::string_view text, std::string_view text2, float duration_s, const char *)
{
	const std::string message(text), details(text2);
	DS_LOG("%s %s", message.c_str(), details.c_str());
	osd::notify(message, (int)(1000.f * (duration_s > 0.f ? duration_s : 3.f)), details);
}

namespace
{

constexpr int PageWidth = 480;	// at 1x; its height is 4:3 of that: the display engine scales it to the panel

std::atomic<bool> quit { false };
std::atomic<bool> shotWanted { false };		// SIGUSR1 (tests)
SDL_Joystick *joystick = nullptr;

// Vulkan, drawing into the panel's pages
class BrickGraphics : public GraphicsContext
{
public:
	bool init(int resolution, std::string& error)
	{
		init_glslang();
		std::string loadError;
		if (!VulkanLoad(&loadError))
		{
			error = "Vulkan: " + loadError;
			return false;
		}
		vulkan = new VulkanContext();
		VulkanContext::CreateInfo info {};
		info.app_name = "Deadstation Portable";
		info.app_ver = 1;
		info.flags = VulkanInitFlags::DISABLE_IMPLICIT_LAYERS;
		if (vulkan->CreateInstance(info) != VK_SUCCESS)
		{
			error = vulkan->InitError();
			return false;
		}
		if (vulkan->CreateDevice(vulkan->GetBestPhysicalDevice()) != VK_SUCCESS)
		{
			error = vulkan->InitError();
			return false;
		}
		if (!panel.init(vulkan->GetDevice(), error, PageWidth * resolution))
			return false;
		vulkan->InitScreen(&panel);
		draw = Draw::T3DCreateVulkanContext(vulkan, true);
		if (!draw->CreatePresets())
		{
			error = "Vulkan: no presets";
			return false;
		}
		draw->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan->GetBackbufferWidth(), vulkan->GetBackbufferHeight());
		setSizes();
		return true;
	}

	// the screen's mode for this resolution, with everything sized for it
	bool remake(int resolution, std::string& error)
	{
		panel.drain();
		draw->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan->GetBackbufferWidth(), vulkan->GetBackbufferHeight());
		panel.term();
		if (!panel.init(vulkan->GetDevice(), error, PageWidth * resolution))
			return false;
		draw->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan->GetBackbufferWidth(), vulkan->GetBackbufferHeight());
		setSizes();
		return true;
	}

	void Shutdown() override
	{
		if (draw != nullptr)
			draw->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan->GetBackbufferWidth(), vulkan->GetBackbufferHeight());
		delete draw;
		draw = nullptr;
		if (vulkan != nullptr)
		{
			vulkan->WaitUntilQueueIdle();
			panel.term();
			vulkan->DestroyDevice();
			vulkan->DestroyInstance();
			delete vulkan;
		}
		vulkan = nullptr;
		finalize_glslang();
	}

	void Resize() override {}
	void *GetAPIContext() override { return vulkan; }
	Draw::DrawContext *GetDrawContext() override { return draw; }

	Panel panel;

private:
	void setSizes()
	{
		const int w = (int)panel.Width(), h = (int)panel.Height();
		g_display.Recalculate(w, h, 1.f, 1.f, 1.f);
		PSP_CoreParameter().pixelWidth = w;
		PSP_CoreParameter().pixelHeight = h;
	}

	VulkanContext *vulkan = nullptr;
	Draw::DrawContext *draw = nullptr;
};
BrickGraphics graphics;

// the disc's id as the PSP has it (UMD_DATA.BIN at the root of the disc, ULUS-10000|...): what a recomp module names
std::string pspDiscId(const std::string& path)
{
	std::string error;
	std::unique_ptr<FileLoader> loader(ConstructFileLoader(Path(path)));
	std::unique_ptr<BlockDevice> device(ConstructBlockDevice(loader.get(), &error));
	if (device == nullptr)
		return "";
	std::vector<u8> sector(2048);
	if (!device->ReadBlock(16, sector.data()) || memcmp(sector.data() + 1, "CD001", 5) != 0)
		return "";
	const u8 *rootEntry = sector.data() + 156;
	const u32 rootSector = rootEntry[2] | rootEntry[3] << 8 | rootEntry[4] << 16 | (u32)rootEntry[5] << 24;
	std::vector<u8> root(2048);
	if (!device->ReadBlock(rootSector, root.data()))
		return "";
	for (size_t at = 0; at + 33 < root.size() && root[at] != 0; at += root[at])
	{
		const u8 nameLength = root[at + 32];
		if (at + 33 + nameLength > root.size() || nameLength < 12 || memcmp(&root[at + 33], "UMD_DATA.BIN", 12) != 0)
			continue;
		const u32 fileSector = root[at + 2] | root[at + 3] << 8 | root[at + 4] << 16 | (u32)root[at + 5] << 24;
		std::vector<u8> file(2048);
		if (!device->ReadBlock(fileSector, file.data()))
			return "";
		std::string id;
		for (size_t i = 0; i < 16 && file[i] != '|' && file[i] != 0; i++)
			if (file[i] != '-')
				id.push_back((char)file[i]);
		return id;
	}
	return "";
}

void screenshot(const std::string& rom)
{
	std::vector<uint8_t> rgb;
	int w = 0, h = 0;
	graphics.panel.drain();
	if (!graphics.panel.readShown(rgb, w, h))
		return;
	const char *sd = getenv("SDCARD_PATH");
	const std::string dir = std::string(sd != nullptr ? sd : "/mnt/SDCARD") + "/Screenshots";
	mkdir(dir.c_str(), 0755);
	char stamp[32];
	const time_t now = time(nullptr);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	const std::string path = dir + "/" + ui::gameName(rom) + "." + stamp + ".png";
	std::vector<uint8_t> raw;
	raw.reserve((size_t)(w * 3 + 1) * h);
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
	}
	uLongf zsize = compressBound((uLong)raw.size());
	std::vector<uint8_t> z(zsize);
	if (compress2(z.data(), &zsize, raw.data(), (uLong)raw.size(), 6) != Z_OK)
		return;
	z.resize(zsize);
	std::ofstream out(path, std::ios::binary);
	const auto be = [&](uint32_t v) { const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; out.write((const char *)b, 4); };
	const auto chunk = [&](const char *type, const std::vector<uint8_t>& data) {
		be((uint32_t)data.size());
		uLong crc = crc32(0, (const Bytef *)type, 4);
		crc = crc32(crc, data.data(), (uInt)data.size());
		out.write(type, 4);
		out.write((const char *)data.data(), (std::streamsize)data.size());
		be((uint32_t)crc);
	};
	out.write("\x89PNG\r\n\x1a\n", 8);
	std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
			(uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	DS_LOG("screenshot: %s", path.c_str());
	osd::notify("Screenshot saved", 2000);
}

// the pad's changes since the last call: (input, pressed) pairs
std::vector<std::pair<int, bool>> padEvents()
{
	std::vector<std::pair<int, bool>> events;
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_QUIT:
			quit = true;
			break;
		case SDL_JOYDEVICEADDED:
			if (joystick == nullptr)
			{
				joystick = SDL_JoystickOpen(event.jdevice.which);
				controls::open(joystick);
			}
			break;
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			events.push_back({ event.jbutton.button, event.type == SDL_JOYBUTTONDOWN });
			break;
		case SDL_JOYHATMOTION:
		{
			static Uint8 lastHat = 0;
			static const Uint8 Bits[] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
			for (int i = 0; i < 4; i++)
				if ((event.jhat.value & Bits[i]) != (lastHat & Bits[i]))
					events.push_back({ controls::HatUp + i, (event.jhat.value & Bits[i]) != 0 });
			lastHat = event.jhat.value;
			break;
		}
		case SDL_JOYAXISMOTION:
		{
			static bool down[8];
			if (event.jaxis.axis < 8)
			{
				const bool now = event.jaxis.value > 16384;
				if (now != down[event.jaxis.axis])
					events.push_back({ controls::Axis + event.jaxis.axis, now });
				down[event.jaxis.axis] = now;
			}
			break;
		}
		default:
			break;
		}
	}
	return events;
}

// The pad on a thread of its own while the game runs: reading it is a syscall or two and a dozen locks, a percent of the CPU's frame in God of War.
std::mutex padMutex;
std::vector<std::pair<int, bool>> padQueue;
std::atomic<bool> padRunning { false };

void padThread()
{
	pthread_setname_np(pthread_self(), "dsp-pad");
	while (padRunning)
	{
		const auto events = padEvents();
		controls::update();
		if (!events.empty())
		{
			std::lock_guard<std::mutex> lock(padMutex);
			padQueue.insert(padQueue.end(), events.begin(), events.end());
		}
		SDL_Delay(8);
	}
}

std::vector<std::pair<int, bool>> takePadEvents()
{
	std::vector<std::pair<int, bool>> events;
	std::lock_guard<std::mutex> lock(padMutex);
	events.swap(padQueue);
	return events;
}

// the game until its next frame, and that frame handed to the GE's thread (hostFrameEnd), which is drawing the one before
void hostFrame()
{
	PSP_RunLoopWhileState();
	if (coreState == CORE_NEXTFRAME)
		coreState = CORE_RUNNING_CPU;
	ge::endFrame(__DisplayTakeFlipForced());
	g_frameTiming.PostSubmit();
}

// on the GE's thread: the frame into the next page, if there is a new picture or force says so
bool hostFrameEnd(Draw::DrawContext *draw, bool force)
{
	gpu->FramebufferDirty();
	const bool really = gpu->FramebufferReallyDirty();
	if (!really && !force)
		return false;
	gpu->SetCurFramebufferDirty(really);
	if (really)
		DisplayFireActualFlip();
	gpu->PrepareCopyDisplayToOutput();
	gpu->EndHostFrame();
	// opaque black behind the picture: the display blends by each pixel's alpha, and the game's alpha is its stencil
	draw->BindFramebufferAsRenderTarget(nullptr, { Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, 0xFF000000 }, "page");
	gpu->CopyDisplayToOutput();
	draw->EndFrame();
	draw->Present(Draw::PresentMode::FIFO);
	return true;
}

// a save state's work, or a mode's, can't be while a frame is under way
void afterMenu(bool& showFps, bool& failed)
{
	settings::apply();
	showFps = settings::showFps();
	if (settings::changed() & settings::Resolution)
	{
		std::string error;
		if (!graphics.remake(settings::resolution(), error))
		{
			DS_LOG("the screen: %s", error.c_str());
			failed = true;
			return;
		}
		gpu->NotifyDisplayResized();
		gpu->NotifyRenderResized();
		graphics.panel.matchRate();
		osd::again();
	}
	gpu->NotifyConfigChanged();
}

}	// namespace

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--restore-panel") == 0)
	{
		Panel::restoreLeftRate();
		return 0;
	}
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: deadstation-portable <game>\n");
		return 2;
	}
	const std::string game = argv[1];
	const char *romEnv = getenv("DEADSTATION_PORTABLE_ROM");
	const std::string rom = romEnv != nullptr && *romEnv != '\0' ? romEnv : game;
	const char *homeEnv = getenv("DEADSTATION_PORTABLE_HOME");
	const std::string home = homeEnv != nullptr ? homeEnv : ".";
	DS_LOG("%s, game %s", DEADSTATION_VERSION, game.c_str());
	// the game's own settings and controls, by its disc id (games/<id>.cfg, games/<id>.controls.cfg), before either is read
	const std::string discId = pspDiscId(game);
	DS_LOG("disc %s", discId.empty() ? "(no id)" : discId.c_str());
	settings::setGame(discId);

	// recomp modules marked for deletion deleted, an install cut short tidied away; then the disc's recomp module
	// hosted, if that is its chosen core (no emulator started)
	recomp::setDiscId(pspDiscId);
	recomp::tidy();
	if (recomp::get("app.version") != DEADSTATION_APP_VERSION)
		recomp::set("app.version", DEADSTATION_APP_VERSION);
	recomp::runChosen(game, [] {
		menu::openHostedControls(
				[] {
					if (SDL_Init(SDL_INIT_JOYSTICK) != 0)
						return false;
					if (SDL_NumJoysticks() > 0)
						joystick = SDL_JoystickOpen(0);
					controls::open(joystick);
					controls::load();
					return joystick != nullptr;
				},
				[] {
					if (joystick != nullptr)
						SDL_JoystickClose(joystick);
					joystick = nullptr;
					controls::open(nullptr);
					SDL_Quit();
				},
				padEvents);
	});

	signal(SIGTERM, [](int) { quit = true; });
	signal(SIGUSR1, [](int) { shotWanted = true; });
	// A trap (an assert's Crash(), a BRK in code), an illegal instruction or a memory fault: where, said out loud, then the default end. The address
	// is the executable's own when it's in it (llvm-addr2line -e the unstripped build), else absolute (JIT code, a library).
	{
		struct sigaction crash {};
		crash.sa_flags = SA_SIGINFO | SA_RESETHAND;
		crash.sa_sigaction = [](int sig, siginfo_t *info, void *context) {
			const ucontext_t *uc = (const ucontext_t *)context;
			const uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc, lr = (uintptr_t)uc->uc_mcontext.regs[30];
			char line[256];
			const auto where = [](uintptr_t a, char *out, size_t size) {
				Dl_info info {};
				if (dladdr((void *)a, &info) != 0 && info.dli_fname != nullptr)
					snprintf(out, size, "%s+0x%zx", strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname, (size_t)(a - (uintptr_t)info.dli_fbase));
				else
					snprintf(out, size, "0x%zx", (size_t)a);
			};
			char at[96], from[96];
			where(pc, at, sizeof(at));
			where(lr, from, sizeof(from));
			// a fault PPSSPP's memory handler didn't take (it hands them here): the address, and the default end (its handler would call
			// this one again at every retry)
			if (sig == SIGSEGV || sig == SIGBUS)
				signal(sig, SIG_DFL);
			const int n = (sig == SIGSEGV || sig == SIGBUS) ?
				snprintf(line, sizeof(line), "Deadstation Portable: CRASHED, signal %d at %s, return address %s, fault address %p\n", sig, at, from, info != nullptr ? info->si_addr : nullptr) :
				snprintf(line, sizeof(line), "Deadstation Portable: CRASHED, signal %d at %s, return address %s\n", sig, at, from);
			if (n > 0)
				(void)!write(2, line, (size_t)n);
			// the mappings the two are in (an anonymous rwx one is JIT code)
			const int maps = open("/proc/self/maps", O_RDONLY);
			if (maps >= 0) {
				static char buf[1 << 16];
				size_t have = 0;
				ssize_t got;
				while (have < sizeof(buf) - 1 && (got = read(maps, buf + have, sizeof(buf) - 1 - have)) > 0)
					have += (size_t)got;
				close(maps);
				buf[have] = '\0';
				for (char *row = buf; *row != '\0';) {
					char *end = strchr(row, '\n');
					if (end != nullptr)
						*end = '\0';
					const uintptr_t lo = (uintptr_t)strtoull(row, nullptr, 16);
					const char *dash = strchr(row, '-');
					const uintptr_t hi = dash != nullptr ? (uintptr_t)strtoull(dash + 1, nullptr, 16) : 0;
					if ((pc >= lo && pc < hi) || (lr >= lo && lr < hi)) {
						const int m = snprintf(line, sizeof(line), "Deadstation Portable: CRASHED in %s: %s\n", pc >= lo && pc < hi ? (lr >= lo && lr < hi ? "pc, lr" : "pc") : "lr", row);
						if (m > 0)
							(void)!write(2, line, (size_t)m);
					}
					if (end == nullptr)
						break;
					row = end + 1;
				}
			}
			// the registers (a JIT dispatch into a block: x16 where it went, x17 the guest word's top byte, x26 the JIT's base, x27 the
			// MIPS state, x28 guest memory), and where the guest was: its pc and the word there (0x68...... a block's mark)
			for (int i = 0; i < 31; i += 4) {
				const auto &r = uc->uc_mcontext.regs;
				const int m = i + 3 < 31 ?
					snprintf(line, sizeof(line), "Deadstation Portable: CRASHED x%d-x%d %016llx %016llx %016llx %016llx\n", i, i + 3,
						(unsigned long long)r[i], (unsigned long long)r[i + 1], (unsigned long long)r[i + 2], (unsigned long long)r[i + 3]) :
					snprintf(line, sizeof(line), "Deadstation Portable: CRASHED x%d-x30 %016llx %016llx %016llx, sp %016llx\n", i,
						(unsigned long long)r[i], (unsigned long long)r[i + 1], (unsigned long long)r[i + 2], (unsigned long long)uc->uc_mcontext.sp);
				if (m > 0)
					(void)!write(2, line, (size_t)m);
			}
			if (currentMIPS != nullptr && MIPSComp::jit != nullptr) {
				const u32 gpc = currentMIPS->pc;
				const u32 word = Memory::IsValid4AlignedAddress(gpc) ? Memory::ReadUnchecked_U32(gpc) : 0;
				const int m = snprintf(line, sizeof(line), "Deadstation Portable: CRASHED guest pc %08x, word there %08x, JIT code base %p, its blocks %d\n",
					gpc, word, (const void *)MIPSComp::jit->GetCodeBase(), MIPSComp::jit->GetBlockCache() ? MIPSComp::jit->GetBlockCache()->GetNumBlocks() : -1);
				if (m > 0)
					(void)!write(2, line, (size_t)m);
				// the newest block (where compiled code ends) and the block the guest word names, if it's a block's mark
				JitBlockCache *blocks = MIPSComp::jit->GetBlockCache();
				if (blocks != nullptr && blocks->GetNumBlocks() > 0) {
					const JitBlock *last = blocks->GetBlock(blocks->GetNumBlocks() - 1);
					const int k = blocks->GetBlockNumberFromEmuHackOp(MIPSOpcode(word), true);
					const JitBlock *named = k >= 0 && k < blocks->GetNumBlocks() ? blocks->GetBlock(k) : nullptr;
					const int q = snprintf(line, sizeof(line), "Deadstation Portable: CRASHED newest block %08x at %p +%u; the word's block %d: %08x at %p +%u%s\n",
						last->originalAddress, (const void *)last->normalEntry, (unsigned)last->codeSize, k,
						named ? named->originalAddress : 0u, named ? (const void *)named->normalEntry : nullptr, named ? (unsigned)named->codeSize : 0u,
						named && named->invalid ? " (invalid)" : "");
					if (q > 0)
						(void)!write(2, line, (size_t)q);
				}
			}
		};
		sigaction(SIGTRAP, &crash, nullptr);
		sigaction(SIGILL, &crash, nullptr);
		// (installed before the core's own fault handler, which keeps these and hands on the faults it doesn't take)
		sigaction(SIGSEGV, &crash, nullptr);
		sigaction(SIGBUS, &crash, nullptr);
	}
	signal(SIGINT, [](int) { quit = true; });

	if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0)
	{
		DS_LOG("SDL: %s", SDL_GetError());
		return 1;
	}
	if (SDL_NumJoysticks() > 0)
		joystick = SDL_JoystickOpen(0);
	controls::open(joystick);
	controls::load();
	controls::setGame(rom);
	states::setGame(rom);

	// what the emulation needs around it
	static bool logging = true;
	g_logManager.Init(&logging);
	g_logManager.EnableOutput(LogOutput::Stdio);
	g_logManager.SetAllLogLevels(LogLevel::LWARNING);
	net::Init();
	g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);
	const Path pakAssets = File::GetExeDirectory() / "assets";
	g_VFS.Register("", new DirectoryReader(pakAssets));
	g_i18nrepo.LoadIni(g_Config.sLanguageIni);
	g_Config.flash0Directory = pakAssets / "flash0";
	g_Config.memStickDirectory = Path(home) / "memstick";
	g_Config.appCacheDirectory = Path(home) / "cache";
	for (PSPDirectories d : { DIRECTORY_PSP, DIRECTORY_SAVEDATA, DIRECTORY_SYSTEM, DIRECTORY_CACHE, DIRECTORY_PAUTH })
		File::CreateFullPath(GetSysDirectory(d));
	settings::apply();

	std::string error;
	if (!graphics.init(settings::resolution(), error))
	{
		DS_LOG("the screen: %s", error.c_str());
		SDL_Quit();
		return 1;
	}
	Panel& panel = graphics.panel;
	menu::init(&panel, rom, game);
	osd::init(&panel);
	menu::showStart();

	CoreParameter coreParam {};
	coreParam.cpuCore = (CPUCore)g_Config.iCpuCore;
	coreParam.graphicsContext = &graphics;
	coreParam.enableSound = true;
	coreParam.fileToStart = Path(game);
	coreParam.pixelWidth = (int)panel.Width();
	coreParam.pixelHeight = (int)panel.Height();
	if (!PSP_InitStart(coreParam))
	{
		menu::showStartError("The game could not be loaded.");
		graphics.Shutdown();
		SDL_Quit();
		return 1;
	}
	std::string bootError;
	BootState boot;
	while ((boot = PSP_InitUpdate(&bootError)) == BootState::Booting)
	{
		padEvents();
		SDL_Delay(5);
	}
	if (boot != BootState::Complete)
	{
		DS_LOG("the game could not load: %s", bootError.c_str());
		menu::showStartError(bootError.empty() ? "The game could not be loaded." : bootError);
		graphics.Shutdown();
		SDL_Quit();
		return 1;
	}
	// The first time this game runs (never in tests): a word about shader compilation's stutter, on its own page until a button.
	// Seen once, a game is marked so in its cache folder (<disc ID>.played), whether or not it ever keeps shaders.
	{
		const Path dir = GetSysDirectory(DIRECTORY_APP_CACHE);
		const std::string id = g_paramSFO.GetDiscID();
		const Path played = dir / (id + ".played");
		if (!id.empty() && getenv("DEADSTATION_PORTABLE_STATE") == nullptr && !File::Exists(played))
		{
			menu::firstRunNotice(padEvents, [] { return quit.load(); });
			controls::toGame();
			File::CreateFullPath(dir);
			File::CreateEmptyFile(played);
		}
	}
	profiler::startThread("main");
	// Hashing every texture at every use is a fifth of a megabyte a draw... God of War's don't change behind the game's back.
	g_Config.bTextureBackoffCache = PSP_CoreParameter().compat.flags().LazyTextureHash;
	coreState = CORE_RUNNING_CPU;
	if (panel.matchRate())
		__DisplaySetPanelRate(Panel::GameHz);
	audio::start();

	Draw::DrawContext *draw = graphics.GetDrawContext();
	ge::measure(getenv("DEADSTATION_PORTABLE_PERF") != nullptr);
	ge::start({
		[draw] {
			draw->BeginFrame(Draw::DebugFlags::NONE);
			gpu->BeginHostFrame();
		},
		[draw](bool force) { return hostFrameEnd(draw, force); },
	});
	const char *testState = getenv("DEADSTATION_PORTABLE_STATE");
	const bool perfEverySecond = getenv("DEADSTATION_PORTABLE_PERF") != nullptr;
	bool showFps = settings::showFps();
	int exitCode = 0;
	uint64_t perfFrom = 0;
	double perfAt = time_now_d();
	unsigned perfFrames = 0;
	int flipsFrom = __DisplayGetFlipCount();
	int vblanksFrom = __DisplayGetVCount();
	uint64_t shownFrom = 0, heldFrom = 0;
	double runWorst = 0;

	jitcache::start((GetSysDirectory(DIRECTORY_APP_CACHE) / (g_paramSFO.GetDiscID() + ".jitblocks")).ToString());
	cpufreq::start();

	padRunning = true;
	std::thread pad(padThread);

	while (!quit)
	{
		const auto events = takePadEvents();
		if (menu::isOpen())
		{
			controls::toMenu();
			cpufreq::release();
			for (const auto& [code, pressed] : events)
				menu::input(code, pressed);
			const menu::Result result = menu::frame();
			if (result == menu::Result::Open)
			{
				// a picture shown: straight on, the flip waiting for the panel's refresh; nothing moving: the pad, 8 ms on
				if (!ui::shown())
					SDL_Delay(8);
				continue;
			}
			if (result == menu::Result::Quit)
				break;
			if (result == menu::Result::Switch)
			{
				exitCode = recomp::SwitchExit;
				break;
			}
			bool failed = false;
			afterMenu(showFps, failed);
			if (failed)
				break;
			if (coreState == CORE_RUNTIME_ERROR)
				break;
			controls::toGame();
			audio::clear();
			audio::pause(false);
			__DisplaySetWasPaused();
			continue;
		}
		bool openMenu = false;
		for (const auto& [code, pressed] : events)
		{
			if (!pressed)
				continue;
			if (code == controls::MenuButton)
				openMenu = true;
			else if (code == controls::binding(controls::SwapDpad))
			{
				controls::toggleDpad();
				osd::notify(controls::dpadIsNub() ? "D-pad: analog nub" : "D-pad: d-pad", 1500);
			}
			else if (code == controls::binding(controls::SaveState))
			{
				ge::pause();
				audio::pause(true);
				osd::notify(menu::saveSlot() ? "State saved" : "The game could not be saved", 2000);
				audio::pause(false);
				__DisplaySetWasPaused();
			}
			else if (code == controls::binding(controls::LoadState))
			{
				ge::pause();
				if (!menu::slotExists())
					osd::notify("Save state not found", 2000);
				else if (!menu::loadSlot())
					osd::notify("The game could not be loaded", 2000);
				audio::clear();
				__DisplaySetWasPaused();
			}
			else if (code == controls::binding(controls::Screenshot))
			{
				ge::pause();
				screenshot(rom);
			}
		}
		if (openMenu)
		{
			ge::pause();
			audio::pause(true);
			// the shaders the game has built so far, kept for its next start (nothing to write if none are new)
			gpu->SaveShaderCache();
			if (menu::open())
				continue;
			audio::pause(false);
		}
		// (tests) the page on show at a moment of the game's own clock, whatever the host's speed: DEADSTATION_PORTABLE_SHOT_AT=vblanks since the state
		static const int shotAt = getenv("DEADSTATION_PORTABLE_SHOT_AT") != nullptr ? atoi(getenv("DEADSTATION_PORTABLE_SHOT_AT")) : 0;
		static int shotFrom = -1;
		if (shotAt != 0 && testState == nullptr && shotFrom < 0)
			shotFrom = __DisplayGetVCount();
		if (shotAt != 0 && shotFrom >= 0 && __DisplayGetVCount() - shotFrom >= shotAt && shotFrom != INT_MAX)
		{
			shotFrom = INT_MAX;
			DS_LOG("test: picture at vblank %d, %d game frames", __DisplayGetVCount(), __DisplayGetFlipCount());
			shotWanted = true;
		}
		if (shotWanted.exchange(false))
		{
			std::vector<uint8_t> rgb;
			int w = 0, h = 0;
			ge::pause();
			panel.drain();
			if (panel.readShown(rgb, w, h))
			{
				std::ofstream out("/tmp/dsp-shot.ppm.tmp", std::ios::binary);
				out << "P6\n" << w << " " << h << "\n255\n";
				out.write((const char *)rgb.data(), (std::streamsize)rgb.size());
				out.close();
				std::rename("/tmp/dsp-shot.ppm.tmp", "/tmp/dsp-shot.ppm");
			}
		}
		if (testState != nullptr && *testState != '\0' && perfFrames > 0)
		{
			ge::pause();
			DS_LOG("test: state %s %s", testState, states::load(testState) ? "loaded" : "NOT loaded");
			testState = nullptr;
			audio::clear();
			__DisplaySetWasPaused();
		}

		{
			static bool once = false;
			if (!once) {
				once = true;
				// Ahead of the kernel's and everyone else's small jobs: a fight's frame has no time to give them. Here, so what's already started isn't.
				setpriority(PRIO_PROCESS, 0, -5);
			}
		}
		const double before = time_now_d();
		hostFrame();
		runWorst = std::max(runWorst, time_now_d() - before);
		{
			// A frame with time to spare goes to translating what the game's known to run, before it's needed in a fight.
			static double lastWall = 0.0, lastCpu = 0.0;
			timespec cpuNow;
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuNow);
			const double wall = time_now_d(), cpu = cpuNow.tv_sec + cpuNow.tv_nsec * 1e-9;
			if (!menu::starting() && cpu - lastCpu < (wall - lastWall) * 0.6)
				jitcache::tick(1.0);
			lastWall = wall;
			lastCpu = cpu;
		}
		if (menu::starting() && __DisplayGetFlipCount() != flipsFrom)
		{
			ge::pause();
			menu::endStart();
		}
		if (!menu::starting())
		{
			osd::frame(showFps);
			cpufreq::tick();
		}
		perfFrames++;

		if (coreState == CORE_RUNTIME_ERROR || coreState == CORE_POWERDOWN)
		{
			// the game asked to exit, or can't go on
			if (coreState == CORE_POWERDOWN)
				break;
			ge::pause();
			DS_LOG("the game stopped");
			audio::pause(true);
			if (!menu::openWithMessage("The game stopped.\nSee the log."))
				break;
			continue;
		}

		const double now = time_now_d();
		if (now - perfAt >= 1.0)
		{
			static int perfCount = 0;
			if (perfEverySecond || ++perfCount % 10 == 0)
			{
				const int flips = __DisplayGetFlipCount(), vblanks = __DisplayGetVCount();
				const uint64_t shown = panel.framesShown();
				const double span = now - perfAt;
				static u64 busyFrom = 0, waitedFrom = 0;
				const int perfFlips = std::max(1, __DisplayGetVCount() - vblanksFrom);
				timespec cpuNow;
				clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuNow);
				const u64 mainCpu = (u64)cpuNow.tv_sec * 1000000000 + cpuNow.tv_nsec;
				static u64 mainCpuFrom = 0, geCpuFrom = 0;
				DS_LOG("ge: %.1f ms a vblank busy, the CPU %.1f ms waiting for it; CPU time a vblank: main %.1f ms, ge %.1f ms", (ge::busyNs() - busyFrom) / 1e6 / perfFlips, (ge::waitedNs() - waitedFrom) / 1e6 / perfFlips, (mainCpu - mainCpuFrom) / 1e6 / perfFlips, (ge::cpuNs() - geCpuFrom) / 1e6 / perfFlips);
				DS_LOG("ge waits: %s", ge::takeWaitSites().c_str());
				// no frame from the game for 3 s while its time runs: the display lists, once, to see what holds them
				static int stuckSince = -1;
				static bool stuckTold = false;
				if (flips != flipsFrom || vblanks == vblanksFrom) {
					stuckSince = -1;
				} else if (stuckSince < 0) {
					stuckSince = vblanks;
				} else if (!stuckTold && vblanks - stuckSince > 180) {
					stuckTold = true;
					DS_LOG("STUCK: no frame in %d vblanks; GE busy %d, lists queued %d", vblanks - stuckSince, ge::busy() ? 1 : 0, gpu->ListsQueued() ? 1 : 0);
					for (int i = 0; i < 64; i++) {
						const DisplayList *dl = gpu->getList(i);
						if (dl != nullptr && dl->state != PSP_GE_DL_STATE_NONE)
							DS_LOG("STUCK: list %d state %d pc %08x stall %08x signal %d intr %d pending %d started %d sub %d", i, (int)dl->state, dl->pc, dl->stall, (int)dl->signal, dl->interruptsEnabled ? 1 : 0, dl->pendingInterrupt ? 1 : 0, dl->started ? 1 : 0, dl->subIntrBase);
					}
				}
				mainCpuFrom = mainCpu;
				geCpuFrom = ge::cpuNs();
				busyFrom = ge::busyNs();
				waitedFrom = ge::waitedNs();
				const uint64_t held = panel.framesHeld();
				// "game frames": the ones drawn (a skipped frame isn't); "the game made": every frame the game finished, skipped or drawn.
				// The PSP's CPU clock the game asked for (scePower; a state brings back the one saved with it): 222 MHz unless it sets 333
				static int madeFrom = gpuStats.numFlips;
				// and the Brick's own clock now, as its governor set it
				int brickKhz = 0;
				if (FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r")) {
					if (fscanf(f, "%d", &brickKhz) != 1)
						brickKhz = 0;
					fclose(f);
				}
				DS_LOG("perf: %.1f game frames/s (of %.1f vblanks/s), %.1f shown, %u host frames, worst %.1f ms, sound level %u, %u underruns, %u dropped, %u held, PSP CPU %d MHz, the game made %.1f frames/s, Brick CPU %d MHz",
						(flips - flipsFrom) / span, (vblanks - vblanksFrom) / span, (double)(shown - shownFrom) / span, perfFrames, runWorst * 1000.0,
						audio::level(), audio::takeUnderruns(), audio::takeDropped(), (unsigned)(held - heldFrom), CoreTiming::GetClockFrequencyHz() / 1000000,
						(gpuStats.numFlips - madeFrom) / span, brickKhz / 1000);
				madeFrom = gpuStats.numFlips;
				heldFrom = held;
				if (g_Config.bAutoFrameSkip) {
					float drawnCost = 0.f, skippedCost = 0.f;
					bool skipOn = false;
					__DisplaySkipStats(&drawnCost, &skippedCost, &skipOn);
					int skipCount = 0, waitCount = 0;
					float waitMs = 0.f, lateMean = 0.f, lateWorst = 0.f;
					__DisplayTakeSkipCounts(&skipCount, &waitCount, &waitMs, &lateMean, &lateWorst);
					DS_LOG("skip: a second of the game's time took %.3f s drawn, %.3f s skipped; skipping %s; %d skipped (at %.1f ms late, worst %.1f), %d waited %.0f ms in all", drawnCost, skippedCost, skipOn ? "allowed" : "off", skipCount, lateMean, lateWorst, waitCount, waitMs);
				}
				flipsFrom = flips;
				vblanksFrom = vblanks;
				shownFrom = shown;
			}
			perfAt = now;
			perfFrames = 0;
			runWorst = 0;
		}
	}
	padRunning = false;
	pad.join();

	DS_LOG("quitting");
	cpufreq::stop();
	ge::pause();
	jitcache::finish();
	ge::stop();
	profiler::dump();
	audio::stop();
	PSP_Shutdown();
	graphics.Shutdown();
	g_threadManager.Teardown();
	g_VFS.Clear();
	g_logManager.Shutdown();
	if (joystick != nullptr)
		SDL_JoystickClose(joystick);
	SDL_Quit();
	return exitCode;
}
