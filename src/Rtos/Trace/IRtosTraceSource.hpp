#ifndef _IRTOSRTRACESOURCE_HPP
#define _IRTOSRTRACESOURCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosTraceTypes.hpp"

/*
 * What a source hands over in one read.
 *
 * The batch is the unit of trust. A read either produces a whole, consistent
 * set of events with the cursor that follows them, or it produces nothing that
 * may be committed to. Half a batch cannot be applied: the events in it were
 * decoded against a cursor, and advancing the cursor past events that were not
 * decoded would silently drop them - which is exactly the failure the loss
 * accounting exists to make impossible.
 *
 * Everything a caller needs to explain the batch travels with it: the
 * capability bits in force, what was skipped, what the source thought of
 * itself while reading. A caller that has to ask the source a second question
 * about the same read is a caller that can get two answers from two moments.
 */
struct RtosTraceBatch
{
	std::vector<RtosTraceEvent> events;

	/* Where the read started and where it would leave the cursor. Both are
	   source-defined counters; the model stores them as opaque numbers. */
	uint64_t cursorBefore = 0;
	uint64_t cursorAfter = 0;

	/* The read saw a record that was being written at the time (ring) or a
	   frame that failed its check (stream). Nothing in this batch may be
	   committed. */
	bool torn = false;

	/* The read failed outright - probe gone, control block unreadable. Also
	   not committable, and different from `torn` in what it says about the
	   target: a torn record is a target that is running, a failed read is a
	   link that is not. */
	bool readFailed = false;

	/* Records of a type this build does not know. They were skipped, and
	   skipping is not an error - but the count is part of the answer, because
	   a decoder that quietly drops half a stream looks exactly like a target
	   that sends half as much. */
	uint32_t unknownRecords = 0;

	/* Records the target refused to write (its own reject counter), when the
	   source can read it. */
	uint32_t rejectedByTarget = 0;

	RtosTraceCapabilities capabilities;
	RtosTraceSourceState state = RtosTraceSourceState::NotStarted;

	/* One line per thing worth saying about this read, for the log and for the
	   quality track. Not translated: these are facts about the wire. */
	std::vector<std::string> diagnostics;

	/* The rule the whole layer rests on: only a clean batch moves the cursor. */
	bool canCommit() const
	{
		return !torn && !readFailed;
	}

	uint64_t committedCursor(uint64_t current) const
	{
		return canCommit() ? cursorAfter : current;
	}

	bool empty() const
	{
		return events.empty();
	}
};

/*
 * A place events come from.
 *
 * Four implementations are planned and they differ in almost everything except
 * this interface: MTF v1 (order and counts, no target clock), SWD v2 (ring with
 * cursors and generation), RTEC2 (full handles and lifecycle), and a recording
 * being replayed (already decoded). Nothing above this line may know which one
 * it is talking to; what it needs to know instead is `capabilities()`, which is
 * data rather than a type test.
 *
 * `poll` takes a budget in seconds and returns what it managed inside it. It is
 * allowed to return an empty batch; it is not allowed to block past the budget,
 * because the caller is the sampling thread and the target is being read
 * through the same probe.
 */
class IRtosTraceSource
{
   public:
	virtual ~IRtosTraceSource() = default;

	/* Opens the transport and reads the description block. Failing here leaves
	   nothing open behind: a source that half starts is a probe nobody can
	   take back. */
	virtual bool start(RtosTraceProblem& problem) = 0;

	/* Closes the transport. Safe to call twice, and called after a failed
	   start. */
	virtual void stop() = 0;

	/* Asks for a new epoch: the caller has decided the current stream cannot
	   be trusted (three torn reads, a sequence that cannot be explained) and
	   wants the target to start a new one, or the source to re-read its
	   description block. */
	virtual void requestNewEpoch() = 0;

	virtual RtosTraceCapabilities capabilities() const = 0;
	virtual RtosTraceSourceState state() const = 0;

	/* For the log and for the settings line: "swd", "mtf", "replay". */
	virtual const char* name() const = 0;

	virtual RtosTraceBatch poll(double budgetSeconds) = 0;
};

#endif
