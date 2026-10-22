#ifndef _RTOSTRACEREDUCER_HPP
#define _RTOSTRACEREDUCER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosTracePipeline.hpp"
#include "RtosTraceTypes.hpp"

/*
 * Events, folded into the things a reader looks at.
 *
 * The timeline draws tasks as lanes, and a lane is an interval: "this task ran
 * from here to there". Interrupts, waits, faults and allocations want the same
 * shape - an interval or a mark on one line - and each of them is built by
 * pairing two events or by reading one. That pairing is rules, not drawing, so
 * it lives here where it can be tested without a window.
 *
 * Two rules shape everything:
 *
 * 1. **An unpaired event is a fact, not nothing.** An interrupt that entered and
 *    never left (the target was stopped mid-handler, or the exit was lost) is
 *    counted as `unclosed` and not drawn as an interval of unknown length. The
 *    same for a wait without its end. A picture that silently drops these looks
 *    exactly like a target that did nothing unusual.
 *
 * 2. **No target time means no duration.** When the source has no clock, the
 *    interval is still recorded - it happened, and in this order - but with
 *    `hasSeconds = false` rather than a made up zero. Every rule above that
 *    needs a duration then has an answer: not computable.
 */
struct RtosTrackEntry
{
	enum class Kind
	{
		Task,
		Isr,
		Fault,
		Sync,
		Heap,
		Gap,
	};

	Kind kind = Kind::Task;

	/* Where it sits in the event history, so that the interface can jump back
	   to the event that produced it. */
	size_t firstEvent = 0;
	size_t lastEvent = 0;

	/* Both ends, on the session axis. `hasSeconds` is false when the source had
	   no target clock: the entry still exists, its length does not. */
	bool hasSeconds = false;
	double fromSeconds = 0.0;
	double toSeconds = 0.0;

	bool open = false;

	/* Who or what it is about: an IRQ number, an object's name, an allocation
	   address. Empty when the event did not say. */
	std::string name;
	uint32_t address = 0;

	/* Isr: nesting depth at which it was entered. */
	uint32_t depth = 0;

	/* Fault: what the frame said, and which parts of it were there. */
	uint8_t faultClass = 0;
	uint8_t faultFlags = 0;
	uint16_t cfsr = 0;
	uint32_t pc = 0;
	uint32_t lr = 0;

	/* Sync: how the wait ended (§3.3), 0xFF when the end was never seen. */
	uint8_t waitResult = 0xFF;

	/* Heap: bytes, and whether this entry allocated or freed. */
	uint32_t bytes = 0;
	bool allocated = false;

	/* Gap: how many records were lost and why (§4.2 of the wire format). */
	uint32_t lostRecords = 0;
	uint8_t gapReason = 0;
};

struct RtosTrackProjection
{
	std::vector<RtosTrackEntry> isr;
	std::vector<RtosTrackEntry> fault;
	std::vector<RtosTrackEntry> sync;
	std::vector<RtosTrackEntry> heap;
	std::vector<RtosTrackEntry> gap;

	/* Entries that never got their second event. Counted per track because
	   "the target was stopped inside an interrupt" and "the exit was lost in a
	   gap" are different problems that look the same in an empty track. */
	uint32_t unclosedIsr = 0;
	uint32_t unclosedSync = 0;

	/* Ends without a beginning: a wait that ended for a task that was never
	   seen to start waiting, usually because the session joined late. */
	uint32_t orphanIsr = 0;
	uint32_t orphanSync = 0;

	/* Net heap change over the window, and how many entries it came from. Only
	   meaningful when both alloc and free carry a size. */
	int64_t heapNetBytes = 0;
	uint32_t heapUnknownSize = 0;
};

class RtosTraceReducer
{
   public:
	/* One pass over the history, oldest first. */
	static RtosTrackProjection project(const std::vector<RtosNormalizedEvent>& events);
};

#endif
