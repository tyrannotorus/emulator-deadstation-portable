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

#include "ppsspp_config.h"

#include <cstdlib>
#include <mutex>


#include "Common/LogReporting.h"
#include "Common/StringUtils.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"

#include "Core/Config.h"

#include "Core/MIPS/IR/IRJit.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitState.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/MIPSTables.h"

#include "../ARM64/Arm64Jit.h"
#include "../ARM64/Arm64IRJit.h"

namespace MIPSComp {
	JitInterface *jit;
	std::recursive_mutex jitLock;

	void JitAt() {
		// TODO: We could probably check for a bad pc here, and fire an exception. Could spare us from some crashes.
		// Although, we just tried to load from this address to check for a JIT block, and if we're here, that succeeded..
		jit->Compile(currentMIPS->pc);
	}

	void DoDummyJitState(PointerWrap &p) {
		// This is here so the savestate matches between jit and non-jit.
		auto s = p.Section("Jit", 1, 2);
		if (!s)
			return;

		bool dummy = false;
		Do(p, dummy);
		if (s >= 2) {
			dummy = true;
			Do(p, dummy);
		}
	}

	BranchInfo::BranchInfo(u32 pc, MIPSOpcode o, MIPSOpcode delayO, bool al, bool l)
		: compilerPC(pc), op(o), delaySlotOp(delayO), likely(l), andLink(al) {
		delaySlotInfo = MIPSGetInfo(delaySlotOp).value;
		delaySlotIsBranch = (delaySlotInfo & (IS_JUMP | IS_CONDBRANCH)) != 0;
	}

	u32 ResolveNotTakenTarget(const BranchInfo &branchInfo) {
		u32 notTakenTarget = branchInfo.compilerPC + 8;
		if ((branchInfo.delaySlotInfo & (IS_JUMP | IS_CONDBRANCH)) != 0) {
			// If a branch has a j/jr/jal/jalr as a delay slot, that is run if the branch is not taken.
			// TODO: Technically, in the likely case, we should somehow suppress andLink on this exit.
			bool isJump = (branchInfo.delaySlotInfo & IS_JUMP) != 0;
			// If the delay slot is a branch, likely skips it.
			if (isJump || !branchInfo.likely)
				notTakenTarget -= 4;

			// For a branch (not a jump), it actually should try the delay slot and take its target potentially.
			// This is similar to the VFPU case and has not been seen, so just report it.
			if (!isJump && SignExtend16ToU32(branchInfo.delaySlotOp) != SignExtend16ToU32(branchInfo.op) - 1)
				ERROR_LOG_REPORT(Log::JIT, "Branch in branch delay slot at %08x with different target", branchInfo.compilerPC);
			if (isJump && branchInfo.likely && (branchInfo.delaySlotInfo & (OUT_RA | OUT_RD)) != 0)
				ERROR_LOG_REPORT(Log::JIT, "Jump in likely branch delay slot with link at %08x", branchInfo.compilerPC);
	}
		return notTakenTarget;
}

	JitInterface *CreateNativeJit(MIPSState *mipsState, bool useIR) {
		if (useIR)
			return new MIPSComp::Arm64IRJit(mipsState);
		else
			return new MIPSComp::Arm64Jit(mipsState);
	}

}

