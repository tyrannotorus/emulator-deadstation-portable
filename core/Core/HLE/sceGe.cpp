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

#include <map>
#include <vector>
#include <mutex>

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeList.h"
#include "Common/Serialize/SerializeMap.h"
#include "Common/Data/Collections/ThreadSafeList.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/MIPS/MIPS.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/MemMapHelpers.h"
#include "Core/System.h"
#include "Core/HLE/sceGe.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/KernelWaitHelpers.h"
#include "GPU/GPUState.h"
#include "GPU/GPUCommon.h"
#include "GPU/GeThread.h"

static const int LIST_ID_MAGIC = 0x35000000;

static PspGeCallbackData ge_callback_data[16];
static bool ge_used_callbacks[16] = {0};

typedef std::vector<SceUID> WaitingThreadList;
static std::map<int, WaitingThreadList> listWaitingThreads;
static WaitingThreadList drawWaitingThreads;

struct GeInterruptData {
	int listid;
	u32 pc;
	u32 cmd;
};

static ThreadSafeList<GeInterruptData> ge_pending_cb;
static int geSyncEvent;
static int geInterruptEvent;
static int geCycleEvent;

bool __GeRunLists();
static void __GeListClosed(int listid);

class GeIntrHandler : public IntrHandler {
public:
	GeIntrHandler() : IntrHandler(PSP_GE_INTR) {}

	bool run(PendingInterrupt& pend) override {
		// Deadstation Portable (ge::lateEvents): what this reads of the list, the GE's thread wrote before raising the interrupt, and
		// the state it sets (complete) changes nothing that thread does: no wait for the GE here (God of War: 59.7 -> 49.0 with one)
		if (ge_pending_cb.empty()) {
			ERROR_LOG_REPORT(Log::sceGe, "Unable to run GE interrupt: no pending interrupt");
			return false;
		}

		GeInterruptData intrdata = ge_pending_cb.front();
		DisplayList* dl = gpu->getList(intrdata.listid);

		if (dl == NULL) {
			WARN_LOG(Log::sceGe, "Unable to run GE interrupt: list doesn't exist: %d", intrdata.listid);
			return false;
		}

		if (!dl->interruptsEnabled) {
			ERROR_LOG_REPORT(Log::sceGe, "Unable to run GE interrupt: list has interrupts disabled, should not happen");
			return false;
		}

		gpu->InterruptStart(intrdata.listid);

		const u32 cmd = intrdata.cmd;
		int subintr = -1;
		if (dl->subIntrBase >= 0) {
			switch (dl->signal) {
			case PSP_GE_SIGNAL_SYNC:
			case PSP_GE_SIGNAL_JUMP:
			case PSP_GE_SIGNAL_CALL:
			case PSP_GE_SIGNAL_RET:
				// Do nothing.
				break;

			case PSP_GE_SIGNAL_HANDLER_PAUSE:
				if (cmd == GE_CMD_FINISH)
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
				break;

			default:
				if (cmd == GE_CMD_SIGNAL)
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
				else
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_FINISH;
				break;
			}
		}

		// Set the list as complete once the interrupt starts.
		// In other words, not before another interrupt finishes.
		if (dl->signal != PSP_GE_SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH) {
			dl->state = PSP_GE_DL_STATE_COMPLETED;
			__GeListClosed(intrdata.listid);
		}

		SubIntrHandler* handler = get(subintr);
		if (handler != NULL) {
			DEBUG_LOG(Log::CPU, "Entering GE interrupt handler %08x", handler->handlerAddress);
			currentMIPS->pc = handler->handlerAddress;
			u32 data = dl->subIntrToken;
			currentMIPS->r[MIPS_REG_A0] = data & 0xFFFF;
			currentMIPS->r[MIPS_REG_A1] = handler->handlerArg;
			currentMIPS->r[MIPS_REG_A2] = sceKernelGetCompiledSdkVersion() <= 0x02000010 ? 0 : intrdata.pc + 4;
			// RA is already taken care of in __RunOnePendingInterrupt

			return true;
		}

		if (dl->signal == PSP_GE_SIGNAL_HANDLER_SUSPEND) {
			if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
				if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
					dl->state = PSP_GE_DL_STATE_QUEUED;
				}
			}
		}

		ge_pending_cb.pop_front();
		gpu->InterruptEnd(intrdata.listid);
		// Seen in GoW.
		if (subintr >= 0)
			DEBUG_LOG(Log::sceGe, "Ignoring interrupt for display list %d, already been released.", intrdata.listid);

		// Hm. This might be really tricky to get to behave the same in both modes. Here we are in __KernelReschedule, CoreTiming::Advance, ProcessEvents, GeExecuteInterrupt, ... .... __RunOnePendingInterrupt
		// But not sure how much it will matter. The test pause2 hits here.
		__GeRunLists();
		return false;
	}

	void handleResult(PendingInterrupt& pend) override {
		// (ge::lateEvents) InterruptEnd takes the list off the queue the GE's thread runs from
		if (ge::lateEvents)
			ge::sync();
		GeInterruptData intrdata = ge_pending_cb.front();
		ge_pending_cb.pop_front();

		DisplayList* dl = gpu->getList(intrdata.listid);
		if (!dl->interruptsEnabled) {
			ERROR_LOG_REPORT(Log::sceGe, "Unable to finish GE interrupt: list has interrupts disabled, should not happen");
			return;
		}

		switch (dl->signal) {
		case PSP_GE_SIGNAL_HANDLER_SUSPEND:
			if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
				// uofw says dl->state = endCmd & 0xFF;
				DisplayListState newState = static_cast<DisplayListState>(Memory::ReadUnchecked_U32(intrdata.pc - 4) & 0xFF);
				//dl->status = static_cast<DisplayListStatus>(Memory::ReadUnchecked_U32(intrdata.pc) & 0xFF);
				//if(dl->status < 0 || dl->status > PSP_GE_LIST_PAUSED)
				//	ERROR_LOG(Log::sceGe, "Weird DL status after signal suspend %x", dl->status);
				if (newState != PSP_GE_DL_STATE_RUNNING) {
					DEBUG_LOG_REPORT(Log::sceGe, "GE Interrupt: newState might be %d", newState);
				}

				if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
					dl->state = PSP_GE_DL_STATE_QUEUED;
				}
			}
			break;
		default:
			break;
		}

		gpu->InterruptEnd(intrdata.listid);

		// TODO: This is called from __KernelReturnFromInterrupt which does a bunch of stuff afterwards.
		// Using hleSplitSyscallOverGe here breaks the gpu/signals/suspend.prx test, for that reason.
		// However, it's still useful when debugging, and I believe the scheduling difference is quite insignificant -
		// we are already being extremely inaccurate by blasting the full display list here instead of running
		// it in the background in parallel with the CPU.
		// So, when debugging is active, we'll just use hleSplitSyscallOverGe.
		__GeRunLists();
	}
};

static GeIntrHandler *geIntrHandler = nullptr;

// a handler the game has for a list's interrupts: the game may be waiting on it, and idle time can't pass it by
static bool __GeHasHandler(int subIntrBase) {
	return subIntrBase >= 0 && geIntrHandler != nullptr && (geIntrHandler->get(subIntrBase | PSP_GE_SUBINTR_FINISH) != nullptr || geIntrHandler->get(subIntrBase | PSP_GE_SUBINTR_SIGNAL) != nullptr);
}

// Lists with such a handler, enqueued and not yet finished as the game has seen it (its FINISH interrupt, a dequeue, a break),
// kept by the CPU thread itself: the GE thread's own queue changes as it runs.
static u64 handlerListsOpen = 0;

static void __GeListOpened(int listid, int subIntrBase) {
	if (listid >= 0 && listid < 64 && __GeHasHandler(subIntrBase))
		handlerListsOpen |= 1ULL << listid;
}

static void __GeListClosed(int listid) {
	if (listid >= 0 && listid < 64)
		handlerListsOpen &= ~(1ULL << listid);
}

// from the lists themselves, the GE idle (a state loaded)
static void __GeListsOpenAgain() {
	handlerListsOpen = 0;
	if (!gpu)
		return;
	for (int i = 0; i < 64; i++) {
		const DisplayList *dl = gpu->getList(i);
		if (dl && dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED)
			__GeListOpened(i, dl->subIntrBase);
	}
}

// The lists queued go on running on the GE's thread, with the CPU going on, unless the game could be waiting for what they end
// with. True if they ran here. Decided from the CPU thread's own state only, never the GE thread's progress.
bool __GeRunLists() {
	if (!ge::lateEvents && handlerListsOpen != 0) {
		ge::runListsHere(CoreTiming::GetTicks());
		return true;
	}
	ge::runLists(CoreTiming::GetTicks());
	return !ge::active();
}

// Deadstation Portable (ge::lateEvents): the CPU has nothing to run (CoreTiming::Idle). A list the game has a handler for may still
// be on the GE's thread, and what it raises could be what the game waits for: idle time passes only once it's known.
bool __GeIdleSync() {
	if (!ge::lateEvents || handlerListsOpen == 0 || !ge::unreleased())
		return false;
	return ge::idleSync();
}

static void __GeExecuteSync(u64 userdata, int cyclesLate) {
	int listid = userdata >> 32;
	GPUSyncType type = (GPUSyncType) (userdata & 0xFFFFFFFF);
	bool wokeThreads = __GeTriggerWait(type, listid);
	if (ge::lateEvents && type == GPU_SYNC_DRAW && wokeThreads)
		ge::sync();
	gpu->SyncEnd(type, listid, wokeThreads);
}

static void __GeExecuteInterrupt(u64 userdata, int cyclesLate) {
	__TriggerInterrupt(PSP_INTR_IMMEDIATE, PSP_GE_INTR, PSP_INTR_SUB_NONE);
}

static void __GeCheckCycles(u64 userdata, int cyclesLate) {
	// Deprecated
}

static void __GeForgetStalls();

void __GeInit() {
	__GeForgetStalls();
	handlerListsOpen = 0;
	memset(&ge_used_callbacks, 0, sizeof(ge_used_callbacks));
	memset(&ge_callback_data, 0, sizeof(ge_callback_data));
	ge_pending_cb.clear();
	geIntrHandler = new GeIntrHandler();
	__RegisterIntrHandler(PSP_GE_INTR, geIntrHandler);

	geSyncEvent = CoreTiming::RegisterEvent("GeSyncEvent", &__GeExecuteSync);
	geInterruptEvent = CoreTiming::RegisterEvent("GeInterruptEvent", &__GeExecuteInterrupt);

	// Deprecated
	geCycleEvent = CoreTiming::RegisterEvent("GeCycleEvent", &__GeCheckCycles);

	listWaitingThreads.clear();
	drawWaitingThreads.clear();
}

struct GeInterruptData_v1 {
	int listid;
	u32 pc;
};

void __GeDoState(PointerWrap &p) {
	__GeForgetStalls();
	auto s = p.Section("sceGe", 1, 2);
	if (!s)
		return;

	DoArray(p, ge_callback_data, ARRAY_SIZE(ge_callback_data));
	DoArray(p, ge_used_callbacks, ARRAY_SIZE(ge_used_callbacks));

	if (s >= 2) {
		Do(p, ge_pending_cb);
	} else {
		std::list<GeInterruptData_v1> old;
		Do(p, old);
		ge_pending_cb.clear();
		for (const auto &ge : old) {
			GeInterruptData intrdata = {ge.listid, ge.pc};
			intrdata.cmd = Memory::ReadUnchecked_U32(ge.pc - 4) >> 24;
			ge_pending_cb.push_back(intrdata);
		}
	}

	Do(p, geSyncEvent);
	CoreTiming::RestoreRegisterEvent(geSyncEvent, "GeSyncEvent", &__GeExecuteSync);
	Do(p, geInterruptEvent);
	CoreTiming::RestoreRegisterEvent(geInterruptEvent, "GeInterruptEvent", &__GeExecuteInterrupt);
	Do(p, geCycleEvent);
	CoreTiming::RestoreRegisterEvent(geCycleEvent, "GeCycleEvent", &__GeCheckCycles);

	Do(p, listWaitingThreads);
	Do(p, drawWaitingThreads);

	// Everything else is done in sceDisplay (before this: the lists are loaded)
	__GeListsOpenAgain();
}

void __GeShutdown() {
	__GeForgetStalls();
}

bool __GeTriggerSync(GPUSyncType type, int id, u64 atTicks) {
	u64 userdata = (u64)id << 32 | (u64)type;
	s64 future = atTicks - CoreTiming::GetTicks();
	if (type == GPU_SYNC_DRAW) {
		s64 left = CoreTiming::UnscheduleEvent(geSyncEvent, userdata);
		if (left > future)
			future = left;
	}
	CoreTiming::ScheduleEvent(future, geSyncEvent, userdata);
	return true;
}

bool __GeTriggerInterrupt(int listid, u32 pc, u64 atTicks) {
	GeInterruptData intrdata;
	intrdata.listid = listid;
	intrdata.pc = pc;
	intrdata.cmd = Memory::ReadUnchecked_U32(pc - 4) >> 24;

	ge_pending_cb.push_back(intrdata);

	u64 userdata = (u64)listid << 32 | (u64) pc;
	CoreTiming::ScheduleEvent(atTicks - CoreTiming::GetTicks(), geInterruptEvent, userdata);
	return true;
}

void __GeWaitCurrentThread(GPUSyncType type, SceUID waitId, const char *reason) {
	WaitType waitType;
	if (type == GPU_SYNC_DRAW) {
		drawWaitingThreads.push_back(__KernelGetCurThread());
		waitType = WAITTYPE_GEDRAWSYNC;
	} else if (type == GPU_SYNC_LIST) {
		listWaitingThreads[waitId].push_back(__KernelGetCurThread());
		waitType = WAITTYPE_GELISTSYNC;
	} else {
		ERROR_LOG_REPORT(Log::sceGe, "__GeWaitCurrentThread: bad wait type");
		return;
	}

	__KernelWaitCurThread(waitType, waitId, 0, 0, false, reason);
}

static bool __GeTriggerWait(WaitType waitType, SceUID waitId, WaitingThreadList &waitingThreads) {
	// TODO: Do they ever get a result other than 0?
	bool wokeThreads = false;
	for (int threadID : waitingThreads)
		wokeThreads |= HLEKernel::ResumeFromWait(threadID, waitType, waitId, 0);
	waitingThreads.clear();
	return wokeThreads;
}

bool __GeTriggerWait(GPUSyncType type, SceUID waitId) {
	// We check for the old type for old savestate compatibility.
	if (type == GPU_SYNC_DRAW || (WaitType)type == WAITTYPE_GEDRAWSYNC)
		return __GeTriggerWait(WAITTYPE_GEDRAWSYNC, waitId, drawWaitingThreads);
	else if (type == GPU_SYNC_LIST || (WaitType)type == WAITTYPE_GELISTSYNC)
		return __GeTriggerWait(WAITTYPE_GELISTSYNC, waitId, listWaitingThreads[waitId]);
	else
		ERROR_LOG_REPORT(Log::sceGe, "__GeTriggerWait: bad wait type");
	return false;
}

// Some games spam this, like MediEvil.
static u32 sceGeEdramGetAddr() {
	u32 retVal = 0x04000000;
	hleEatCycles(150);
	return hleLogVerbose(Log::sceGe, retVal);
}

// TODO: Return a different value for the PS3 enhanced-emulator games?
static u32 sceGeEdramGetSize() {
	const u32 retVal = 0x00200000;
	return hleLogVerbose(Log::sceGe, retVal);
}

static int __GeSubIntrBase(int callbackId) {
	return callbackId * 2;
}

// the GE's display lists (GPUCommon::DisplayListMaxCount; sceGeListSync's 64)
static constexpr int GeListCount = 64;
// Deadstation Portable: each list's stall as the game last set it (enqueue, updates): where the commands it now lets the GE run start
static u32 cpuStall[GeListCount];
// what a walk of a list's commands knows of it (__GeQuietCommands)
struct StallWalk {
	bool valid = false;		// the offset is the GE's: since an enqueue, or a wait that left the GE idle
	u32 offset = 0;
};
static StallWalk stallWalk[GeListCount];

static void __GeKnowStall(u32 listid, u32 stall) {
	if (listid < (u32)GeListCount) {
		cpuStall[listid] = stall & 0x0FFFFFFF;
		stallWalk[listid].valid = true;
		stallWalk[listid].offset = 0;	// as EnqueueList starts it
	}
}

// not known again until the next enqueue (a state loaded, a list taken off)
static void __GeForgetStalls() {
	for (StallWalk &walk : stallWalk)
		walk.valid = false;
}

u32 sceGeListEnQueue(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	ge::syncLists();
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);

	bool runList;
	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, false, &runList);
	if ((int)listID >= 0) {
		__GeKnowStall(listID, stallAddress);
		__GeListOpened(listID, __GeSubIntrBase(callbackId));
		listID = LIST_ID_MAGIC ^ listID;
	}
	if (runList) {
		__GeRunLists();
	}
	hleEatCycles(490);
	hleCoreTimingForceCheck();
	DEBUG_LOG(Log::sceGe,
		"%08x=sceGeListEnQueue(addr=%08x, stall=%08x, cbid=%08x, param=%08x) ticks=%lld", listID,
		listAddress, stallAddress, callbackId, optParamAddr, (long long)CoreTiming::GetTicks());
	return hleNoLog(listID); // We already logged above, logs get confusing if we use hleLogSuccess.
}

u32 sceGeListEnQueueHead(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	ge::sync();
	DEBUG_LOG(Log::sceGe,
		"sceGeListEnQueueHead(addr=%08x, stall=%08x, cbid=%08x, param=%08x) ticks=%lld",
		listAddress, stallAddress, callbackId, optParamAddr, (long long)CoreTiming::GetTicks());
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);

	bool runList;
	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, true, &runList);
	if ((int)listID >= 0) {
		__GeKnowStall(listID, stallAddress);
		__GeListOpened(listID, __GeSubIntrBase(callbackId));
		listID = LIST_ID_MAGIC ^ listID;
	}
	if (runList) {
		__GeRunLists();
	}
	hleEatCycles(480);
	hleCoreTimingForceCheck();
	return hleNoLog(listID); // We already logged above, logs get confusing if we use hleLogSuccess.
}

static int sceGeListDeQueue(u32 listID) {
	ge::sync();
	__GeForgetStalls();
	WARN_LOG(Log::sceGe, "sceGeListDeQueue(%08x)", listID);
	int result = gpu->DequeueList(LIST_ID_MAGIC ^ listID);
	if (result == 0)
		__GeListClosed(LIST_ID_MAGIC ^ listID);
	hleReSchedule("dlist dequeued");
	return hleNoLog(result);
}


// The commands from one stall to the next can't let the game see the GE: none of those the GE will run raises an interrupt or
// ends the list (SIGNAL, FINISH, END), goes where this walk can't follow (BJUMP, a JUMP elsewhere, video memory) or writes
// memory (TRANSFERSTART). Running them on the GE's thread while the CPU goes on changes nothing the game sees: their run starts
// at the update's time as before, and a run that stops at its stall hands nothing back (GPUCommon::ProcessDLQueue). The GE
// reads vertex and texture memory for them a little later, as a PSP's GE does.
// The walk goes as the GE does: a JUMP to where it goes (LEGO's jump over vertices in the list, and within their called lists),
// a CALL into its list and back at its RET, targets from BASE (only once set in this walk: another list may set it between) and the list's offset
// (ORIGIN, OFFSETADDR; per list, kept across walks, as the GE keeps it).

static bool __GeQuietCommands(StallWalk &walk, u32 from, u32 to) {
	if (!walk.valid)
		return false;
	if (to <= from || to - from > 0x40000 || ((from | to) & 3) != 0 || !Memory::IsValidRange(from, to - from) || Memory::IsVRAMAddress(from)) {
		return false;
	}
	bool baseKnown = false;
	u32 base = 0;
	u32 returnTo[8], offsetAt[8];
	int depth = 0;
	int budget = 0x10000;
	// the update then waits, and the GE's offset is read again after it (sceGeListUpdateStallAddr)
	const StallWalk before = walk;
	const auto lost = [&](u32) {
		walk = before;
		return false;
	};
	u32 pc = from;
	while (depth != 0 || pc != to) {
		if (--budget < 0 || !Memory::IsValidAddress(pc) || Memory::IsVRAMAddress(pc))
			return lost(0x101);
		const u32 op = Memory::ReadUnchecked_U32(pc);
		const u32 cmd = op >> 24;
		switch (cmd) {
		case GE_CMD_BASE:
			baseKnown = true;
			base = op & 0x000F0000;
			break;
		case GE_CMD_ORIGIN:
			walk.offset = pc;
			break;
		case GE_CMD_OFFSETADDR:
			walk.offset = op << 8;
			break;
		case GE_CMD_JUMP:
		case GE_CMD_CALL:
		{
			if (!baseKnown)
				return lost(cmd);
			const u32 target = (walk.offset + ((base << 8) | (op & 0x00FFFFFC))) & 0x0FFFFFFF;
			if (!Memory::IsValidAddress(target) || Memory::IsVRAMAddress(target))
				return lost(cmd);
			if (cmd == GE_CMD_JUMP) {
				// anywhere, as the GE goes (LEGO's called lists jump back to a shared tail): the walk still ends only at the stall,
				// and its budget ends a loop
				pc = target;
				continue;
			}
			if (depth == 8)
				return lost(cmd);
			returnTo[depth] = pc + 4;
			offsetAt[depth] = walk.offset;
			depth++;
			pc = target;
			continue;
		}
		case GE_CMD_RET:
			if (depth == 0)
				return lost(cmd);
			depth--;
			pc = returnTo[depth];
			walk.offset = offsetAt[depth];
			continue;
		case GE_CMD_BJUMP:
		case GE_CMD_END: case GE_CMD_SIGNAL: case GE_CMD_FINISH: case GE_CMD_TRANSFERSTART:
			return lost(cmd);
		}
		pc += 4;
	}
	return true;
}

static int sceGeListUpdateStallAddr(u32 displayListID, u32 stallAddress) {
	const int listid = LIST_ID_MAGIC ^ displayListID;
	const u32 newStall = stallAddress & 0x0FFFFFFF;
	if (ge::active() && listid >= 0 && listid < GeListCount && (ge::lateEvents || __GeQuietCommands(stallWalk[listid], cpuStall[listid], newStall))) {
		// LEGO moves its stall ~80 times a frame: the CPU waited for the GE's thread each time (7.8 ms a vblank)
		hleEatCycles(190);
		hleCoreTimingForceCheck();
		const u32 retval = gpu->UpdateStallResult(listid);
		if (retval == 0) {
			cpuStall[listid] = newStall;
			ge::updateStall(listid, newStall, CoreTiming::GetTicks());
		}
		return hleNoLog(retval);
	}
	ge::sync();
	// Advance() might cause an interrupt, so defer the Advance but do it ASAP.
	// Final Fantasy Type-0 has a graphical artifact without this (timing issue.)
	hleEatCycles(190);
	hleCoreTimingForceCheck();

	DEBUG_LOG(Log::sceGe, "sceGeListUpdateStallAddr(dlid=%i, stalladdr=%08x)", displayListID, stallAddress);
	bool runList;
	int retval = gpu->UpdateStall(LIST_ID_MAGIC ^ displayListID, stallAddress, &runList);
	if (retval == 0 && listid >= 0 && listid < GeListCount)
		cpuStall[listid] = newStall;
	bool ranHere = true;
	if (runList) {
		ranHere = __GeRunLists();
	}
	// the GE idle at the new stall (a list with a handler run here): its offset for the list is known again
	if (listid >= 0 && listid < GeListCount) {
		stallWalk[listid].valid = retval == 0 && ranHere;
		if (stallWalk[listid].valid)
			stallWalk[listid].offset = gpu->ListOffsetAddr(listid);
	}
	return hleNoLog(retval);
}

// 0 : wait for completion. 1:check and return
int sceGeListSync(u32 displayListID, u32 mode) {
	if ((LIST_ID_MAGIC ^ displayListID) < 64)
		ge::sync();
	hleEatCycles(220);  // Fudged without measuring, copying sceGeContinue.
	return hleLogDebug(Log::sceGe, gpu->ListSync(LIST_ID_MAGIC ^ displayListID, mode));
}

static u32 sceGeDrawSync(u32 mode) {
	ge::sync();
	//wait/check entire drawing state
	if (PSP_CoreParameter().compat.flags().DrawSyncEatCycles)
		hleEatCycles(500000); //HACK(?) : Potential fix for Crash Tag Team Racing and a few Gundam games
	else if (!PSP_CoreParameter().compat.flags().DrawSyncInstant)
		hleEatCycles(1240);
	return hleLogDebug(Log::sceGe, gpu->DrawSync(mode));
}

static int sceGeContinue() {
	ge::sync();
	bool runList;
	int ret = gpu->Continue(&runList);
	if (runList) {
		__GeRunLists();
	}
	hleEatCycles(220);
	hleReSchedule("ge continue");
	return hleLogDebug(Log::sceGe, ret);
}

static int sceGeBreak(u32 mode, u32 unknownPtr) {
	if (mode > 1) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_INVALID_MODE, "invalid mode");
	}
	// Not sure what this is supposed to be for...
	if ((int)unknownPtr < 0 || (int)(unknownPtr + 16) < 0) {
		WARN_LOG_REPORT(Log::sceGe, "sceGeBreak(mode=%d, unknown=%08x): invalid ptr", mode, unknownPtr);
		return SCE_KERNEL_ERROR_PRIV_REQUIRED;
	} else if (unknownPtr != 0) {
		WARN_LOG_REPORT_ONCE(gebreak_unkptr, Log::sceGe, "sceGeBreak(mode=%d, unknown=%08x): unknown ptr (%s)", mode, unknownPtr, Memory::IsValidAddress(unknownPtr) ? "valid" : "invalid");
	}

	//mode => 0 : current dlist 1: all drawing
	DEBUG_LOG(Log::sceGe, "sceGeBreak(mode=%d, unknown=%08x)", mode, unknownPtr);
	ge::sync();
	int result = gpu->Break(mode);
	__GeListsOpenAgain();
	if (result >= 0 && mode == 0) {
		return hleNoLog(LIST_ID_MAGIC ^ result);
	}
	return hleNoLog(result);
}

static u32 sceGeSetCallback(u32 structAddr) {
	int cbID = -1;
	for (size_t i = 0; i < ARRAY_SIZE(ge_used_callbacks); ++i) {
		if (!ge_used_callbacks[i]) {
			cbID = (int) i;
			break;
		}
	}

	if (cbID == -1) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_OUT_OF_MEMORY, "out of callback ids");
	}

	ge_used_callbacks[cbID] = true;
	auto callbackData = PSPPointer<PspGeCallbackData>::Create(structAddr);
	ge_callback_data[cbID] = *callbackData;
	callbackData.NotifyRead("GeSetCallback");

	int subIntrBase = __GeSubIntrBase(cbID);

	// TODO: Maybe don't ignore return values of the hleCalls?

	if (ge_callback_data[cbID].finish_func != 0) {
		hleCall(InterruptManager, u32, sceKernelRegisterSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH,
				ge_callback_data[cbID].finish_func, ge_callback_data[cbID].finish_arg);
		hleCall(InterruptManager, u32, sceKernelEnableSubIntr, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH);
	}
	if (ge_callback_data[cbID].signal_func != 0) {
		hleCall(InterruptManager, u32, sceKernelRegisterSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL,
				ge_callback_data[cbID].signal_func, ge_callback_data[cbID].signal_arg);
		hleCall(InterruptManager, u32, sceKernelEnableSubIntr, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL);
	}

	return hleLogDebug(Log::sceGe, cbID);
}

static int sceGeUnsetCallback(u32 cbID) {
	if (cbID >= ARRAY_SIZE(ge_used_callbacks)) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_INVALID_ID, "invalid callback id");
	}

	if (ge_used_callbacks[cbID]) {
		int subIntrBase = __GeSubIntrBase(cbID);

		// TODO: Maybe don't ignore return values?
		hleCall(InterruptManager, u32, sceKernelReleaseSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH);
		hleCall(InterruptManager, u32, sceKernelReleaseSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL);
	} else {
		WARN_LOG(Log::sceGe, "sceGeUnsetCallback(cbid=%08x): ignoring unregistered callback id", cbID);
	}

	ge_used_callbacks[cbID] = false;
	return hleLogDebug(Log::sceGe, 0);
}

// Points to 512 32-bit words, where we can probably layout the context however we want
// unless some insane game pokes it and relies on it...
u32 sceGeSaveContext(u32 ctxAddr) {
	ge::sync();
	if (gpu->BusyDrawing()) {
		// Real error code.
		return hleLogWarning(Log::sceGe, -1, "lists in process, aborting");
	}

	// Let's just dump gstate.
	if (Memory::IsValidAddress(ctxAddr)) {
		gstate.Save((u32_le *)Memory::GetPointer(ctxAddr));
	}

	// This action should probably be pushed to the end of the queue of the display thread -
	// when we have one.
	return hleLogDebug(Log::sceGe, 0);
}

u32 sceGeRestoreContext(u32 ctxAddr) {
	ge::sync();
	if (gpu->BusyDrawing()) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_BUSY, "lists in process, aborting");
	}

	GE_CALL(([&] {
		if (Memory::IsValidAddress(ctxAddr)) {
			gstate.Restore((u32_le *)Memory::GetPointer(ctxAddr));
		}
		gpu->ReapplyGfxState();
	}()));
	return hleLogDebug(Log::sceGe, 0);
}

static int sceGeGetMtx(int type, u32 matrixPtr) {
	ge::sync();
	int size = type == GE_MTX_PROJECTION ? 16 : 12;
	if (!Memory::IsValidRange(matrixPtr, size * sizeof(float))) {
		return hleLogError(Log::sceGe, -1, "bad matrix ptr");
	}

	u32_le *dest = (u32_le *)Memory::GetPointerWriteUnchecked(matrixPtr);
	// Note: this reads the CPU-visible matrix values, which may differ from the actual used values.
	// They only differ when more DATA commands are sent than are valid for a matrix.
	if (!gpu || !gpu->GetMatrix24(GEMatrixType(type), dest, 0))
		return hleLogError(Log::sceGe, SCE_KERNEL_ERROR_INVALID_INDEX, "invalid matrix");

	return hleLogInfo(Log::sceGe, 0);
}

static u32 sceGeGetCmd(int cmd) {
	ge::sync();
	if (cmd >= 0 && cmd < (int)ARRAY_SIZE(gstate.cmdmem)) {
		// Does not mask away the high bits.  But matrix regs don't read back.
		u32 val = gstate.cmdmem[cmd];
		switch (cmd) {
		case GE_CMD_BONEMATRIXDATA:
		case GE_CMD_WORLDMATRIXDATA:
		case GE_CMD_VIEWMATRIXDATA:
		case GE_CMD_PROJMATRIXDATA:
		case GE_CMD_TGENMATRIXDATA:
			val &= 0xFF000000;
			break;

		case GE_CMD_BONEMATRIXNUMBER:
			val &= 0xFF00007F;
			break;

		case GE_CMD_WORLDMATRIXNUMBER:
		case GE_CMD_VIEWMATRIXNUMBER:
		case GE_CMD_PROJMATRIXNUMBER:
		case GE_CMD_TGENMATRIXNUMBER:
			val &= 0xFF00000F;
			break;

		default:
			break;
		}
		return hleLogInfo(Log::sceGe, val);
	}
	return hleLogError(Log::sceGe, SCE_KERNEL_ERROR_INVALID_INDEX);
}

static int sceGeGetStack(int index, u32 stackPtr) {
	ge::sync();
	return hleReportWarning(Log::sceGe, gpu->GetStack(index, stackPtr));
}

static u32 sceGeEdramSetAddrTranslation(u32 new_size) {
	ge::sync();
	bool outsideRange = new_size != 0 && (new_size < 0x200 || new_size > 0x1000);
	bool notPowerOfTwo = (new_size & (new_size - 1)) != 0;
	if (outsideRange || notPowerOfTwo) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid value");
	}
	if (!gpu) {
		return hleLogError(Log::sceGe, -1, "GPUInterface not available");
	}

	return hleLogDebug(Log::sceGe, gpu->SetAddrTranslation(new_size));
}

const HLEFunction sceGe_user[] = {
	{0XE47E40E4, &WrapU_V<sceGeEdramGetAddr>,            "sceGeEdramGetAddr",            'x', ""    },
	{0XAB49E76A, &WrapU_UUIU<sceGeListEnQueue>,          "sceGeListEnQueue",             'x', "xxip"},
	{0X1C0D95A6, &WrapU_UUIU<sceGeListEnQueueHead>,      "sceGeListEnQueueHead",         'x', "xxip"},
	{0XE0D68148, &WrapI_UU<sceGeListUpdateStallAddr>,    "sceGeListUpdateStallAddr",     'i', "xx"  },
	{0X03444EB4, &WrapI_UU<sceGeListSync>,               "sceGeListSync",                'x', "xx"  },
	{0XB287BD61, &WrapU_U<sceGeDrawSync>,                "sceGeDrawSync",                'x', "x"   },
	{0XB448EC0D, &WrapI_UU<sceGeBreak>,                  "sceGeBreak",                   'i', "xx"  },
	{0X4C06E472, &WrapI_V<sceGeContinue>,                "sceGeContinue",                'i', ""    },
	{0XA4FC06A4, &WrapU_U<sceGeSetCallback>,             "sceGeSetCallback",             'i', "p"   },
	{0X05DB22CE, &WrapI_U<sceGeUnsetCallback>,           "sceGeUnsetCallback",           'i', "x"   },
	{0X1F6752AD, &WrapU_V<sceGeEdramGetSize>,            "sceGeEdramGetSize",            'x', ""    },
	{0XB77905EA, &WrapU_U<sceGeEdramSetAddrTranslation>, "sceGeEdramSetAddrTranslation", 'x', "x"   },
	{0XDC93CFEF, &WrapU_I<sceGeGetCmd>,                  "sceGeGetCmd",                  'x', "i"   },
	{0X57C8945B, &WrapI_IU<sceGeGetMtx>,                 "sceGeGetMtx",                  'i', "ip"  },
	{0X438A385A, &WrapU_U<sceGeSaveContext>,             "sceGeSaveContext",             'x', "x"   },
	{0X0BF608FB, &WrapU_U<sceGeRestoreContext>,          "sceGeRestoreContext",          'x', "x"   },
	{0X5FB86AB0, &WrapI_U<sceGeListDeQueue>,             "sceGeListDeQueue",             'i', "x"   },
	{0XE66CB92E, &WrapI_IU<sceGeGetStack>,               "sceGeGetStack",                'i', "ix"  },
};

void Register_sceGe_user() {
	RegisterHLEModule("sceGe_user", ARRAY_SIZE(sceGe_user), sceGe_user);
}
