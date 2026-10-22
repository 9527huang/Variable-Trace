#ifndef _TRACEADAPTERS_HPP
#define _TRACEADAPTERS_HPP

#include <cstdint>
#include <deque>
#include <vector>

#include "IRtosTraceSource.hpp"
#include "RtosEventRing.hpp"
#include "RtosItmDecoder.hpp"
#include "RtosTraceTypes.hpp"

/*
 * The two carriers this program already speaks, wrapped in the new interface.
 *
 * Both carry the same eight byte record - which task, and when - and both are
 * older than RTEC2: an address is a real handle, but there is no generation, no
 * lifecycle and no sequence number. That is exactly what makes them useful now:
 * they are the compatibility path, and the model above them has to work with
 * what they give rather than wait for the new protocol to exist.
 *
 * What they do not have is as important as what they do:
 *
 * - **No lifecycle.** A generation is never announced, so every entity comes
 *   out of the identity store with `generationUnknown`. Two lifetimes of one
 *   address are two entities only when a delete or a restart says so.
 * - **No sequence.** A missing record is invisible. The memory ring counts what
 *   it overwrote; the SWO stream counts nothing at all.
 * - **No ISR, fault or heap.** Only switches.
 *
 * The capabilities they report are what makes the rules above return
 * `NotApplicable` instead of a confident zero.
 */

/* SWO/ITM: bytes in, records out, and the framing lives in `RtosItmDecoder`. */
class MtfTraceAdapter final : public IRtosTraceSource
{
   public:
	/* Bytes as the probe hands them over, in any sizes. A record that arrives
	   in pieces is held until the last piece comes. */
	void feed(const uint8_t* bytes, size_t size);

	/* Records from somewhere else: a replay, a test. Same path from here on. */
	void pushRecords(const std::vector<RtosEventRing::Record>& records);

	bool start(RtosTraceProblem& problem) override;
	void stop() override;
	void requestNewEpoch() override;

	RtosTraceCapabilities capabilities() const override;
	RtosTraceSourceState state() const override;

	const char* name() const override
	{
		return "mtf";
	}

	RtosTraceBatch poll(double budgetSeconds) override;

   private:
	RtosItmDecoder decoder;

	std::deque<RtosEventRing::Record> pending;

	uint64_t cursor = 0;
	uint64_t epoch = 1;
	RtosTraceSourceState sourceState = RtosTraceSourceState::NotStarted;
	bool newEpochWanted = false;
};

/*
 * The memory ring: records the target keeps, read by the caller.
 *
 * The reading itself is not here - it needs the probe, and the probe belongs to
 * the sampling path. What is here is what happens to a batch of records once
 * they have been read: switches become events, `lost` becomes a gap rather than
 * a longer run, and a reader that was put back to zero becomes a new epoch
 * rather than a continuation.
 */
class SwdTraceAdapter final : public IRtosTraceSource
{
   public:
	void pushRecords(const std::vector<RtosEventRing::Record>& records, uint32_t lost, bool restarted);
	void pushRecords(const std::vector<RtosEventRing::Record>& records);

	bool start(RtosTraceProblem& problem) override;
	void stop() override;
	void requestNewEpoch() override;

	RtosTraceCapabilities capabilities() const override;
	RtosTraceSourceState state() const override;

	const char* name() const override
	{
		return "swd";
	}

	RtosTraceBatch poll(double budgetSeconds) override;

   private:
	std::deque<RtosTraceEvent> pending;

	uint64_t cursor = 0;
	uint64_t epoch = 1;
	uint32_t lostSoFar = 0;
	RtosTraceSourceState sourceState = RtosTraceSourceState::NotStarted;
};

#endif
