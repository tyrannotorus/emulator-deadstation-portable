
#include "GPU/Common/VramAlias.h"
#include "Common/GPU/thin3d.h"
#include "Common/Serialize/Serializer.h"
#include "Common/System/System.h"

#include "Core/System.h"
#include "Core/Config.h"
#include "Core/Util/PPGeDraw.h"

#include "GPU/GPUCommonHW.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/FramebufferManagerCommon.h"

struct CommonCommandTableEntry {
	uint8_t cmd;
	uint8_t flags;
	uint64_t dirty;
	GPUCommonHW::CmdFunc func;
};

struct CommandInfo {
	uint64_t flags;
	GPUCommonHW::CmdFunc func;

	// Dirty flags are mashed into the regular flags by a left shift of 8.
	void AddDirty(u64 dirty) {
		flags |= dirty << 8;
	}
	void RemoveDirty(u64 dirty) {
		flags &= ~(dirty << 8);
	}
};

static CommandInfo cmdInfo_[256];

// The command loop is hand-written ARM64 (GeRunLoopArm64.cpp). Its tables: a flags byte a command, then its dirty bits; and each
// command's handler as a plain function.
static struct { u8 flags[256]; u64 dirty[256]; } geTable_ alignas(64);
static void *geHandlers_[256];
static void BuildGeTables() {
	for (int i = 0; i < 256; i++) {
		const u64 flags = cmdInfo_[i].flags;
		geTable_.flags[i] = ((flags & FLAG_EXECUTE) ? 1 : 0) | ((flags & (FLAG_EXECUTE | FLAG_EXECUTEONCHANGE)) ? 2 : 0) | ((flags & FLAG_FLUSHBEFOREONCHANGE) ? 4 : 0);
		geTable_.dirty[i] = flags >> 8;
		// A member pointer (Itanium ABI, AArch64): the function's address (or a vtable offset), then the this-adjustment << 1 |
		// virtual. The handlers are non-virtual members of GPUCommonHW and its first base: the address is the function.
		struct Raw { uintptr_t ptr; intptr_t adj; } raw;
		static_assert(sizeof(Raw) == sizeof(GPUCommonHW::CmdFunc), "member pointer layout");
		memcpy(&raw, &cmdInfo_[i].func, sizeof(raw));
		_assert_msg_(raw.adj == 0, "GE command %02x: a virtual or this-adjusting handler, which the ARM64 loop can't call", i);
		geHandlers_[i] = (void *)raw.ptr;
	}
}
struct GeLoopArgs {
	GPUCommonHW *gpu;
	u32 *pc;
	const u8 *base;
	u32 *cmdmem;
	void *const *handlers;
	const u8 *table;
	uint64_t *dirty;
	const int *pending;
	int *downcount;
	void (*flush)(GPUCommonHW *gpu);
};
static_assert(sizeof(GeLoopArgs) == 80, "GeLoopArgs: the asm reads it at fixed offsets");
extern "C" void GeRunLoop(const GeLoopArgs *args);
void GPUCommonHW::FlushDrawEngineForAsm() {
	drawEngineCommon_->Flush();
}
static void GeFlushThunk(GPUCommonHW *gpu) {
	gpu->FlushDrawEngineForAsm();
}

const CommonCommandTableEntry commonCommandTable[] = {
	// From Common. No flushing but definitely need execute.
	{ GE_CMD_OFFSETADDR, FLAG_EXECUTE, 0, &GPUCommon::Execute_OffsetAddr },
	{ GE_CMD_ORIGIN, FLAG_EXECUTE | FLAG_READS_PC, 0, &GPUCommon::Execute_Origin },
	{ GE_CMD_JUMP, FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommon::Execute_Jump },
	{ GE_CMD_CALL, FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommon::Execute_Call },
	{ GE_CMD_RET, FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommon::Execute_Ret },
	{ GE_CMD_END, FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommon::Execute_End },
	{ GE_CMD_VADDR, FLAG_EXECUTE, 0, &GPUCommon::Execute_Vaddr },
	{ GE_CMD_IADDR, FLAG_EXECUTE, 0, &GPUCommon::Execute_Iaddr },
	{ GE_CMD_BJUMP, FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommon::Execute_BJump },  // EXECUTE
	{ GE_CMD_BOUNDINGBOX, FLAG_EXECUTE, 0, &GPUCommonHW::Execute_BoundingBox }, // Shouldn't need to FLUSHBEFORE.

	{ GE_CMD_PRIM, FLAG_EXECUTE, 0, &GPUCommonHW::Execute_Prim },
	{ GE_CMD_BEZIER, FLAG_EXECUTE, 0, &GPUCommonHW::Execute_Bezier },
	{ GE_CMD_SPLINE, FLAG_EXECUTE, 0, &GPUCommonHW::Execute_Spline },

	// Changing the vertex type requires us to flush.
	{ GE_CMD_VERTEXTYPE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommonHW::Execute_VertexType },

	{ GE_CMD_LOADCLUT, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE, 0, &GPUCommonHW::Execute_LoadClut},

	// These two are actually processed in CMD_END.
	{ GE_CMD_SIGNAL },
	{ GE_CMD_FINISH },

	// Changes that dirty the framebuffer
	{ GE_CMD_FRAMEBUFPTR, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_FRAMEBUFWIDTH, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE },
	{ GE_CMD_FRAMEBUFPIXFORMAT, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_ZBUFPTR, FLAG_FLUSHBEFOREONCHANGE },
	{ GE_CMD_ZBUFWIDTH, FLAG_FLUSHBEFOREONCHANGE },

	{ GE_CMD_FOGCOLOR, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FOGCOLOR },
	{ GE_CMD_FOG1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FOGCOEF },
	{ GE_CMD_FOG2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FOGCOEF },

	// These affect the fragment shader so need flushing.
	{ GE_CMD_CLEARMODE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE },
	{ GE_CMD_TEXTUREMAPENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE },
	{ GE_CMD_FOGENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_TEXMODE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_TEXSHADELS, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE },
	// Raster state for Direct3D 9, uncommon.
	{ GE_CMD_SHADEMODE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE },
	{ GE_CMD_TEXFUNC, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAGMENTSHADER_STATE | DIRTY_TEX_ALPHA_MUL },
	{ GE_CMD_COLORTEST, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_ALPHATESTENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_COLORTESTENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_COLORTESTMASK, FLAG_FLUSHBEFOREONCHANGE, DIRTY_ALPHACOLORMASK | DIRTY_FRAGMENTSHADER_STATE },

	// These change the vertex shader so need flushing.
	{ GE_CMD_REVERSENORMAL, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE },
	{ GE_CMD_LIGHTINGENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE },
	{ GE_CMD_LIGHTENABLE0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_LIGHTENABLE1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_LIGHTENABLE2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_LIGHTENABLE3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_LIGHTTYPE0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_LIGHT0 },
	{ GE_CMD_LIGHTTYPE1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_LIGHT1 },
	{ GE_CMD_LIGHTTYPE2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_LIGHT2 },
	{ GE_CMD_LIGHTTYPE3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_LIGHT3 },
	{ GE_CMD_MATERIALUPDATE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE },

	// These change all shaders so need flushing.
	{ GE_CMD_LIGHTMODE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE },

	{ GE_CMD_TEXFILTER, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXWRAP, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS | DIRTY_FRAGMENTSHADER_STATE },

	// Uniform changes. though the fragmentshader optimizes based on these sometimes.
	{ GE_CMD_ALPHATEST, FLAG_FLUSHBEFOREONCHANGE, DIRTY_ALPHACOLORREF | DIRTY_ALPHACOLORMASK | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_COLORREF, FLAG_FLUSHBEFOREONCHANGE, DIRTY_ALPHACOLORREF | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_TEXENVCOLOR, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXENV },

	// Simple render state changes. Handled in StateMapping.cpp.
	{ GE_CMD_CULL, FLAG_FLUSHBEFOREONCHANGE, DIRTY_RASTER_STATE },
	{ GE_CMD_CULLFACEENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_RASTER_STATE },
	{ GE_CMD_DITHERENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_RASTER_STATE },
	{ GE_CMD_STENCILOP, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_STENCILTEST, FLAG_FLUSHBEFOREONCHANGE, DIRTY_STENCILREPLACEVALUE | DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE },
	{ GE_CMD_STENCILTESTENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_ALPHABLENDENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_BLENDMODE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_BLENDFIXEDA, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_BLENDFIXEDB, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_MASKRGB, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_COLORWRITEMASK },
	{ GE_CMD_MASKALPHA, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_COLORWRITEMASK },
	{ GE_CMD_ZTEST, FLAG_FLUSHBEFOREONCHANGE, DIRTY_DEPTHSTENCIL_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_ZTESTENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_DEPTHSTENCIL_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_ZWRITEDISABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_DEPTHSTENCIL_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_LOGICOP, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE },
	{ GE_CMD_LOGICOPENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE },

	{ GE_CMD_TEXMAPMODE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE },

	// These are read on every SubmitPrim, no need for dirtying or flushing.
	{ GE_CMD_TEXSCALEU },
	{ GE_CMD_TEXSCALEV },
	{ GE_CMD_TEXOFFSETU },
	{ GE_CMD_TEXOFFSETV },

	{ GE_CMD_TEXSIZE0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE, 0, &GPUCommonHW::Execute_TexSize0 },
	{ GE_CMD_TEXSIZE1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXSIZE2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXSIZE3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXSIZE4, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXSIZE5, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXSIZE6, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXSIZE7, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXFORMAT, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_IMAGE },
	{ GE_CMD_TEXLEVEL, FLAG_EXECUTEONCHANGE, DIRTY_TEXTURE_PARAMS, &GPUCommonHW::Execute_TexLevel },
	{ GE_CMD_TEXLODSLOPE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_IMAGE },
	{ GE_CMD_TEXADDR1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR4, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR5, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR6, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXADDR7, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_IMAGE },
	{ GE_CMD_TEXBUFWIDTH1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH4, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH5, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH6, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },
	{ GE_CMD_TEXBUFWIDTH7, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS },

	// These must flush on change, so that LoadClut doesn't have to always flush.
	{ GE_CMD_CLUTADDR, FLAG_FLUSHBEFOREONCHANGE },
	{ GE_CMD_CLUTADDRUPPER, FLAG_FLUSHBEFOREONCHANGE },
	{ GE_CMD_CLUTFORMAT, FLAG_FLUSHBEFOREONCHANGE, DIRTY_TEXTURE_PARAMS | DIRTY_DEPAL },

	// Morph weights. TODO: Remove precomputation?
	{ GE_CMD_MORPHWEIGHT0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT4, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT5, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT6, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },
	{ GE_CMD_MORPHWEIGHT7, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE, 0, &GPUCommon::Execute_MorphWeight },

	// Control spline/bezier patches. Don't really require flushing as such, but meh.
	{ GE_CMD_PATCHDIVISION, FLAG_FLUSHBEFOREONCHANGE },
	{ GE_CMD_PATCHPRIMITIVE, FLAG_FLUSHBEFOREONCHANGE },
	{ GE_CMD_PATCHFACING, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VERTEXSHADER_STATE },
	{ GE_CMD_PATCHCULLENABLE, FLAG_FLUSHBEFOREONCHANGE },

	// Can probably ignore this one as we don't support AA lines.
	{ GE_CMD_ANTIALIASENABLE, FLAG_FLUSHBEFOREONCHANGE },

	// Viewport.
	{ GE_CMD_OFFSETX, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_CULL_PLANES },
	{ GE_CMD_OFFSETY, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_CULL_PLANES },
	{ GE_CMD_VIEWPORTXSCALE, FLAG_FLUSHBEFOREONCHANGE,  DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_CULLRANGE | DIRTY_PROJMATRIX | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULL_PLANES },
	{ GE_CMD_VIEWPORTYSCALE, FLAG_FLUSHBEFOREONCHANGE,  DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_CULLRANGE | DIRTY_PROJMATRIX | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULL_PLANES },
	{ GE_CMD_VIEWPORTXCENTER, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_CULLRANGE | DIRTY_PROJMATRIX | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULL_PLANES },
	{ GE_CMD_VIEWPORTYCENTER, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_CULLRANGE | DIRTY_PROJMATRIX | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULL_PLANES },
	{ GE_CMD_VIEWPORTZSCALE, FLAG_FLUSHBEFOREONCHANGE,  DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_CULLRANGE | DIRTY_DEPTHRANGE | DIRTY_PROJMATRIX | DIRTY_VIEWPORTSCISSOR_STATE },
	{ GE_CMD_VIEWPORTZCENTER, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_CULLRANGE | DIRTY_DEPTHRANGE | DIRTY_PROJMATRIX | DIRTY_VIEWPORTSCISSOR_STATE },
	{ GE_CMD_DEPTHCLAMPENABLE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_RASTER_STATE },

	// Z clip
	{ GE_CMD_MINZ, FLAG_FLUSHBEFOREONCHANGE, DIRTY_DEPTHRANGE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE },
	{ GE_CMD_MAXZ, FLAG_FLUSHBEFOREONCHANGE, DIRTY_DEPTHRANGE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE },

	// Region
	{ GE_CMD_REGION1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_CULL_PLANES },
	{ GE_CMD_REGION2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_CULL_PLANES },

	// Scissor
	{ GE_CMD_SCISSOR1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_CULL_PLANES },
	{ GE_CMD_SCISSOR2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_FRAMEBUF | DIRTY_TEXTURE_PARAMS | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE | DIRTY_CULL_PLANES },

	// Lighting base colors
	{ GE_CMD_AMBIENTCOLOR, FLAG_FLUSHBEFOREONCHANGE, DIRTY_AMBIENT },
	{ GE_CMD_AMBIENTALPHA, FLAG_FLUSHBEFOREONCHANGE, DIRTY_AMBIENT },
	{ GE_CMD_MATERIALDIFFUSE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_MATDIFFUSE },
	{ GE_CMD_MATERIALEMISSIVE, FLAG_FLUSHBEFOREONCHANGE, DIRTY_MATEMISSIVE },
	{ GE_CMD_MATERIALAMBIENT, FLAG_FLUSHBEFOREONCHANGE, DIRTY_MATAMBIENTALPHA },
	{ GE_CMD_MATERIALALPHA, FLAG_FLUSHBEFOREONCHANGE, DIRTY_MATAMBIENTALPHA },
	{ GE_CMD_MATERIALSPECULAR, FLAG_FLUSHBEFOREONCHANGE, DIRTY_MATSPECULAR },
	{ GE_CMD_MATERIALSPECULARCOEF, FLAG_FLUSHBEFOREONCHANGE, DIRTY_MATSPECULAR },

	// Light parameters
	{ GE_CMD_LX0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LY0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LZ0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LX1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LY1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LZ1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LX2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LY2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LZ2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LX3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LY3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LZ3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },

	{ GE_CMD_LDX0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LDY0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LDZ0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LDX1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LDY1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LDZ1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LDX2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LDY2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LDZ2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LDX3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LDY3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LDZ3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },

	{ GE_CMD_LKA0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LKB0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LKC0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LKA1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LKB1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LKC1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LKA2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LKB2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LKC2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LKA3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LKB3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LKC3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },

	{ GE_CMD_LKS0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LKS1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LKS2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LKS3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },

	{ GE_CMD_LKO0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LKO1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LKO2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LKO3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },

	{ GE_CMD_LAC0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LDC0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LSC0, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT0 },
	{ GE_CMD_LAC1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LDC1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LSC1, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT1 },
	{ GE_CMD_LAC2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LDC2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LSC2, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT2 },
	{ GE_CMD_LAC3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LDC3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },
	{ GE_CMD_LSC3, FLAG_FLUSHBEFOREONCHANGE, DIRTY_LIGHT3 },

	// Ignored commands
	{ GE_CMD_TEXFLUSH, FLAG_EXECUTE, 0, &GPUCommonHW::Execute_TexFlush },
	{ GE_CMD_TEXSYNC, 0 },

	// These are just nop or part of other later commands.
	{ GE_CMD_NOP, 0 },
	{ GE_CMD_BASE, 0 },
	{ GE_CMD_TRANSFERSRC, 0 },
	{ GE_CMD_TRANSFERSRCW, 0 },
	{ GE_CMD_TRANSFERDST, 0 },
	{ GE_CMD_TRANSFERDSTW, 0 },
	{ GE_CMD_TRANSFERSRCPOS, 0 },
	{ GE_CMD_TRANSFERDSTPOS, 0 },
	{ GE_CMD_TRANSFERSIZE, 0 },
	{ GE_CMD_TRANSFERSTART, FLAG_EXECUTE | FLAG_READS_PC, 0, &GPUCommonHW::Execute_BlockTransferStart },

	// We don't use the dither table.
	{ GE_CMD_DITH0 },
	{ GE_CMD_DITH1 },
	{ GE_CMD_DITH2 },
	{ GE_CMD_DITH3 },

	// These handle their own flushing.
	{ GE_CMD_WORLDMATRIXNUMBER, FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommonHW::Execute_WorldMtxNum },
	{ GE_CMD_WORLDMATRIXDATA,   FLAG_EXECUTE, 0, &GPUCommonHW::Execute_WorldMtxData },
	{ GE_CMD_VIEWMATRIXNUMBER,  FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommonHW::Execute_ViewMtxNum },
	{ GE_CMD_VIEWMATRIXDATA,    FLAG_EXECUTE, 0, &GPUCommonHW::Execute_ViewMtxData },
	{ GE_CMD_PROJMATRIXNUMBER,  FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommonHW::Execute_ProjMtxNum },
	{ GE_CMD_PROJMATRIXDATA,    FLAG_EXECUTE, 0, &GPUCommonHW::Execute_ProjMtxData },
	{ GE_CMD_TGENMATRIXNUMBER,  FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommonHW::Execute_TgenMtxNum },
	{ GE_CMD_TGENMATRIXDATA,    FLAG_EXECUTE, 0, &GPUCommonHW::Execute_TgenMtxData },
	{ GE_CMD_BONEMATRIXNUMBER,  FLAG_EXECUTE | FLAG_READS_PC | FLAG_WRITES_PC, 0, &GPUCommonHW::Execute_BoneMtxNum },
	{ GE_CMD_BONEMATRIXDATA,    FLAG_EXECUTE, 0, &GPUCommonHW::Execute_BoneMtxData },

	// Vertex Screen/Texture/Color
	{ GE_CMD_VSCX },
	{ GE_CMD_VSCY },
	{ GE_CMD_VSCZ },
	{ GE_CMD_VTCS },
	{ GE_CMD_VTCT },
	{ GE_CMD_VTCQ },
	{ GE_CMD_VCV },
	{ GE_CMD_VAP, FLAG_EXECUTE, 0, &GPUCommon::Execute_ImmVertexAlphaPrim },
	{ GE_CMD_VFC },
	{ GE_CMD_VSCV },

	// "Missing" commands (gaps in the sequence)
	{ GE_CMD_UNKNOWN_03, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_0D, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_11, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_29, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_34, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_35, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_39, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_4E, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_4F, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_52, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_59, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_5A, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_B6, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_B7, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_D1, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_ED, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_EF, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_FA, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_FB, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_FC, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_FD, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	{ GE_CMD_UNKNOWN_FE, FLAG_EXECUTE, 0, &GPUCommon::Execute_Unknown },
	// Appears to be debugging related or something?  Hit a lot in GoW.
	{ GE_CMD_NOP_FF, 0 },
};

GPUCommonHW::GPUCommonHW(GraphicsContext *gfxCtx, Draw::DrawContext *draw) : GPUCommon(gfxCtx, draw) {
	memset(cmdInfo_, 0, sizeof(cmdInfo_));

	// Convert the command table to a faster format, and check for dupes.
	std::set<u8> dupeCheck;
	for (size_t i = 0; i < ARRAY_SIZE(commonCommandTable); i++) {
		const u8 cmd = commonCommandTable[i].cmd;
		if (dupeCheck.find(cmd) != dupeCheck.end()) {
			ERROR_LOG(Log::G3D, "Command table Dupe: %02x (%i)", (int)cmd, (int)cmd);
		} else {
			dupeCheck.insert(cmd);
		}
		cmdInfo_[cmd].flags |= (uint64_t)commonCommandTable[i].flags | (commonCommandTable[i].dirty << 8);
		cmdInfo_[cmd].func = commonCommandTable[i].func;
		if ((cmdInfo_[cmd].flags & (FLAG_EXECUTE | FLAG_EXECUTEONCHANGE)) && !cmdInfo_[cmd].func) {
			// Can't have FLAG_EXECUTE commands without a function pointer to execute.
			Crash();
		}
	}
	// Find commands missing from the table.
	for (int i = 0; i < 0xEF; i++) {
		if (dupeCheck.find((u8)i) == dupeCheck.end()) {
			ERROR_LOG(Log::G3D, "Command missing from table: %02x (%i)", i, i);
		}
	}

	UpdateCmdInfo();
	UpdateMSAALevel(draw);
}

GPUCommonHW::~GPUCommonHW() {
	// Clear features so they're not visible in system info.
	gstate_c.SetUseFlags(0);

	// Delete the various common managers.
	framebufferManager_->DestroyAllFBOs();
	delete framebufferManager_;
	delete textureCache_;
	if (shaderManager_) {
		shaderManager_->ClearShaders();
		delete shaderManager_;
	}
}

// Called once per frame. Might also get called during the pause screen
// if "transparent".
void GPUCommonHW::CheckConfigChanged() {
	if (configChanged_) {
		ClearCacheNextFrame();
		gstate_c.SetUseFlags(CheckGPUFeatures());
		drawEngineCommon_->NotifyConfigChanged();
		textureCache_->NotifyConfigChanged();
		configChanged_ = false;
	}
}

void GPUCommonHW::CheckDisplayResized() {
	if (displayResized_) {
		framebufferManager_->NotifyDisplayResized();
		displayResized_ = false;
	}
}

void GPUCommonHW::CheckRenderResized() {
	if (renderResized_) {
		framebufferManager_->NotifyRenderResized(msaaLevel_);
		renderResized_ = false;
	}
}

// Call at the END of the GPU implementation's DeviceLost
void GPUCommonHW::DeviceLost() {
	framebufferManager_->DeviceLost();
	draw_ = nullptr;
	textureCache_->Clear(false);
	textureCache_->DeviceLost();
	shaderManager_->DeviceLost();
	drawEngineCommon_->DeviceLost();
}

// Call at the start of the GPU implementation's DeviceRestore
void GPUCommonHW::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	displayResized_ = true;  // re-check display bounds.
	renderResized_ = true;
	framebufferManager_->DeviceRestore(draw_);
	textureCache_->DeviceRestore(draw_);
	shaderManager_->DeviceRestore(draw_);
	drawEngineCommon_->DeviceRestore(draw_);

	PPGeSetDrawContext(draw_);

	gstate_c.SetUseFlags(CheckGPUFeatures());
	UpdateCmdInfo();
}

void GPUCommonHW::UpdateCmdInfo() {
	if (g_Config.bSoftwareSkinning) {
		cmdInfo_[GE_CMD_VERTEXTYPE].flags &= ~FLAG_FLUSHBEFOREONCHANGE;
		cmdInfo_[GE_CMD_VERTEXTYPE].func = &GPUCommonHW::Execute_VertexTypeSkinning;
	} else {
		cmdInfo_[GE_CMD_VERTEXTYPE].flags |= FLAG_FLUSHBEFOREONCHANGE;
		cmdInfo_[GE_CMD_VERTEXTYPE].func = &GPUCommonHW::Execute_VertexType;
	}

	// Reconfigure for light ubershader or not.
	for (int i = 0; i < 4; i++) {
		if (gstate_c.Use(GPU_USE_LIGHT_UBERSHADER)) {
			cmdInfo_[GE_CMD_LIGHTENABLE0 + i].AddDirty(DIRTY_LIGHT_CONTROL);
			cmdInfo_[GE_CMD_LIGHTTYPE0 + i].AddDirty(DIRTY_LIGHT_CONTROL);
		} else {
			cmdInfo_[GE_CMD_LIGHTENABLE0 + i].RemoveDirty(DIRTY_LIGHT_CONTROL);
			cmdInfo_[GE_CMD_LIGHTENABLE0 + i].AddDirty(DIRTY_VERTEXSHADER_STATE);
			cmdInfo_[GE_CMD_LIGHTTYPE0 + i].RemoveDirty(DIRTY_LIGHT_CONTROL);
			cmdInfo_[GE_CMD_LIGHTTYPE0 + i].AddDirty(DIRTY_VERTEXSHADER_STATE);
		}
	}

	if (gstate_c.Use(GPU_USE_LIGHT_UBERSHADER)) {
		cmdInfo_[GE_CMD_MATERIALUPDATE].RemoveDirty(DIRTY_VERTEXSHADER_STATE);
		cmdInfo_[GE_CMD_MATERIALUPDATE].AddDirty(DIRTY_LIGHT_CONTROL);
	} else {
		cmdInfo_[GE_CMD_MATERIALUPDATE].RemoveDirty(DIRTY_LIGHT_CONTROL);
		cmdInfo_[GE_CMD_MATERIALUPDATE].AddDirty(DIRTY_VERTEXSHADER_STATE);
	}

	if (gstate_c.Use(GPU_USE_FRAGMENT_UBERSHADER)) {
		// Texfunc controls both texalpha and doubling. The rest is not dynamic yet so can't remove fragment shader dirtying.
		cmdInfo_[GE_CMD_TEXFUNC].AddDirty(DIRTY_TEX_ALPHA_MUL);
	} else {
		cmdInfo_[GE_CMD_TEXFUNC].RemoveDirty(DIRTY_TEX_ALPHA_MUL);
	}
	BuildGeTables();
}

void GPUCommonHW::BeginHostFrame() {
	GPUCommon::BeginHostFrame();
	if (drawEngineCommon_->EverUsedExactEqualDepth() && !sawExactEqualDepth_) {
		sawExactEqualDepth_ = true;
		gstate_c.SetUseFlags(CheckGPUFeatures());
	}
}

void GPUCommonHW::SetDisplayFramebuffer(u32 framebuf, u32 stride, GEBufferFormat format) {
	framebufferManager_->SetDisplayFramebuffer(framebuf, stride, format);
}

void GPUCommonHW::CheckFlushOp(int cmd, u32 diff) {
	const u8 cmdFlags = cmdInfo_[cmd].flags;
	if (diff && (cmdFlags & FLAG_FLUSHBEFOREONCHANGE)) {
		if (dumpThisFrame_) {
			NOTICE_LOG(Log::G3D, "================ FLUSH ================");
		}
		drawEngineCommon_->Flush();
	}
}

void GPUCommonHW::PreExecuteOp(u32 op, u32 diff) {
	CheckFlushOp(op >> 24, diff);
}

void GPUCommonHW::PrepareCopyDisplayToOutput() {
	drawEngineCommon_->FlushQueuedDepth();
	// Flush anything left over.
	drawEngineCommon_->Flush();

	shaderManager_->DirtyLastShader();

	// after this, render pass is active.
	framebufferManager_->PrepareCopyDisplayToOutput(curFramebufferDirty_);
}

void GPUCommonHW::CopyDisplayToOutput() {
	framebufferManager_->CopyDisplayToOutput();
	curFramebufferDirty_ = false;
}

bool GPUCommonHW::PresentedThisFrame() const {
	return framebufferManager_->PresentedThisFrame();
}

void GPUCommonHW::DoState(PointerWrap &p) {
	GPUCommon::DoState(p);

	// TODO: Some of these things may not be necessary.
	// None of these are necessary when saving.
	if (p.mode == p.MODE_READ) {
		textureCache_->Clear(true);

		gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		framebufferManager_->DestroyAllFBOs();
	}
}

void GPUCommonHW::ClearCacheNextFrame() {
	textureCache_->ClearNextFrame();
}

// Needs to be called on GPU thread, not reporting thread.

u32 GPUCommonHW::CheckGPUFeatures() const {
	u32 features = 0;
	if (draw_->GetDeviceCaps().logicOpSupported) {
		features |= GPU_USE_LOGIC_OP;
	}
	if (draw_->GetDeviceCaps().anisoSupported) {
		features |= GPU_USE_ANISOTROPY;
	}
	if (draw_->GetDeviceCaps().textureNPOTFullySupported) {
		features |= GPU_USE_TEXTURE_NPOT;
	}
	if (draw_->GetDeviceCaps().dualSourceBlend) {
		if (false || !draw_->GetBugs().Has(Draw::Bugs::DUAL_SOURCE_BLENDING_BROKEN)) {
			features |= GPU_USE_DUALSOURCE_BLEND;
		}
	}
	if (draw_->GetDeviceCaps().blendMinMaxSupported) {
		features |= GPU_USE_BLEND_MINMAX;
	}

	if (draw_->GetDeviceCaps().clipDistanceSupported) {
		features |= GPU_USE_CLIP_DISTANCE;
	}

	if (draw_->GetDeviceCaps().cullDistanceSupported) {
		features |= GPU_USE_CULL_DISTANCE;
	}

	if (draw_->GetDeviceCaps().textureDepthSupported) {
		features |= GPU_USE_DEPTH_TEXTURE;
	}

	// Deadstation Portable: never depth clamp. The Brick's GPU (PowerVR GE8300) runs every draw with depth clamp as if without
	// early depth tests or hidden surface removal: God of War's frame replayed (local/brick/fillbench) 6.1 -> 20.2 ms with it,
	// flat shaded, 7.2 -> 36.9 ms with its own shader. Without it, accurate depth's centred range, as on GPUs that have no
	// clamp: God of War at 960x544 23 -> 44 vblanks/s, LEGO 34 -> 40; geometry past the far plane is clipped instead of
	// clamped (one far line in Burnout).
	if (draw_->GetDeviceCaps().depthClampSupported) {
		features |= GPU_USE_ACCURATE_DEPTH;
	}

	bool canClipOrCull = draw_->GetDeviceCaps().clipDistanceSupported || draw_->GetDeviceCaps().cullDistanceSupported;
	bool canDiscardVertex = !draw_->GetBugs().Has(Draw::Bugs::BROKEN_NAN_IN_CONDITIONAL);
	if ((canClipOrCull || canDiscardVertex) && true) {
		// We'll dynamically use the parts that are supported, to reduce artifacts as much as possible.
		features |= GPU_USE_VS_RANGE_CULLING;
	}
	// A division, compares and four more outputs for every vertex, for what these games don't draw anyway.
	if (PSP_CoreParameter().compat.flags().NoVertexRangeCulling)
		features &= ~(GPU_USE_VS_RANGE_CULLING | GPU_USE_CLIP_DISTANCE | GPU_USE_CULL_DISTANCE);

	if (draw_->GetDeviceCaps().framebufferFetchSupported) {
		features |= GPU_USE_FRAMEBUFFER_FETCH;
		features |= GPU_USE_SHADER_BLENDING;   // doesn't matter if we are buffered or not here.
	} else {
		if (!g_Config.bSkipBufferEffects) {
			features |= GPU_USE_SHADER_BLENDING;
		}
	}

	if (draw_->GetShaderLanguageDesc().bitwiseOps && g_Config.bUberShaderVertex) {
		features |= GPU_USE_LIGHT_UBERSHADER;
	}

	if (PSP_CoreParameter().compat.flags().ClearToRAM) {
		features |= GPU_USE_CLEAR_RAM_HACK;
	}

	// Even without depth clamp, force accurate depth on for some games that break without it.
	if (PSP_CoreParameter().compat.flags().DepthRangeHack) {
		features |= GPU_USE_ACCURATE_DEPTH;
	}

	// Some backends will turn this off again in the calling function.
	if (g_Config.bUberShaderFragment) {
		features |= GPU_USE_FRAGMENT_UBERSHADER;
	}

	return features;
}

u32 GPUCommonHW::CheckGPUFeaturesLate(u32 features) const {
	// If we already have a 16-bit depth buffer, we don't need to round.
	bool prefer24 = draw_->GetDeviceCaps().preferredDepthBufferFormat == Draw::DataFormat::D24_S8;
	bool prefer16 = draw_->GetDeviceCaps().preferredDepthBufferFormat == Draw::DataFormat::D16;
	if (!prefer16) {
		if (sawExactEqualDepth_ && (features & GPU_USE_ACCURATE_DEPTH) != 0 && !PSP_CoreParameter().compat.flags().ForceMaxDepthResolution) {
			// Exact equal tests tend to have issues unless we use the PSP's depth range.
			// We use 24-bit depth virtually everwhere, the fallback is just for safety.
			if (prefer24)
				features |= GPU_SCALE_DEPTH_FROM_24BIT_TO_16BIT;
			else
				features |= GPU_ROUND_FRAGMENT_DEPTH_TO_16BIT;
		} else if (!g_Config.bHighQualityDepth && (features & GPU_USE_ACCURATE_DEPTH) != 0) {
			features |= GPU_SCALE_DEPTH_FROM_24BIT_TO_16BIT;
		} else if (PSP_CoreParameter().compat.flags().PixelDepthRounding) {
			if (prefer24 && (features & GPU_USE_ACCURATE_DEPTH) != 0) {
				// Here we can simulate a 16 bit depth buffer by scaling.
				// Note that the depth buffer is fixed point, not floating, so dividing by 256 is pretty good.
				features |= GPU_SCALE_DEPTH_FROM_24BIT_TO_16BIT;
			} else {
				// Use fragment rounding on where available otherwise.
				features |= GPU_ROUND_FRAGMENT_DEPTH_TO_16BIT;
			}
		} else if (PSP_CoreParameter().compat.flags().VertexDepthRounding) {
			features |= GPU_ROUND_DEPTH_TO_16BIT;
		}
	}

	return features;
}

void GPUCommonHW::UpdateMSAALevel(Draw::DrawContext *draw) {
	int level = g_Config.iMultiSampleLevel;
	if (draw && draw->GetDeviceCaps().multiSampleLevelsMask & (1 << level)) {
		msaaLevel_ = level;
	} else {
		// Didn't support the configured level, so revert to 0.
		msaaLevel_ = 0;
	}
}










void GPUCommonHW::CheckDepthUsage(VirtualFramebuffer *vfb) {
	if (!gstate_c.usingDepth) {
		bool isReadingDepth = false;
		bool isClearingDepth = false;
		bool isWritingDepth = false;
		if (gstate.isModeClear()) {
			isClearingDepth = gstate.isClearModeDepthMask();
			isWritingDepth = isClearingDepth;
		} else if (gstate.isDepthTestEnabled()) {
			isWritingDepth = gstate.isDepthWriteEnabled();
			isReadingDepth = gstate.getDepthTestFunction() > GE_COMP_ALWAYS;
		}

		if (isWritingDepth || isReadingDepth) {
			gstate_c.usingDepth = true;
			gstate_c.clearingDepth = isClearingDepth;
			vfb->last_frame_depth_render = gpuStats.numFlips;
			if (isWritingDepth) {
				vfb->last_frame_depth_updated = gpuStats.numFlips;
			}
			framebufferManager_->SetDepthFrameBuffer(isClearingDepth);
		}
	}
}

void GPUCommonHW::InvalidateCache(u32 addr, int size, GPUInvalidationType type) {
	if (size > 0)
		textureCache_->Invalidate(addr, size, type);
	else
		textureCache_->InvalidateAll(type);

	if (type != GPU_INVALIDATE_ALL && framebufferManager_->MayIntersectFramebufferColor(addr)) {
		// Vempire invalidates (with writeback) after drawing, but before blitting.
		// TODO: Investigate whether we can get this to work some other way.
		if (type == GPU_INVALIDATE_SAFE) {
			framebufferManager_->UpdateFromMemory(addr, size);
		}
	}
}

bool GPUCommonHW::FramebufferDirty() {
	if (!framebufferManager_)
		return true;
	VirtualFramebuffer *vfb = framebufferManager_->GetDisplayVFB();
	if (vfb) {
		bool dirty = vfb->dirtyAfterDisplay;
		vfb->dirtyAfterDisplay = false;
		return dirty;
	}
	return true;
}

bool GPUCommonHW::FramebufferReallyDirty() {
	if (!framebufferManager_)
		return true;
	VirtualFramebuffer *vfb = framebufferManager_->GetDisplayVFB();
	if (vfb) {
		bool dirty = vfb->reallyDirtyAfterDisplay;
		vfb->reallyDirtyAfterDisplay = false;
		return dirty;
	}
	return true;
}

void GPUCommonHW::ExecuteOp(u32 op, u32 diff) {
	const u8 cmd = op >> 24;
	const CommandInfo info = cmdInfo_[cmd];
	const u8 cmdFlags = info.flags;
	if ((cmdFlags & FLAG_EXECUTE) || (diff && (cmdFlags & FLAG_EXECUTEONCHANGE))) {
		(this->*info.func)(op, diff);
	} else if (diff) {
		uint64_t dirty = info.flags >> 8;
		if (dirty)
			gstate_c.Dirty(dirty);
	}
}

void GPUCommonHW::FastRunLoop(DisplayList &list) {

	if (!Memory::IsValidAddress(list.pc)) {
		// We're having some serious problems here, just bail and try to limp along and not crash the app.
		downcount = 0;
		return;
	}

	// Deadstation Portable: the loop is hand-written ARM64 (GeRunLoopArm64.cpp), with the rules the C++ loop here had.
	const GeLoopArgs args{ this, &list.pc, Memory::base, gstate.cmdmem, geHandlers_, (const u8 *)&geTable_, &gstate_c.dirty,
		drawEngineCommon_->NumDrawCallsPtr(), &downcount, &GeFlushThunk };
	GeRunLoop(&args);
}

void GPUCommonHW::Execute_VertexType(u32 op, u32 diff) {
	if (diff) {
		// TODO: We only need to dirty vshader-state here if the output format will be different.
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE);

		if (diff & GE_VTYPE_THROUGH_MASK) {
			// Switching between through and non-through, we need to invalidate a bunch of stuff.
			gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE | DIRTY_CULLRANGE);
		}
	}
}

void GPUCommonHW::Execute_VertexTypeSkinning(u32 op, u32 diff) {
	// Don't flush when weight count changes.
	if (diff & ~GE_VTYPE_WEIGHTCOUNT_MASK) {
		// Restore and flush
		gstate.vertType ^= diff;
		Flush();
		gstate.vertType ^= diff;
		// In this case, we may be doing weights and morphs.
		// Update any bone matrix uniforms so it uses them correctly.
		if ((op & GE_VTYPE_MORPHCOUNT_MASK) != 0) {
			gstate_c.Dirty(gstate_c.deferredVertTypeDirty);
			gstate_c.deferredVertTypeDirty = 0;
		}
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE);
	}
	if (diff & GE_VTYPE_THROUGH_MASK)
		gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE | DIRTY_CULLRANGE);
}

// Vertices and indices that have just come through VRAM are read where they came from (VramAlias.h).
static inline void ReadThroughUploads(DrawEngineCommon *engine, const void *&verts, const void *&inds, u32 vertexType, int count, const VertexDecoder *decoder) {
	if (!g_vramAlias.pending())
		return;
	if (inds) {
		const int indexShift = ((vertexType & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
		inds = Memory::GetPointerUnchecked(g_vramAlias.readable(gstate_c.indexAddr, count << indexShift));
	}
	if (Memory::IsVRAMAddress(gstate_c.vertexAddr)) {
		u32 vertices = count;
		if (inds) {
			u16 lower = 0, upper = 0;
			engine->IndexBounds(inds, count, vertexType, &lower, &upper);
			vertices = upper + 1;
		}
		verts = Memory::GetPointerUnchecked(g_vramAlias.readable(gstate_c.vertexAddr, vertices * decoder->VertexSize()));
	}
}

void GPUCommonHW::Execute_Prim(u32 op, u32 diff) {
	// This drives all drawing. All other state we just buffer up, then we apply it only
	// when it's time to draw. As most PSP games set state redundantly ALL THE TIME, this is a huge optimization.


	if (immCount_ != 0)
		FlushImm();

	// Upper bits are ignored.
	const GEPrimitiveType prim = static_cast<GEPrimitiveType>((op >> 16) & 7);
	SetDrawType(DRAW_PRIM, prim);

	// Discard AA lines as we can't do anything that makes sense with these anyway. The SW plugin might, though.
	if (gstate.isAntiAliasEnabled()) {
		// Heuristic derived from discussions in #6483 and #12588.
		// Discard AA lines in Persona 3 Portable, DOA Paradise and Summon Night 5, while still keeping AA lines in Echochrome.
		if ((prim == GE_PRIM_LINE_STRIP || prim == GE_PRIM_LINES) && gstate.getTextureFunction() == GE_TEXFUNC_REPLACE)
			return;
	}

	// Update cached framebuffer format.
	// We store it in the cache so it can be modified for blue-to-alpha, next.
	gstate_c.framebufFormat = gstate.FrameBufFormat();

	if (!Memory::IsValidAddress(gstate_c.vertexAddr)) {
		ERROR_LOG(Log::G3D, "Bad vertex address %08x!", gstate_c.vertexAddr);
		return;
	}

	// See the documentation for gstate_c.blueToAlpha.
	bool blueToAlpha = false;
	if (PSP_CoreParameter().compat.flags().BlueToAlpha) {
		if (gstate_c.framebufFormat == GEBufferFormat::GE_FORMAT_565 && gstate.getColorMask() == 0x0FFFFF && !gstate.isLogicOpEnabled()) {
			blueToAlpha = true;
			gstate_c.framebufFormat = GEBufferFormat::GE_FORMAT_4444;
		}
		if (blueToAlpha != gstate_c.blueToAlpha) {
			gstate_c.blueToAlpha = blueToAlpha;
			gstate_c.Dirty(DIRTY_FRAMEBUF | DIRTY_FRAGMENTSHADER_STATE | DIRTY_BLEND_STATE);
		}
	}

	if (PSP_CoreParameter().compat.flags().SplitFramebufferMargin) {
		switch (gstate.vertType & 0xFFFFFF) {
		case 0x00800102:  // through, u16 uv, u16 pos (used for the framebuffer effect in-game)
		case 0x0080011c:  // through, 8888 color, s16 pos (used for clearing in the margin of the title screen)
		case 0x00000183:  // float uv, float pos (used for drawing in the margin of the title screen)
			// Need to re-check the framebuffer every one of these draws, to update the split if needed.
			gstate_c.Dirty(DIRTY_FRAMEBUF);
		}
	}

	if (gstate_c.effectSkipOffscreen && (u32)gpuStats.numFlips - gstate_c.effectFlip <= 2) {
		const u32 target = gstate.getFrameBufRawAddress() & 0x00FFFFFF;
		if (target != gstate_c.effectSceneA && target != gstate_c.effectSceneB && !(target >= gstate_c.effectBuffer && target < gstate_c.effectBuffer + 0x800)) {
			const u32 skipCount = op & 0xFFFF;
			const u32 skipType = gstate.vertType;
			VertexDecoder *skipDecoder = drawEngineCommon_->GetVertexDecoder(GetVertTypeID(skipType, gstate.getUVGenMode(), g_Config.bSoftwareSkinning));
			AdvanceVerts(skipType, skipCount, skipCount * skipDecoder->VertexSize());
			return;
		}
	}

	// This also makes skipping drawing very effective.
	bool changed;
	VirtualFramebuffer *const vfb = framebufferManager_->SetRenderFrameBuffer(gstate_c.IsDirty(DIRTY_FRAMEBUF), gstate_c.skipDrawReason, &changed);
	if (blueToAlpha) {
		vfb->usageFlags |= FB_USAGE_BLUE_TO_ALPHA;
	}

	if (changed) {
		drawEngineCommon_->FlushQueuedDepth();
	}

	if (gstate_c.dirty & DIRTY_VERTEXSHADER_STATE) {
		vertexCost_ = EstimatePerVertexCost();
	}

	const u32 count = op & 0xFFFF;
	// Must check this after SetRenderFrameBuffer so we know SKIPDRAW_NON_DISPLAYED_FB.
	if (gstate_c.skipDrawReason & (SKIPDRAW_SKIPFRAME | SKIPDRAW_NON_DISPLAYED_FB)) {
		// Rough estimate, not sure what's correct.
		cyclesExecuted += vertexCost_ * count;
		if (gstate.isModeClear()) {
			gpuStats.numClears++;
		}
		return;
	}

	CheckDepthUsage(vfb);

	const void *verts = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
	const void *inds = nullptr;

	const bool isTriangle = IsTrianglePrim(prim);

	bool canExtend = isTriangle;
	u32 vertexType = gstate.vertType;
	if ((vertexType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		u32 indexAddr = gstate_c.indexAddr;
		const int indexShift = ((vertexType & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
		if (!Memory::IsValidRange(indexAddr, count << indexShift)) {
			ERROR_LOG(Log::G3D, "Bad index address %08x (%d)!", indexAddr, count);
			return;
		}
		inds = Memory::GetPointerUnchecked(indexAddr);
		canExtend = false;
	}

	gstate_c.UpdateUVScaleOffset();

	// cull mode
	int cullMode = gstate.getCullMode();

	uint32_t vertTypeID = GetVertTypeID(vertexType, gstate.getUVGenMode(), g_Config.bSoftwareSkinning);
	VertexDecoder *decoder = drawEngineCommon_->GetVertexDecoder(vertTypeID);
	ReadThroughUploads(drawEngineCommon_, verts, inds, vertexType, count, decoder);

	// Through mode early-out for simple float 2D draws, like in Fate Extra CCC (very beneficial there due to avoiding texture loads)
	if ((vertexType & (GE_VTYPE_THROUGH_MASK | GE_VTYPE_POS_MASK | GE_VTYPE_IDX_MASK)) == (GE_VTYPE_THROUGH_MASK | GE_VTYPE_POS_FLOAT | GE_VTYPE_IDX_NONE)) {
		int bytesRead = 0;
		if (!drawEngineCommon_->TestBoundingBoxThrough(verts, count, decoder, vertexType, &bytesRead)) {
			gpuStats.numCulledDraws++;
			int cycles = vertexCost_ * count;
			gpuStats.vertexGPUCycles += cycles;
			cyclesExecuted += cycles;
			// NOTE! We still have to advance vertex pointers!
			gstate_c.vertexAddr += bytesRead;   // We know from the above check that it's not an indexed draw.
			return;
		}
	}

#define MAX_CULL_CHECK_COUNT 6

// For now, turn off culling on platforms where we don't have SIMD bounding box tests, like RISC-V.

#define PASSES_CULLING ((vertexType & (GE_VTYPE_THROUGH_MASK | GE_VTYPE_MORPHCOUNT_MASK | GE_VTYPE_WEIGHT_MASK | GE_VTYPE_IDX_MASK)) || count > MAX_CULL_CHECK_COUNT)


	// If certain conditions are true, do frustum culling.
	bool passCulling = PASSES_CULLING;
	if (!passCulling) {
		// Do software culling.
		if (drawEngineCommon_->TestBoundingBoxFast(verts, count, decoder, vertexType)) {
			passCulling = true;
		} else {
			gpuStats.numCulledDraws++;
		}
	}

	int bytesRead = 0;

	// If the first one in a batch passes, let's assume the whole batch passes.
	// Cuts down on checking, while not losing that much efficiency.
	bool onePassed = false;
	if (passCulling) {
		if (!drawEngineCommon_->SubmitPrim(verts, inds, prim, count, decoder, vertTypeID, true, &bytesRead)) {
			canExtend = false;
		}
		onePassed = true;
	} else {
		// Still need to advance bytesRead.
		drawEngineCommon_->SkipPrim(prim, count, decoder, vertTypeID, &bytesRead);
		canExtend = false;
	}

	// After drawing, we advance the vertexAddr (when non indexed) or indexAddr (when indexed).
	// Some games rely on this, they don't bother reloading VADDR and IADDR.
	// The VADDR/IADDR registers are NOT updated.
	AdvanceVerts(vertexType, count, bytesRead);

	int totalVertCount = count;

	// PRIMs are often followed by more PRIMs. Save some work and submit them immediately.
	const u32_le *start = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
	const u32_le *src = start;
	const u32_le *stall = currentList->stall ? (const u32_le *)Memory::GetPointerUnchecked(currentList->stall) : 0;

	// Optimized submission of sequences of PRIM. Allows us to avoid going through all the mess
	// above for each one. This can be expanded to support additional games that intersperse
	// PRIM commands with other commands. A special case is Earth Defence Force 2 that changes culling mode
	// between each prim, we just change the triangle winding right here to still be able to join draw calls.

	const uint32_t vtypeCheckMask = g_Config.bSoftwareSkinning ? (~GE_VTYPE_WEIGHTCOUNT_MASK) : 0xFFFFFFFF;

	if (!useFastRunLoop_)
		goto bail;  // we're either recording or stepping.

	while (src != stall) {
		uint32_t data = *src;
		switch (data >> 24) {
		case GE_CMD_PRIM:
		{
			GEPrimitiveType newPrim = static_cast<GEPrimitiveType>((data >> 16) & 7);
			if (IsTrianglePrim(newPrim) != isTriangle)
				goto bail;  // Can't join over this boundary. Might as well exit and get this on the next time around.
			// TODO: more efficient updating of verts/inds

			u32 count = data & 0xFFFF;
			bool clockwise = !gstate.isCullEnabled() || gstate.getCullMode() == cullMode;
			if (canExtend) {
				// Non-indexed draws can be cheaply merged if vertexAddr hasn't changed, that means the vertices
				// are consecutive in memory. We also ignore culling here.
				_dbg_assert_((vertexType & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_NONE);
				int commandsExecuted = drawEngineCommon_->ExtendNonIndexedPrim(src, stall, decoder, vertTypeID, clockwise, &bytesRead, isTriangle);
				if (!commandsExecuted) {
					goto bail;
				}
				src += commandsExecuted - 1;
				gstate_c.vertexAddr += bytesRead;
				totalVertCount += count;
				break;
			}

			verts = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
			inds = nullptr;
			if ((vertexType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
				const u32 indexAddr = gstate_c.indexAddr;
				const int indexShift = ((vertexType & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
				if (!Memory::IsValidRange(gstate_c.indexAddr, count << indexShift)) {
					// Bad index range. Let's give up the fast loop.
					goto bail;
				}
				inds = Memory::GetPointerUnchecked(indexAddr);
			} else {
				// We can extend again after submitting a normal draw.
				canExtend = isTriangle;
			}
			ReadThroughUploads(drawEngineCommon_, verts, inds, vertexType, count, decoder);

			bool passCulling = onePassed || PASSES_CULLING;
			if (!passCulling) {
				// Do software culling.
				_dbg_assert_((vertexType & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_NONE);
				if (drawEngineCommon_->TestBoundingBoxFast(verts, count, decoder, vertexType)) {
					passCulling = true;
				} else {
					gpuStats.numCulledDraws++;
				}
			}
			if (passCulling) {
				if (!drawEngineCommon_->SubmitPrim(verts, inds, newPrim, count, decoder, vertTypeID, clockwise, &bytesRead)) {
					canExtend = false;
				}
				// As soon as one passes, assume we don't need to check the rest of this batch.
				onePassed = true;
			} else {
				// Still need to advance bytesRead.
				drawEngineCommon_->SkipPrim(newPrim, count, decoder, vertTypeID, &bytesRead);
				canExtend = false;
			}
			AdvanceVerts(vertexType, count, bytesRead);
			totalVertCount += count;
			break;
		}
		case GE_CMD_VERTEXTYPE:
		{
			uint32_t diff = data ^ vertexType;
			// don't mask upper bits, vertexType is unmasked
			if (diff) {
				if (diff & vtypeCheckMask)
					goto bail;
				drawEngineCommon_->FlushSkin();
				canExtend = false;  // TODO: Might support extending between some vertex types in the future.
				vertexType = data;
				vertTypeID = GetVertTypeID(vertexType, gstate.getUVGenMode(), g_Config.bSoftwareSkinning);
				decoder = drawEngineCommon_->GetVertexDecoder(vertTypeID);
			}
			break;
		}
		case GE_CMD_VADDR:
		{
			gstate.cmdmem[GE_CMD_VADDR] = data;
			uint32_t newAddr = gstate_c.getRelativeAddress(data & 0x00FFFFFF);
			if (gstate_c.vertexAddr != newAddr) {
				canExtend = false;
				gstate_c.vertexAddr = newAddr;
			}
			break;
		}
		case GE_CMD_IADDR:
			gstate.cmdmem[GE_CMD_IADDR] = data;
			gstate_c.indexAddr = gstate_c.getRelativeAddress(data & 0x00FFFFFF);
			break;
		case GE_CMD_OFFSETADDR:
			gstate.cmdmem[GE_CMD_OFFSETADDR] = data;
			gstate_c.offsetAddr = data << 8;
			break;
		case GE_CMD_BASE:
			gstate.cmdmem[GE_CMD_BASE] = data;
			break;
		case GE_CMD_CULLFACEENABLE:
			// Earth Defence Force 2
			if (gstate.cmdmem[GE_CMD_CULLFACEENABLE] != data) {
				goto bail;
			}
			break;
		case GE_CMD_CULL:
			// flip face by indices for triangles
			cullMode = data & 1;
			break;
		case GE_CMD_TEXFLUSH:
		case GE_CMD_NOP:
		case GE_CMD_NOP_FF:
			gstate.cmdmem[data >> 24] = data;
			break;
		case GE_CMD_BONEMATRIXNUMBER:
			gstate.cmdmem[GE_CMD_BONEMATRIXNUMBER] = data;
			break;
		case GE_CMD_TEXSCALEU:
			// We don't "dirty-check" - we could avoid getFloat24 and setting canExtend=false, but usually
			// when texscale commands are in line with the prims like this, they actually have an effect
			// and requires us to stop extending strips anyway.
			gstate.cmdmem[GE_CMD_TEXSCALEU] = data;
			gstate_c.uv.uScale = getFloat24(data);
			canExtend = false;
			break;
		case GE_CMD_TEXSCALEV:
			gstate.cmdmem[GE_CMD_TEXSCALEV] = data;
			gstate_c.uv.vScale = getFloat24(data);
			canExtend = false;
			break;
		case GE_CMD_TEXOFFSETU:
			gstate.cmdmem[GE_CMD_TEXOFFSETU] = data;
			gstate_c.uv.uOff = getFloat24(data);
			canExtend = false;
			break;
		case GE_CMD_TEXOFFSETV:
			gstate.cmdmem[GE_CMD_TEXOFFSETV] = data;
			gstate_c.uv.vOff = getFloat24(data);
			canExtend = false;
			break;
		case GE_CMD_TEXLEVEL:
			// Same Gran Turismo hack from Execute_TexLevel
			if ((data & 3) != GE_TEXLEVEL_MODE_AUTO && (0x00FF0000 & data) != 0) {
				goto bail;
			}
			gstate.cmdmem[GE_CMD_TEXLEVEL] = data;
			break;
		case GE_CMD_CALL:
		{
			// A bone matrix probably. If not we bail.
			const u32 target = gstate_c.getRelativeAddress(data & 0x00FFFFFC);
			if ((Memory::ReadUnchecked_U32(target) >> 24) == GE_CMD_BONEMATRIXDATA &&
				(Memory::ReadUnchecked_U32(target + 11 * 4) >> 24) == GE_CMD_BONEMATRIXDATA &&
				(Memory::ReadUnchecked_U32(target + 12 * 4) >> 24) == GE_CMD_RET &&
				(target > currentList->stall || target + 12 * 4 < currentList->stall) &&
				(gstate.boneMatrixNumber & 0x00FFFFFF) <= 96 - 12) {
				drawEngineCommon_->FlushSkin();
				canExtend = false;
				FastLoadBoneMatrix(target);
			} else {
				goto bail;
			}
			break;
		}

		// Keep going if these commands don't change state.
		case GE_CMD_TEXBUFWIDTH0:
		case GE_CMD_TEXADDR0:
			if (data != gstate.cmdmem[data >> 24])
				goto bail;
			break;

		default:
			// All other commands might need a flush or something, stop this inner loop.
			goto bail;
		}
		src++;
	}

bail:
	drawEngineCommon_->FlushSkin();
	gstate.cmdmem[GE_CMD_VERTEXTYPE] = vertexType;
	const int cmdCount = src - start;
	// Skip over the commands we just read out manually.
	if (cmdCount > 0) {
		UpdatePC(currentList->pc, currentList->pc + cmdCount * 4);
		currentList->pc += cmdCount * 4;
		// flush back cull mode
		if (cullMode != gstate.getCullMode()) {
			// We rewrote everything to the old cull mode, so flush first.
			drawEngineCommon_->Flush();

			// Now update things for next time.
			gstate.cmdmem[GE_CMD_CULL] ^= 1;
			gstate_c.Dirty(DIRTY_RASTER_STATE);
		}
	}

	int cycles = vertexCost_ * totalVertCount;
	gpuStats.vertexGPUCycles += cycles;

	if (!PSP_CoreParameter().compat.flags().FastEmulatedGPU) {
		cyclesExecuted += cycles;
	}
}

void GPUCommonHW::Execute_Bezier(u32 op, u32 diff) {
	g_vramAlias.materialize();
	// We don't dirty on normal changes anymore as we prescale, but it's needed for splines/bezier.
	gstate_c.framebufFormat = gstate.FrameBufFormat();

	// This also make skipping drawing very effective.
	bool changed;
	VirtualFramebuffer *vfb = framebufferManager_->SetRenderFrameBuffer(gstate_c.IsDirty(DIRTY_FRAMEBUF), gstate_c.skipDrawReason, &changed);
	if (changed) {
		drawEngineCommon_->FlushQueuedDepth();
	}
	if (gstate_c.skipDrawReason & (SKIPDRAW_SKIPFRAME | SKIPDRAW_NON_DISPLAYED_FB)) {
		// TODO: Should this eat some cycles?  Probably yes.  Not sure if important.
		return;
	}

	CheckDepthUsage(vfb);

	if (!Memory::IsValidAddress(gstate_c.vertexAddr)) {
		ERROR_LOG(Log::G3D, "Bad vertex address %08x!", gstate_c.vertexAddr);
		return;
	}

	const void *control_points = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
	const void *indices = NULL;
	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		if (!Memory::IsValidAddress(gstate_c.indexAddr)) {
			ERROR_LOG(Log::G3D, "Bad index address %08x!", gstate_c.indexAddr);
			return;
		}
		indices = Memory::GetPointerUnchecked(gstate_c.indexAddr);
	}

	if (vertTypeIsSkinningEnabled(gstate.vertType)) {
		DEBUG_LOG_REPORT(Log::G3D, "Unusual bezier/spline vtype: %08x, morph: %d, bones: %d", gstate.vertType, (gstate.vertType & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT, vertTypeGetNumBoneWeights(gstate.vertType));
	}

	// Can't flush after setting gstate_c.submitType below since it'll be a mess - it must be done already.
	if (flushOnParams_)
		drawEngineCommon_->Flush();

	Spline::BezierSurface surface;
	surface.tess_u = gstate.getPatchDivisionU();
	surface.tess_v = gstate.getPatchDivisionV();
	surface.num_points_u = op & 0xFF;
	surface.num_points_v = (op >> 8) & 0xFF;
	surface.num_patches_u = (surface.num_points_u - 1) / 3;
	surface.num_patches_v = (surface.num_points_v - 1) / 3;
	surface.primType = gstate.getPatchPrimitiveType();
	surface.patchFacing = gstate.patchfacing & 1;

	SetDrawType(DRAW_BEZIER, PatchPrimToPrim(surface.primType));

	// We need to dirty UVSCALEOFFSET here because we look at the submit type when setting that uniform.
	gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE | DIRTY_UVSCALEOFFSET);
	if (drawEngineCommon_->CanUseHardwareTessellation(surface.primType)) {
		gstate_c.submitType = SubmitType::HW_BEZIER;
		if (gstate_c.spline_num_points_u != surface.num_points_u) {
			gstate_c.Dirty(DIRTY_BEZIERSPLINE);
			gstate_c.spline_num_points_u = surface.num_points_u;
		}
	} else {
		gstate_c.submitType = SubmitType::BEZIER;
	}

	int bytesRead = 0;
	gstate_c.UpdateUVScaleOffset();
	drawEngineCommon_->SubmitCurve(control_points, indices, surface, gstate.vertType, &bytesRead, "bezier");

	gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE | DIRTY_UVSCALEOFFSET);
	gstate_c.submitType = SubmitType::DRAW;

	// After drawing, we advance pointers - see SubmitPrim which does the same.
	const int count = surface.num_points_u * surface.num_points_v;
	AdvanceVerts(gstate.vertType, count, bytesRead);
}

void GPUCommonHW::Execute_Spline(u32 op, u32 diff) {
	g_vramAlias.materialize();
	// We don't dirty on normal changes anymore as we prescale, but it's needed for splines/bezier.
	gstate_c.framebufFormat = gstate.FrameBufFormat();

	// This also make skipping drawing very effective.
	bool changed;
	VirtualFramebuffer *vfb = framebufferManager_->SetRenderFrameBuffer(gstate_c.IsDirty(DIRTY_FRAMEBUF), gstate_c.skipDrawReason, &changed);
	if (changed) {
		drawEngineCommon_->FlushQueuedDepth();
	}
	if (gstate_c.skipDrawReason & (SKIPDRAW_SKIPFRAME | SKIPDRAW_NON_DISPLAYED_FB)) {
		// TODO: Should this eat some cycles?  Probably yes.  Not sure if important.
		return;
	}

	CheckDepthUsage(vfb);

	if (!Memory::IsValidAddress(gstate_c.vertexAddr)) {
		ERROR_LOG(Log::G3D, "Bad vertex address %08x!", gstate_c.vertexAddr);
		return;
	}

	const void *control_points = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
	const void *indices = NULL;
	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		if (!Memory::IsValidAddress(gstate_c.indexAddr)) {
			ERROR_LOG(Log::G3D, "Bad index address %08x!", gstate_c.indexAddr);
			return;
		}
		indices = Memory::GetPointerUnchecked(gstate_c.indexAddr);
	}

	if (vertTypeIsSkinningEnabled(gstate.vertType)) {
		WARN_LOG_ONCE(unusualcurve, Log::G3D, "Unusual bezier/spline vtype: %08x, morph: %d, bones: %d", gstate.vertType, (gstate.vertType & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT, vertTypeGetNumBoneWeights(gstate.vertType));
	}

	// Can't flush after setting gstate_c.submitType below since it'll be a mess - it must be done already.
	if (flushOnParams_)
		drawEngineCommon_->Flush();

	Spline::SplineSurface surface;
	surface.tess_u = gstate.getPatchDivisionU();
	surface.tess_v = gstate.getPatchDivisionV();
	surface.type_u = (op >> 16) & 0x3;
	surface.type_v = (op >> 18) & 0x3;
	surface.num_points_u = op & 0xFF;
	surface.num_points_v = (op >> 8) & 0xFF;
	surface.num_patches_u = surface.num_points_u - 3;
	surface.num_patches_v = surface.num_points_v - 3;
	surface.primType = gstate.getPatchPrimitiveType();
	surface.patchFacing = gstate.patchfacing & 1;

	SetDrawType(DRAW_SPLINE, PatchPrimToPrim(surface.primType));

	// We need to dirty UVSCALEOFFSET here because we look at the submit type when setting that uniform.
	gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE | DIRTY_UVSCALEOFFSET);
	if (drawEngineCommon_->CanUseHardwareTessellation(surface.primType)) {
		gstate_c.submitType = SubmitType::HW_SPLINE;
		if (gstate_c.spline_num_points_u != surface.num_points_u) {
			gstate_c.Dirty(DIRTY_BEZIERSPLINE);
			gstate_c.spline_num_points_u = surface.num_points_u;
		}
	} else {
		gstate_c.submitType = SubmitType::SPLINE;
	}

	int bytesRead = 0;
	gstate_c.UpdateUVScaleOffset();
	drawEngineCommon_->SubmitCurve(control_points, indices, surface, gstate.vertType, &bytesRead, "spline");

	gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE | DIRTY_UVSCALEOFFSET);
	gstate_c.submitType = SubmitType::DRAW;

	// After drawing, we advance pointers - see SubmitPrim which does the same.
	int count = surface.num_points_u * surface.num_points_v;
	AdvanceVerts(gstate.vertType, count, bytesRead);
}

void GPUCommonHW::Execute_BlockTransferStart(u32 op, u32 diff) {
	drawEngineCommon_->FlushQueuedDepth();
	Flush();

	;  // don't include the flush in the profile, would be misleading.

	gstate_c.framebufFormat = gstate.FrameBufFormat();

	// and take appropriate action. This is a block transfer between RAM and VRAM, or vice versa.
	// Can we skip this on SkipDraw?
	DoBlockTransfer(gstate_c.skipDrawReason);
}

void GPUCommonHW::Execute_TexSize0(u32 op, u32 diff) {
	// Render to texture may have overridden the width/height.
	// Don't reset it unless the size is different / the texture has changed.
	if (diff || gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS)) {
		gstate_c.curTextureWidth = gstate.getTextureWidth(0);
		gstate_c.curTextureHeight = gstate.getTextureHeight(0);
		// We will need to reset the texture now.
		gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
	}
}

void GPUCommonHW::Execute_TexLevel(u32 op, u32 diff) {
	// TODO: If you change the rules here, don't forget to update the inner interpreter in Execute_Prim.
	if (diff == 0xFFFFFFFF)
		return;

	gstate.texlevel ^= diff;

	if (diff & 0xFF0000) {
		// Piggyback on this flag for 3D textures.
		gstate_c.Dirty(DIRTY_MIPBIAS);
	}
	if (gstate.getTexLevelMode() != GE_TEXLEVEL_MODE_AUTO && (0x00FF0000 & gstate.texlevel) != 0) {
		Flush();
	}

	gstate.texlevel ^= diff;

	gstate_c.Dirty(DIRTY_TEXTURE_PARAMS | DIRTY_FRAGMENTSHADER_STATE);
}

void GPUCommonHW::Execute_LoadClut(u32 op, u32 diff) {
	gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
	textureCache_->LoadClut(gstate.getClutAddress(), gstate.getClutLoadBytes());
}

void GPUCommonHW::Execute_WorldMtxNum(u32 op, u32 diff) {
	if (!currentList) {
		gstate.worldmtxnum = (GE_CMD_WORLDMATRIXNUMBER << 24) | (op & 0xF);
		return;
	}

	// This is almost always followed by GE_CMD_WORLDMATRIXDATA.
	const u32_le *src = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
	u32 *dst = (u32 *)(gstate.worldMatrix + (op & 0xF));
	const int end = 12 - (op & 0xF);
	int i = 0;

	// We must record the individual data commands while debugRecording_.
	bool fastLoad = end > 0;
	// Stalling in the middle of a matrix would be stupid, I doubt this check is necessary.
	if (currentList->pc < currentList->stall && currentList->pc + end * 4 >= currentList->stall) {
		fastLoad = false;
	}

	if (fastLoad) {
		while ((src[i] >> 24) == GE_CMD_WORLDMATRIXDATA) {
			const u32 newVal = src[i] << 8;
			if (dst[i] != newVal) {
				Flush();
				dst[i] = newVal;
				gstate_c.Dirty(DIRTY_WORLDMATRIX);
			}
			if (++i >= end) {
				break;
			}
		}
	}

	const int count = i;
	gstate.worldmtxnum = (GE_CMD_WORLDMATRIXNUMBER << 24) | ((op & 0xF) + count);

	// Skip over the loaded data, it's done now.
	UpdatePC(currentList->pc, currentList->pc + count * 4);
	currentList->pc += count * 4;
}

void GPUCommonHW::Execute_WorldMtxData(u32 op, u32 diff) {
	// Note: it's uncommon to get here now, see above.
	int num = gstate.worldmtxnum & 0x00FFFFFF;
	u32 newVal = op << 8;
	if (num < 12 && newVal != ((const u32 *)gstate.worldMatrix)[num]) {
		Flush();
		((u32 *)gstate.worldMatrix)[num] = newVal;
		gstate_c.Dirty(DIRTY_WORLDMATRIX);
	}
	num++;
	gstate.worldmtxnum = (GE_CMD_WORLDMATRIXNUMBER << 24) | (num & 0x00FFFFFF);
	gstate.worldmtxdata = GE_CMD_WORLDMATRIXDATA << 24;
}

void GPUCommonHW::Execute_ViewMtxNum(u32 op, u32 diff) {
	if (!currentList) {
		gstate.viewmtxnum = (GE_CMD_VIEWMATRIXNUMBER << 24) | (op & 0xF);
		return;
	}

	// This is almost always followed by GE_CMD_VIEWMATRIXDATA.
	const u32_le *src = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
	u32 *dst = (u32 *)(gstate.viewMatrix + (op & 0xF));
	const int end = 12 - (op & 0xF);
	int i = 0;

	bool fastLoad = end > 0;
	if (currentList->pc < currentList->stall && currentList->pc + end * 4 >= currentList->stall) {
		fastLoad = false;
	}

	if (fastLoad) {
		while ((src[i] >> 24) == GE_CMD_VIEWMATRIXDATA) {
			const u32 newVal = src[i] << 8;
			if (dst[i] != newVal) {
				Flush();
				dst[i] = newVal;
				gstate_c.Dirty(DIRTY_VIEWMATRIX | DIRTY_CULL_PLANES);
			}
			if (++i >= end) {
				break;
			}
		}
	}

	const int count = i;
	gstate.viewmtxnum = (GE_CMD_VIEWMATRIXNUMBER << 24) | ((op & 0xF) + count);

	// Skip over the loaded data, it's done now.
	UpdatePC(currentList->pc, currentList->pc + count * 4);
	currentList->pc += count * 4;
}

void GPUCommonHW::Execute_ViewMtxData(u32 op, u32 diff) {
	// Note: it's uncommon to get here now, see above.
	int num = gstate.viewmtxnum & 0x00FFFFFF;
	u32 newVal = op << 8;
	if (num < 12 && newVal != ((const u32 *)gstate.viewMatrix)[num]) {
		Flush();
		((u32 *)gstate.viewMatrix)[num] = newVal;
		gstate_c.Dirty(DIRTY_VIEWMATRIX | DIRTY_CULL_PLANES);
	}
	num++;
	gstate.viewmtxnum = (GE_CMD_VIEWMATRIXNUMBER << 24) | (num & 0x00FFFFFF);
	gstate.viewmtxdata = GE_CMD_VIEWMATRIXDATA << 24;
}

void GPUCommonHW::Execute_ProjMtxNum(u32 op, u32 diff) {
	if (!currentList) {
		gstate.projmtxnum = (GE_CMD_PROJMATRIXNUMBER << 24) | (op & 0xF);
		return;
	}

	// This is almost always followed by GE_CMD_PROJMATRIXDATA.
	const u32_le *src = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
	u32 *dst = (u32 *)(gstate.projMatrix + (op & 0xF));
	const int end = 16 - (op & 0xF);
	int i = 0;

	bool fastLoad = true;
	if (currentList->pc < currentList->stall && currentList->pc + end * 4 >= currentList->stall) {
		fastLoad = false;
	}

	if (fastLoad) {
		while ((src[i] >> 24) == GE_CMD_PROJMATRIXDATA) {
			const u32 newVal = src[i] << 8;
			if (dst[i] != newVal) {
				Flush();
				dst[i] = newVal;
				gstate_c.Dirty(DIRTY_PROJMATRIX | DIRTY_CULL_PLANES);
			}
			if (++i >= end) {
				break;
			}
		}
	}

	const int count = i;
	gstate.projmtxnum = (GE_CMD_PROJMATRIXNUMBER << 24) | ((op & 0xF) + count);

	// Skip over the loaded data, it's done now.
	UpdatePC(currentList->pc, currentList->pc + count * 4);
	currentList->pc += count * 4;
}

void GPUCommonHW::Execute_ProjMtxData(u32 op, u32 diff) {
	// Note: it's uncommon to get here now, see above.
	int num = gstate.projmtxnum & 0x00FFFFFF;
	u32 newVal = op << 8;
	if (num < 16 && newVal != ((const u32 *)gstate.projMatrix)[num]) {
		Flush();
		((u32 *)gstate.projMatrix)[num] = newVal;
		gstate_c.Dirty(DIRTY_PROJMATRIX | DIRTY_CULL_PLANES);
	}
	num++;
	if (num <= 16)
		gstate.projmtxnum = (GE_CMD_PROJMATRIXNUMBER << 24) | (num & 0x00FFFFFF);
	gstate.projmtxdata = GE_CMD_PROJMATRIXDATA << 24;
}

void GPUCommonHW::Execute_TgenMtxNum(u32 op, u32 diff) {
	if (!currentList) {
		gstate.texmtxnum = (GE_CMD_TGENMATRIXNUMBER << 24) | (op & 0xF);
		return;
	}

	// This is almost always followed by GE_CMD_TGENMATRIXDATA.
	const u32_le *src = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
	u32 *dst = (u32 *)(gstate.tgenMatrix + (op & 0xF));
	const int end = 12 - (op & 0xF);
	int i = 0;

	bool fastLoad = end > 0;
	if (currentList->pc < currentList->stall && currentList->pc + end * 4 >= currentList->stall) {
		fastLoad = false;
	}

	if (fastLoad) {
		while ((src[i] >> 24) == GE_CMD_TGENMATRIXDATA) {
			const u32 newVal = src[i] << 8;
			if (dst[i] != newVal) {
				Flush();
				dst[i] = newVal;
				// We check the matrix to see if we need projection.
				gstate_c.Dirty(DIRTY_TEXMATRIX | DIRTY_FRAGMENTSHADER_STATE);
			}
			if (++i >= end) {
				break;
			}
		}
	}

	const int count = i;
	gstate.texmtxnum = (GE_CMD_TGENMATRIXNUMBER << 24) | ((op & 0xF) + count);

	// Skip over the loaded data, it's done now.
	UpdatePC(currentList->pc, currentList->pc + count * 4);
	currentList->pc += count * 4;
}

void GPUCommonHW::Execute_TgenMtxData(u32 op, u32 diff) {
	// Note: it's uncommon to get here now, see above.
	int num = gstate.texmtxnum & 0x00FFFFFF;
	u32 newVal = op << 8;
	if (num < 12 && newVal != ((const u32 *)gstate.tgenMatrix)[num]) {
		Flush();
		((u32 *)gstate.tgenMatrix)[num] = newVal;
		gstate_c.Dirty(DIRTY_TEXMATRIX | DIRTY_FRAGMENTSHADER_STATE);  // We check the matrix to see if we need projection
	}
	num++;
	gstate.texmtxnum = (GE_CMD_TGENMATRIXNUMBER << 24) | (num & 0x00FFFFFF);
	gstate.texmtxdata = GE_CMD_TGENMATRIXDATA << 24;
}

// Deadstation Portable: a run of BONEMATRIXDATA words copied into the bone matrices (each word << 8), up to limit of them, four at a
// time while all four are the command, the rest one by one; returns how many. The dirty bits for the run's matrices, once.
static int CopyBoneMatrixRun(const u32_le *src, u32 *dst, int limit) {
	int i = 0;
#if PPSSPP_ARCH(ARM64_NEON)
	const uint32x4_t cmd = vdupq_n_u32(GE_CMD_BONEMATRIXDATA);
	for (; i + 4 <= limit; i += 4) {
		const uint32x4_t w = vld1q_u32((const uint32_t *)src + i);
		if (vminvq_u32(vceqq_u32(vshrq_n_u32(w, 24), cmd)) == 0)
			break;
		vst1q_u32(dst + i, vshlq_n_u32(w, 8));
	}
#endif
	for (; i < limit && (src[i] >> 24) == GE_CMD_BONEMATRIXDATA; i++)
		dst[i] = src[i] << 8;
	return i;
}

static u32 BoneMatricesDirty(int first, int count) {
	if (count <= 0)
		return 0;
	const int lo = first / 12, hi = (first + count - 1) / 12;
	return (u32)(DIRTY_BONEMATRIX0 << lo) * ((2u << (hi - lo)) - 1);
}

void GPUCommonHW::Execute_BoneMtxNum(u32 op, u32 diff) {
	if (!currentList) {
		gstate.boneMatrixNumber = (GE_CMD_BONEMATRIXNUMBER << 24) | (op & 0x7F);
		return;
	}

	// This is almost always followed by GE_CMD_BONEMATRIXDATA.
	const u32_le *src = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
	u32 *dst = (u32 *)(gstate.boneMatrix + (op & 0x7F));
	const int end = 12 * 8 - (op & 0x7F);
	int i = 0;

	bool fastLoad = end > 0;
	if (currentList->pc < currentList->stall && currentList->pc + end * 4 >= currentList->stall) {
		fastLoad = false;
	}

	if (fastLoad) {
		// If we can't use software skinning, we have to flush and dirty.
		if (!g_Config.bSoftwareSkinning) {
			while ((src[i] >> 24) == GE_CMD_BONEMATRIXDATA) {
				const u32 newVal = src[i] << 8;
				if (dst[i] != newVal) {
					Flush();
					dst[i] = newVal;
				}
				if (++i >= end) {
					break;
				}
			}

			const unsigned int numPlusCount = (op & 0x7F) + i;
			for (unsigned int num = op & 0x7F; num < numPlusCount; num += 12) {
				gstate_c.Dirty(DIRTY_BONEMATRIX0 << (num / 12));
			}
		} else {
			i = CopyBoneMatrixRun(src, dst, end);
			gstate_c.deferredVertTypeDirty |= BoneMatricesDirty(op & 0x7F, i);
		}
	}

	const int count = i;
	gstate.boneMatrixNumber = (GE_CMD_BONEMATRIXNUMBER << 24) | ((op & 0x7F) + count);

	// Skip over the loaded data, it's done now.
	UpdatePC(currentList->pc, currentList->pc + count * 4);
	currentList->pc += count * 4;
}

void GPUCommonHW::Execute_BoneMtxData(u32 op, u32 diff) {
	// Note: it's uncommon to get here now, see above.
	int num = gstate.boneMatrixNumber & 0x00FFFFFF;
	u32 newVal = op << 8;
	if (num < 96 && newVal != ((const u32 *)gstate.boneMatrix)[num]) {
		// Bone matrices should NOT flush when software skinning is enabled!
		if (!g_Config.bSoftwareSkinning) {
			Flush();
			gstate_c.Dirty(DIRTY_BONEMATRIX0 << (num / 12));
		} else {
			gstate_c.deferredVertTypeDirty |= DIRTY_BONEMATRIX0 << (num / 12);
		} 
		((u32 *)gstate.boneMatrix)[num] = newVal;
	}
	num++;
	// Reached when the data isn't right behind the number, as in a matrix list that's called. Then the rest of it is right behind this
	// (God of War's skinned characters: 2.3% of this thread going through here word by word).
	if (g_Config.bSoftwareSkinning && currentList && num < 96 && !(currentList->pc < currentList->stall && currentList->pc + (96 - num) * 4 >= currentList->stall)) {
		const u32_le *src = (const u32_le *)Memory::GetPointerUnchecked(currentList->pc + 4);
		const int count = CopyBoneMatrixRun(src, (u32 *)gstate.boneMatrix + num, 96 - num);
		gstate_c.deferredVertTypeDirty |= BoneMatricesDirty(num, count);
		num += count;
		if (count != 0) {
			UpdatePC(currentList->pc, currentList->pc + count * 4);
			currentList->pc += count * 4;
		}
	}
	gstate.boneMatrixNumber = (GE_CMD_BONEMATRIXNUMBER << 24) | (num & 0x00FFFFFF);
	gstate.boneMatrixData = GE_CMD_BONEMATRIXDATA << 24;
}

void GPUCommonHW::Execute_TexFlush(u32 op, u32 diff) {
	// Games call this when they need the effect of drawing to be visible to texturing.
	// And for a bunch of other reasons, but either way, this is what we need to do.
	// It's possible we could also use this as a hint for the texture cache somehow.
	framebufferManager_->DiscardFramebufferCopy();
}

u32 GPUCommonHW::DrawSync(int mode) {
	drawEngineCommon_->FlushQueuedDepth();
	return GPUCommon::DrawSync(mode);
}

int GPUCommonHW::ListSync(int listid, int mode) {
	drawEngineCommon_->FlushQueuedDepth();
	return GPUCommon::ListSync(listid, mode);
}

