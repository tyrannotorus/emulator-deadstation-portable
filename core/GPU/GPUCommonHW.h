#pragma once

#include "GPUCommon.h"

// Shared GPUCommon implementation for the HW backends.
// Things that are irrelevant for SoftGPU should live here.
class VertexDecoder;

class GPUCommonHW : public GPUCommon {
public:
	GPUCommonHW(GraphicsContext *gfxCtx, Draw::DrawContext *draw);
	~GPUCommonHW() override;

	// This can fail, and if so no render pass is active.
	void SetCurFramebufferDirty(bool dirty) override { curFramebufferDirty_ = dirty; }
	void PrepareCopyDisplayToOutput() override;
	void CopyDisplayToOutput() override;
	void DoState(PointerWrap &p) override;
	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

	void BeginHostFrame() override;

	u32 CheckGPUFeatures() const override;

	FramebufferManagerCommon *GetFramebufferManagerCommon() {
		return framebufferManager_;
	}
	TextureCacheCommon *GetTextureCacheCommon() {
		return textureCache_;
	}

	void SetDisplayFramebuffer(u32 framebuf, u32 stride, GEBufferFormat format) override;
	void InvalidateCache(u32 addr, int size, GPUInvalidationType type) override;

	u32 DrawSync(int mode) override;
	int ListSync(int listid, int mode) override;

	bool FramebufferDirty() override;
	bool FramebufferReallyDirty() override;

	void Execute_VertexType(u32 op, u32 diff);
	void Execute_VertexTypeSkinning(u32 op, u32 diff);

	void Execute_Prim(u32 op, u32 diff);
	void Execute_Bezier(u32 op, u32 diff);
	void Execute_Spline(u32 op, u32 diff);
	void Execute_BlockTransferStart(u32 op, u32 diff);

	void Execute_TexSize0(u32 op, u32 diff);
	void Execute_TexLevel(u32 op, u32 diff);
	void Execute_LoadClut(u32 op, u32 diff);

	void Execute_WorldMtxNum(u32 op, u32 diff);
	void Execute_WorldMtxData(u32 op, u32 diff);
	void Execute_ViewMtxNum(u32 op, u32 diff);
	void Execute_ViewMtxData(u32 op, u32 diff);
	void Execute_ProjMtxNum(u32 op, u32 diff);
	void Execute_ProjMtxData(u32 op, u32 diff);
	void Execute_TgenMtxNum(u32 op, u32 diff);
	void Execute_TgenMtxData(u32 op, u32 diff);
	void Execute_BoneMtxNum(u32 op, u32 diff);
	void Execute_BoneMtxData(u32 op, u32 diff);

	void Execute_TexFlush(u32 op, u32 diff);

	// TODO: Have these return an error code if they jump to a bad address. If bad, stop the FastRunLoop.
	typedef void (GPUCommonHW::*CmdFunc)(u32 op, u32 diff);

	void FastRunLoop(DisplayList &list) override;
	void FlushDrawEngineForAsm();  // the ARM64 command loop's flush (GeRunLoopArm64.cpp)

	// Execute_Prim's lookahead (the commands after a PRIM, read on from it), its common commands in hand-written ARM64
	// (GePrimLoopArm64.cpp): the loop's state, both halves'
	struct PrimLoop {
		const u32_le *src;
		const u32_le *stall;
		VertexDecoder *decoder;
		DrawEngineCommon *engine;
		u32 *cmdmem;			// gstate.cmdmem
		void *gstateC;			// &gstate_c
		const u8 *base;			// Memory::base
		int *numCulledDraws;	// &gpuStats.numCulledDraws
		u32 vertexType;
		u32 vertTypeID;
		u32 vtypeCheckMask;
		u32 idHigh;				// a vertex type's ID above its low 24 bits: the UV generation mode and software skinning
		int cullMode;
		int totalVertCount;
		int bytesRead;
		bool canExtend;
		bool isTriangle;
		bool onePassed;
		bool vramPending;		// g_vramAlias.pending(): no command the loop runs changes it
		bool bail;
	};
	// From s->src while the commands are its own: it stops at the stall, at a command the C++ runs (s->bail false), or at one
	// the C++ would end the lookahead at (s->bail true).
	static void PrimLoopArm64(GPUCommonHW *gpu, PrimLoop *s);
	bool PrimLoopBoneCall(u32 data);
	void ExecuteOp(u32 op, u32 diff) override;

	bool PresentedThisFrame() const override;

private:
	void CheckDepthUsage(VirtualFramebuffer *vfb) override;
	void CheckFlushOp(int cmd, u32 diff);

protected:
	void UpdateCmdInfo() override;

	void PreExecuteOp(u32 op, u32 diff) override;
	void ClearCacheNextFrame() override;

	// Needs to be called on GPU thread, not reporting thread.
	void UpdateMSAALevel(Draw::DrawContext *draw) override;

	void CheckDisplayResized() override;
	void CheckRenderResized() override;
	void CheckConfigChanged() override;

	u32 CheckGPUFeaturesLate(u32 features) const;

	int msaaLevel_ = 0;
	bool sawExactEqualDepth_ = false;
	ShaderManagerCommon *shaderManager_ = nullptr;
	bool curFramebufferDirty_ = false;
};
