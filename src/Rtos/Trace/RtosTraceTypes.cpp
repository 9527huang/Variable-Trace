#include "RtosTraceTypes.hpp"

/* The strings here are for logs and for the protocol documents. What a reader
   sees comes from the interface, which translates its own vocabulary - a log
   line saying "EpochBegin" is a fact about the wire, not about the target. */

const char* toString(RtosEntityKind kind)
{
	switch (kind)
	{
		case RtosEntityKind::Task:
			return "task";

		case RtosEntityKind::Sync:
			return "sync";

		case RtosEntityKind::Isr:
			return "isr";

		case RtosEntityKind::Heap:
			return "heap";

		case RtosEntityKind::None:
		default:
			return "none";
	}
}

const char* toString(RtosIdentityQuality quality)
{
	switch (quality)
	{
		case RtosIdentityQuality::Exact:
			return "exact";

		case RtosIdentityQuality::Inferred:
			return "inferred";

		case RtosIdentityQuality::Legacy:
			return "legacy";

		case RtosIdentityQuality::Unknown:
		default:
			return "unknown";
	}
}

const char* toString(RtosEvidenceQuality quality)
{
	switch (quality)
	{
		case RtosEvidenceQuality::Good:
			return "good";

		case RtosEvidenceQuality::Partial:
			return "partial";

		case RtosEvidenceQuality::Unknown:
			return "unknown";

		case RtosEvidenceQuality::Unusable:
		default:
			return "unusable";
	}
}

const char* toString(RtosTraceEventType type)
{
	switch (type)
	{
		case RtosTraceEventType::EpochBegin:
			return "EpochBegin";

		case RtosTraceEventType::ClockSync:
			return "ClockSync";

		case RtosTraceEventType::Gap:
			return "Gap";

		case RtosTraceEventType::SourceStatus:
			return "SourceStatus";

		case RtosTraceEventType::TaskCreate:
			return "TaskCreate";

		case RtosTraceEventType::TaskDelete:
			return "TaskDelete";

		case RtosTraceEventType::TaskRename:
			return "TaskRename";

		case RtosTraceEventType::TaskSwitchIn:
			return "TaskSwitchIn";

		case RtosTraceEventType::TaskSwitchOut:
			return "TaskSwitchOut";

		case RtosTraceEventType::TaskReady:
			return "TaskReady";

		case RtosTraceEventType::TaskBlock:
			return "TaskBlock";

		case RtosTraceEventType::TaskWake:
			return "TaskWake";

		case RtosTraceEventType::PriorityChange:
			return "PriorityChange";

		case RtosTraceEventType::SyncCreate:
			return "SyncCreate";

		case RtosTraceEventType::SyncDelete:
			return "SyncDelete";

		case RtosTraceEventType::WaitBegin:
			return "WaitBegin";

		case RtosTraceEventType::WaitEnd:
			return "WaitEnd";

		case RtosTraceEventType::Signal:
			return "Signal";

		case RtosTraceEventType::Acquire:
			return "Acquire";

		case RtosTraceEventType::Release:
			return "Release";

		case RtosTraceEventType::IsrEnter:
			return "IsrEnter";

		case RtosTraceEventType::IsrExit:
			return "IsrExit";

		case RtosTraceEventType::Fault:
			return "Fault";

		case RtosTraceEventType::Alloc:
			return "Alloc";

		case RtosTraceEventType::Free:
			return "Free";

		case RtosTraceEventType::Unknown:
		default:
			return "Unknown";
	}
}

bool opensLifetime(RtosTraceEventType type)
{
	return type == RtosTraceEventType::TaskCreate || type == RtosTraceEventType::SyncCreate;
}

bool closesLifetime(RtosTraceEventType type)
{
	return type == RtosTraceEventType::TaskDelete || type == RtosTraceEventType::SyncDelete;
}

const char* toString(RtosGapReason reason)
{
	switch (reason)
	{
		case RtosGapReason::RingOverwritten:
			return "ring overwritten";

		case RtosGapReason::TargetRejected:
			return "target rejected";

		case RtosGapReason::StreamBytesLost:
			return "stream bytes lost";

		case RtosGapReason::CrcError:
			return "crc error";

		case RtosGapReason::SequenceJump:
			return "sequence jump";

		case RtosGapReason::HostBudgetMissed:
			return "host budget missed";

		case RtosGapReason::TornRetryFailed:
			return "torn retry failed";

		case RtosGapReason::SegmentRestarted:
			return "segment restarted";

		case RtosGapReason::Unknown:
		default:
			return "unknown";
	}
}

const char* capabilityName(uint32_t bit)
{
	switch (bit)
	{
		case 0:
			return "rawHandle32";

		case 1:
			return "lifecycleEvents";

		case 2:
			return "generation";

		case 3:
			return "targetClock";

		case 4:
			return "eventSequence";

		case 5:
			return "lossDetection";

		case 6:
			return "isrEvents";

		case 7:
			return "faultEvents";

		case 8:
			return "syncEvents";

		case 9:
			return "heapEvents";

		case 10:
			return "dictionary";

		default:
			return "reserved";
	}
}

const char* toString(RtosTraceSourceState state)
{
	switch (state)
	{
		case RtosTraceSourceState::Running:
			return "running";

		case RtosTraceSourceState::Degraded:
			return "degraded";

		case RtosTraceSourceState::Stopped:
			return "stopped";

		case RtosTraceSourceState::Failed:
			return "failed";

		case RtosTraceSourceState::NotStarted:
		default:
			return "not started";
	}
}

const char* describe(RtosTraceProblem problem)
{
	switch (problem)
	{
		case RtosTraceProblem::NotAChoice:
			return "no trace source was chosen";

		case RtosTraceProblem::UnsupportedWireVersion:
			return "the target speaks a wire version this build does not read";

		case RtosTraceProblem::ControlBlockMissing:
			return "the control block symbol could not be found";

		case RtosTraceProblem::ControlBlockCorrupt:
			return "the control block is present but does not describe a usable ring";

		case RtosTraceProblem::ProbeBusy:
			return "the probe is owned by another tool";

		case RtosTraceProblem::ProbeLost:
			return "the probe connection was lost";

		case RtosTraceProblem::IdentityUnusable:
			return "the events cannot be attributed to entities";

		case RtosTraceProblem::None:
		default:
			return "";
	}
}
