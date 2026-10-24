#ifndef _RTOSTRACETYPES_HPP
#define _RTOSTRACETYPES_HPP

#include <cstdint>
#include <string>
#include <vector>

/*
 * The vocabulary the trace layer speaks, and nothing else.
 *
 * Three programs have to agree about what an event is: the target that writes
 * it, the Python tools that collect and check it, and this program that reads
 * it. The wire format is frozen in `docs/RTEC2-wire-format.md`; this file is
 * the C++ side of the same contract, and it deliberately holds no logic that
 * could disagree with the document.
 *
 * Two rules shape everything here:
 *
 * 1. **What is not known is not zero.** A handle that could not be read, a
 *    generation that was never seen, a time that did not arrive - each has its
 *    own representation rather than a zero that reads as a fact. The whole
 *    quality apparatus exists so that "we did not see it" never has to be
 *    written as "it was not there".
 *
 * 2. **Identity is a five part key.** Session, epoch, kind, handle and
 *    generation. A shorter key is a legacy convenience: two lifetimes of the
 *    same TCB address are two entities, and code that compares addresses is
 *    code that will one day join two histories that never met.
 */

/* What kind of thing a handle refers to. The namespaces are separate on
   purpose: an ISR number and a task address may well both be 0x20001000. */
enum class RtosEntityKind : uint8_t
{
	None,
	Task,
	Sync,
	Isr,
	Heap,
};

const char* toString(RtosEntityKind kind);

/* How much the identity in an `RtosEntityRef` is worth.
 *
 * `Exact` is the only one that may be used as a key on its own. `Legacy` means
 * a short id from a protocol that predates 32 bit handles; it identifies a slot
 * for as long as nobody reuses it, and the interface has to say so. */
enum class RtosIdentityQuality : uint8_t
{
	Exact,
	Inferred,
	Legacy,
	Unknown,
};

const char* toString(RtosIdentityQuality quality);

/* How much an event, or a conclusion drawn from events, can be trusted.
 *
 * `Unknown` is not a synonym for `Unusable`: it says the question could not be
 * answered (no target clock, a gap across the interval), while `Unusable` says
 * the data itself is known to be broken. A rule that cannot answer must not
 * report a healthy target - that is what these four values are for. */
enum class RtosEvidenceQuality : uint8_t
{
	Good,
	Partial,
	Unknown,
	Unusable,
};

const char* toString(RtosEvidenceQuality quality);

/* The event types of RTEC2 §3.1, in the same order and with the same values.
 * The numbers travel on the wire; renaming a constant is free, renumbering is
 * not. */
enum class RtosTraceEventType : uint8_t
{
	Unknown = 0x00,

	EpochBegin = 0x01,
	ClockSync = 0x02,
	Gap = 0x03,
	SourceStatus = 0x04,

	TaskCreate = 0x10,
	TaskDelete = 0x11,
	TaskRename = 0x12,
	TaskSwitchIn = 0x13,
	TaskSwitchOut = 0x14,
	TaskReady = 0x15,
	TaskBlock = 0x16,
	TaskWake = 0x17,
	PriorityChange = 0x18,

	SyncCreate = 0x20,
	SyncDelete = 0x21,
	WaitBegin = 0x22,
	WaitEnd = 0x23,
	Signal = 0x24,
	Acquire = 0x25,
	Release = 0x26,

	IsrEnter = 0x30,
	IsrExit = 0x31,

	Fault = 0x40,

	Alloc = 0x50,
	Free = 0x51,
};

const char* toString(RtosTraceEventType type);

/* Whether a type opens or closes a lifetime, which is what the identity store
   watches for. Kept here so that the store and the decoders do not each keep
   their own list. */
bool opensLifetime(RtosTraceEventType type);
bool closesLifetime(RtosTraceEventType type);

/* Why a gap happened. The reason is kept rather than folded into one "loss"
   count: an overrun ring and a byte that never arrived call for different
   answers, and a total that mixes them cannot be acted on. */
enum class RtosGapReason : uint8_t
{
	Unknown = 0,
	RingOverwritten = 1,
	TargetRejected = 2,
	StreamBytesLost = 3,
	CrcError = 4,
	SequenceJump = 5,
	HostBudgetMissed = 6,
	TornRetryFailed = 7,
	SegmentRestarted = 8,
};

const char* toString(RtosGapReason reason);

/* One reference to one entity, with the honesty flags attached. */
struct RtosEntityRef
{
	RtosEntityKind kind = RtosEntityKind::None;

	/* The 32 bit handle as the target gave it: a TCB address, an object
	   address, an IRQ number. Zero means "not known", never "entity 0". */
	uint32_t rawHandle = 0;

	/* Zero means unknown. One is the first real generation. */
	uint32_t generation = 0;

	/* Only for sources that speak in short ids (MTF v1, SWD v2). Kept so that
	   a legacy source can still be shown, marked as the weaker thing it is. */
	uint16_t legacyId = 0;

	RtosIdentityQuality quality = RtosIdentityQuality::Unknown;

	bool hasHandle() const
	{
		return rawHandle != 0;
	}

	/* Stable enough to key a history on. Everything else - an inferred handle,
	   a legacy slot, a generation nobody saw - may join two lifetimes that
	   never met, so it is not allowed to. */
	bool isStable() const
	{
		return quality == RtosIdentityQuality::Exact && hasHandle() && generation != 0;
	}
};

/*
 * The identity of one lifetime of one entity.
 *
 * `sessionId` is not a field: a store belongs to one session, and putting the
 * string on every key would make comparison and sorting pay for it. The key is
 * what is left - epoch, kind, handle, generation - which is the part that
 * varies inside a session.
 */
struct RtosEntityKey
{
	uint64_t epoch = 0;
	RtosEntityKind kind = RtosEntityKind::None;
	uint32_t rawHandle = 0;
	uint32_t generation = 0;

	static RtosEntityKey from(const RtosEntityRef& ref, uint64_t epoch)
	{
		RtosEntityKey key;
		key.epoch = epoch;
		key.kind = ref.kind;
		key.rawHandle = ref.rawHandle;
		key.generation = ref.generation;

		return key;
	}

	bool operator==(const RtosEntityKey& other) const
	{
		return epoch == other.epoch && kind == other.kind && rawHandle == other.rawHandle && generation == other.generation;
	}

	bool operator!=(const RtosEntityKey& other) const
	{
		return !(*this == other);
	}

	/* Ordered so that a map can hold them and a listing can be stable. */
	bool operator<(const RtosEntityKey& other) const
	{
		if (epoch != other.epoch)
			return epoch < other.epoch;

		if (kind != other.kind)
			return static_cast<uint8_t>(kind) < static_cast<uint8_t>(other.kind);

		if (rawHandle != other.rawHandle)
			return rawHandle < other.rawHandle;

		return generation < other.generation;
	}

	bool isStable() const
	{
		return rawHandle != 0 && generation != 0 && kind != RtosEntityKind::None;
	}
};

/*
 * One event, as the model sees it.
 *
 * The three times are kept apart on purpose (wire format §5.1): `targetTicks`
 * is the only input to diagnostics, `targetHz` says which clock those ticks
 * belong to, and the display time is derived and never stored here.
 */
struct RtosTraceEvent
{
	uint64_t epoch = 0;
	uint32_t sequence = 0;

	/* False when the source has no target clock (MTF v1, or a target without
	   DWT). Every duration based conclusion is NotApplicable in that case -
	   the arrival time at the host is not the execution time and must not be
	   dressed up as it. */
	bool hasTargetTicks = false;
	uint32_t targetTicks = 0;
	uint32_t targetHz = 0;

	uint8_t coreId = 0;

	RtosTraceEventType type = RtosTraceEventType::Unknown;
	RtosEntityRef subject;
	RtosEntityRef object;

	uint32_t arg0 = 0;
	uint32_t arg1 = 0;

	/* The record's own bytes, when it has any. A fault frame carries its
	   registers here and nowhere else; a rule that wanted them would otherwise
	   have to go back to the decoder with the sequence number and hope. */
	uint8_t payloadBytes = 0;
	uint8_t payload[12] = {};

	RtosEvidenceQuality quality = RtosEvidenceQuality::Unknown;
};

/* The capability bits of wire format §6. */
struct RtosTraceCapabilities
{
	uint32_t bits = 0;

	bool has(uint32_t bit) const
	{
		return (bits & (1u << bit)) != 0;
	}

	void set(uint32_t bit, bool value)
	{
		if (value)
			bits |= (1u << bit);
		else
			bits &= ~(1u << bit);
	}

	bool realHandles() const
	{
		return has(0);
	}

	bool lifecycle() const
	{
		return has(1);
	}

	bool generations() const
	{
		return has(2);
	}

	bool targetClock() const
	{
		return has(3);
	}

	bool sequence() const
	{
		return has(4);
	}

	bool lossDetection() const
	{
		return has(5);
	}

	bool isrEvents() const
	{
		return has(6);
	}

	bool faultEvents() const
	{
		return has(7);
	}

	bool syncEvents() const
	{
		return has(8);
	}

	bool heapEvents() const
	{
		return has(9);
	}

	bool dictionary() const
	{
		return has(10);
	}
};

/* Named bits, so that a manifest or a log line does not carry bare numbers. */
const char* capabilityName(uint32_t bit);

/*
 * What the events came from, written into a recording beside them.
 *
 * A file read back on another machine has no probe and no source object, and
 * the interface still has to say where the events came from and which tracks
 * it is allowed to draw. Both answers are here rather than derived from the
 * file: the capabilities describe what the target could do when the session
 * was recorded, and a reader that recomputed them would be guessing.
 */
struct RtosTraceSourceRecord
{
	std::string name;
	RtosTraceCapabilities capabilities;
};

/* What a source found when it was asked what it can do.
 *
 * The three "cannot" values are kept apart because they are answered
 * differently: a build without the feature is a fact about the firmware, a
 * missing symbol is a fact about the ELF, and a protocol that does not carry
 * the data is a fact about the wire. */
enum class RtosTraceSourceState
{
	NotStarted,
	Running,
	/* The source works but not at full capability: order and counts only. */
	Degraded,
	Stopped,
	Failed,
};

const char* toString(RtosTraceSourceState state);

/* Why a source refused to start. A code rather than a sentence: the words
 * belong to the interface, and only it knows what language the reader reads. */
enum class RtosTraceProblem
{
	None,
	NotAChoice,
	UnsupportedWireVersion,
	ControlBlockMissing,
	ControlBlockCorrupt,
	ProbeBusy,
	ProbeLost,
	IdentityUnusable,
};

const char* describe(RtosTraceProblem problem);

#endif
