#pragma once

#include <vector>
#include <list>
#include <mutex>

#include "ppsspp_config.h"
#include "Common/Common.h"
#include "Common/Swap.h"
#include "Core/MemMap.h"
#include "Common/MemoryUtil.h"
#include "GPU/GPU.h"
#include "GPU/GPUDefinitions.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"


// X11, sigh.
#ifdef None
#undef None
#endif

class FramebufferManagerCommon;
class TextureCacheCommon;
class DrawEngineCommon;
class GraphicsContext;
struct PspGeListArgs;
struct GPUgstate;
class PointerWrap;
struct VirtualFramebuffer;

namespace Draw {
class DrawContext;
}


inline bool IsTrianglePrim(GEPrimitiveType prim) {
	// TODO: KEEP_PREVIOUS is mistakenly treated as TRIANGLE here... This isn't new.
	//
	// Interesting optimization, but not confident in performance:
	// static const bool p[8] = { false, false, false, true, true, true, false, true };
	// 10111000 = 0xB8;
	// return (0xB8U >> (u8)prim) & 1;

	return prim > GE_PRIM_LINE_STRIP && prim != GE_PRIM_RECTANGLES;
}

class GPUCommon {
public:
	// The constructor might run on the loader thread.
	GPUCommon(GraphicsContext *gfxCtx, Draw::DrawContext *draw);

	// FinishInitOnMainThread runs on the main thread, of course.
	virtual void FinishInitOnMainThread() {}

	virtual ~GPUCommon() {}

	Draw::DrawContext *GetDrawContext() {
		return draw_;
	}

	virtual void DeviceLost() = 0;
	virtual void DeviceRestore(Draw::DrawContext *draw) = 0;

	virtual u32 CheckGPUFeatures() const = 0;

	virtual void UpdateCmdInfo() = 0;

	virtual void Reinitialize();

	virtual void BeginHostFrame();
	virtual void EndHostFrame();

	void InterruptStart(int listid);
	void InterruptEnd(int listid);
	void SyncEnd(GPUSyncType waitType, int listid, bool wokeThreads);
	void EnableInterrupts(bool enable) {
		interruptsEnabled_ = enable;
	}

	virtual void CheckDisplayResized() = 0;
	virtual void CheckConfigChanged() = 0;

	virtual void NotifyDisplayResized();
	virtual void NotifyRenderResized();
	virtual void NotifyConfigChanged();

	void DumpNextFrame();

	virtual void PreExecuteOp(u32 op, u32 diff) {}

	DLResult ProcessDLQueue(u64 ticks);
	// the queue, seen from the CPU thread while the GE's runs
	bool ListsQueued();
	bool QueuedListsHave(bool (*handler)(int subIntrBase));

	u32 UpdateStall(int listid, u32 newstall, bool *runList);
	// Deadstation Portable: what UpdateStall would return, read on the CPU thread while the GE's thread may be running the list's
	// commands that can't complete it (sceGeListUpdateStallAddr): the list's state is then stable
	u32 UpdateStallResult(int listid) const;
	// a list's offset (ORIGIN, OFFSETADDR) where it stopped: with the GE's thread idle
	u32 ListOffsetAddr(int listid) const { return dls[listid].offsetAddr; }
	u32 EnqueueList(u32 listpc, u32 stall, int subIntrBase, PSPPointer<PspGeListArgs> args, bool head, bool *runList);
	u32 DequeueList(int listid);
	virtual int ListSync(int listid, int mode);
	virtual u32 DrawSync(int mode);
	int GetStack(int index, u32 stackPtr);
	virtual bool GetMatrix24(GEMatrixType type, u32_le *result, u32 cmdbits);
	virtual void ResetMatrices();
	virtual void DoState(PointerWrap &p);
	bool BusyDrawing();
	u32 Continue(bool *runList);
	u32 Break(int mode);

	virtual bool FramebufferDirty() = 0;
	virtual bool FramebufferReallyDirty() = 0;

	virtual void ReapplyGfxState();

	uint32_t SetAddrTranslation(uint32_t value);
	uint32_t GetAddrTranslation();

	virtual void SetDisplayFramebuffer(u32 framebuf, u32 stride, GEBufferFormat format) = 0;
	virtual void SetCurFramebufferDirty(bool dirty) = 0;
	virtual void PrepareCopyDisplayToOutput() = 0;
	virtual void CopyDisplayToOutput() = 0;
	virtual bool PresentedThisFrame() const = 0;

	// Invalidate any cached content sourced from the specified range.
	// If size = -1, invalidate everything.
	virtual void InvalidateCache(u32 addr, int size, GPUInvalidationType type) = 0;

	// These return true if they handled the operation enough that the actual memory operation should be skipped. Not always a clear-cut case...
	virtual bool PerformMemoryCopy(u32 dest, u32 src, int size, GPUCopyFlag flags = GPUCopyFlag::NONE);
	virtual bool PerformMemorySet(u32 dest, u8 v, int size);

	virtual bool PerformReadbackToMemory(u32 dest, int size);
	virtual bool PerformWriteColorFromMemory(u32 dest, int size);

	virtual void PerformWriteFormattedFromMemory(u32 addr, int size, int width, GEBufferFormat format);
	virtual bool PerformWriteStencilFromMemory(u32 dest, int size, WriteStencil flags);

	virtual void ExecuteOp(u32 op, u32 diff) = 0;

	void Execute_OffsetAddr(u32 op, u32 diff);
	void Execute_Vaddr(u32 op, u32 diff);
	void Execute_Iaddr(u32 op, u32 diff);
	void Execute_Origin(u32 op, u32 diff);
	void Execute_Jump(u32 op, u32 diff);
	void Execute_BJump(u32 op, u32 diff);
	void Execute_Call(u32 op, u32 diff);
	void Execute_Ret(u32 op, u32 diff);
	void Execute_End(u32 op, u32 diff);

	void Execute_BoundingBox(u32 op, u32 diff);

	void Execute_MorphWeight(u32 op, u32 diff);

	void Execute_ImmVertexAlphaPrim(u32 op, u32 diff);

	void Execute_Unknown(u32 op, u32 diff);

	static int EstimatePerVertexCost();

	void Flush();

#ifdef USE_CRT_DBG
#undef new
#endif
	void *operator new(size_t s) {
		return AllocateAlignedMemory(s, 16);
	}
	void operator delete(void *p) {
		FreeAlignedMemory(p);
	}
#ifdef USE_CRT_DBG
#define new DBG_NEW
#endif

	DisplayList* getList(int listid) {
		return &dls[listid];
	}

	s64 GetListTicks(int listid) const {
		if (listid >= 0 && listid < DisplayListMaxCount) {
			return dls[listid].waitUntilTicks;
		}
		return -1;
	}

	void PSPFrame();

protected:
	virtual void ClearCacheNextFrame() {}

	virtual void CheckRenderResized() {}

	void SetDrawType(DrawType type, GEPrimitiveType prim) {
		if (type != lastDraw_) {
			// We always flush when drawing splines/beziers so no need to do so here
			gstate_c.Dirty(DIRTY_UVSCALEOFFSET | DIRTY_VERTEXSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE);
			lastDraw_ = type;
		}
		// Prim == RECTANGLES can cause CanUseHardwareTransform to flip, so we need to dirty.
		// Also, culling may be affected so dirty the raster state.
		if (IsTrianglePrim(prim) != IsTrianglePrim(lastPrim_)) {
			Flush();
			gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE);
			lastPrim_ = prim;
		}
	}

	virtual void CheckDepthUsage(VirtualFramebuffer *vfb) {}
	virtual void FastRunLoop(DisplayList &list) = 0;

	bool SlowRunLoop(DisplayList &list);  // Returns false on breakpoint.
	void UpdatePC(u32 currentPC, u32 newPC);
	void UpdateState(GPURunState state);
	void FastLoadBoneMatrix(u32 target);
	void FlushImm();
	void DoBlockTransfer(u32 skipDrawReason);

	// TODO: Unify this. Vulkan and OpenGL are different due to how they buffer data.
	virtual void FinishDeferred() {}

	void AdvanceVerts(u32 vertType, int count, int bytesRead) {
		if ((vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
			const int indexShift = ((vertType & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
			gstate_c.indexAddr += count << indexShift;
		} else {
			gstate_c.vertexAddr += bytesRead;
		}
	}


	virtual void UpdateMSAALevel(Draw::DrawContext *draw) {}

	enum {
		DisplayListMaxCount = 64
	};

	DrawEngineCommon *drawEngineCommon_ = nullptr;

	// TODO: These should live in GPUCommonHW.
	FramebufferManagerCommon *framebufferManager_ = nullptr;
	TextureCacheCommon *textureCache_ = nullptr;

	bool flushOnParams_ = true;

	GraphicsContext *gfxCtx_;
	Draw::DrawContext *draw_ = nullptr;

	typedef std::list<int> DisplayListQueue;

	int nextListID;
	DisplayList dls[DisplayListMaxCount];
	DisplayList *currentList;
	DisplayListQueue dlQueue;
	std::mutex queueLock_;

	bool interruptRunning = false;
	GPURunState gpuState = GPUSTATE_RUNNING;
	bool isbreak;  // This doesn't mean debugger breakpoints.
	u64 drawCompleteTicks;
	u64 busyTicks;

	int downcount;
	u64 startingTicks;
	u32 cycleLastPC;
	int cyclesExecuted;

	bool resumingFromDebugBreak_ = false;
	bool dumpNextFrame_ = false;
	bool dumpThisFrame_ = false;
	bool useFastRunLoop_ = false;
	bool interruptsEnabled_ = false;
	bool displayResized_ = false;
	bool renderResized_ = false;
	bool configChanged_ = false;
	DrawType lastDraw_ = DRAW_UNKNOWN;
	GEPrimitiveType lastPrim_ = GE_PRIM_INVALID;

	int vertexCost_ = 0;

	// No idea how big this buffer needs to be.
	enum {
		MAX_IMMBUFFER_SIZE = 32,
	};

	TransformedVertex immBuffer_[MAX_IMMBUFFER_SIZE];
	int immCount_ = 0;
	GEPrimitiveType immPrim_ = GE_PRIM_INVALID;
	uint32_t immFlags_ = 0;
	bool immFirstSent_ = false;

	uint32_t edramTranslation_ = 0x400;

	// When matrix data overflows, the CPU visible values wrap and bleed between matrices.
	// But this doesn't actually change the values used by rendering.
	// The CPU visible values affect the GPU when list contexts are restored.
	// Note: not maintained by all backends, here for save stating.
	union {
		struct {
			u32 bone[12 * 8];
			u32 world[12];
			u32 view[12];
			u32 proj[16];
			u32 tgen[12];
		};
		u32 all[12 * 8 + 12 + 12 + 16 + 12];
	} matrixVisible;

private:
	void DoExecuteCall(u32 target);
	void PopDLQueue();
	void CheckDrawSync();
};
