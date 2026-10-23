#ifndef _RTOSTRACEPIPELINE_HPP
#define _RTOSTRACEPIPELINE_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "IRtosTraceSource.hpp"
#include "RtosIdentityStore.hpp"
#include "RtosTraceClock.hpp"
#include "RtosTraceTypes.hpp"

/*
 * The bridge between a source of events and a history that can be asked questions.
 *
 * A source hands over bytes decoded into events. That is not yet something the
 * interface can use: the events carry handles, not identities, and ticks, not
 * seconds. This is where the difference is made up - once, in one place, so
 * that the display and the rules cannot each do it their own way and disagree.
 *
 * Three steps, in this order, and the order matters:
 *
 * 1. **Tick extension** happens first, because everything downstream compares
 *    times and a value that wrapped is not comparable.
 * 2. **Identity** comes next: an event belongs to a lifetime of a handle, and
 *    which lifetime is decided by the store alone.
 * 3. **Display time** is derived last, from the extended ticks and the
 *    frequency in force. It is empty when it cannot be computed - never a zero
 *    that would sit on an axis at the start of the session.
 *
 * A batch that cannot be committed is dropped whole: its events were decoded
 * against a record that was changing, and half of them would be a history that
 * never happened.
 */
struct RtosNormalizedEvent
{
	RtosTraceEvent event;

	/* Which lifetime of which entity this belongs to. `kind == None` when the
	   event has no subject (or no object). */
	RtosEntityKey subject;
	RtosEntityKey object;

	/* The identity of the subject could not be pinned: the lifetime was never
	   announced, or a gap swallowed the announcement. */
	bool subjectGenerationUnknown = false;

	/* A lifetime opened or closed across a gap, so the counts around it are
	   not usable. */
	bool afterGap = false;

	/* Seconds on the session's axis, from the first event that carried a
	   target time. Absent when the source has no clock or the frequency is not
	   known for the span - which is not the same as "at time zero". */
	bool hasDisplaySeconds = false;
	double displaySeconds = 0.0;
};

class RtosTracePipeline
{
   public:
	/* The most events kept. A stream that produces more than this is either
	   badly misconfigured or a good reason to record to a file instead; either
	   way the history stops growing and says how much it dropped. */
	static constexpr size_t eventLimit = 200000;

	explicit RtosTracePipeline(std::unique_ptr<IRtosTraceSource> source);
	~RtosTracePipeline();

	RtosTracePipeline(const RtosTracePipeline&) = delete;
	RtosTracePipeline& operator=(const RtosTracePipeline&) = delete;

	bool start(RtosTraceProblem& problem);
	void stop();

	bool attached() const
	{
		return traceSource != nullptr;
	}

	/* What one pump did. `applied` counts events that went into the history,
	   which is zero for a batch that could not be committed. */
	struct Pump
	{
		size_t applied = 0;
		bool committed = false;
		bool torn = false;
		bool readFailed = false;
		uint32_t unknownRecords = 0;
		uint32_t gaps = 0;
		uint32_t droppedForSize = 0;
		uint64_t epoch = 0;
	};

	Pump pump(double budgetSeconds);

	/* The acquisition thread's switch records: handed to the source if it takes
	   them, then drained in the same call. One place, because the caller is the
	   sampling loop and it has exactly one moment to spend here. */
	size_t pushSwitchRuns(const std::vector<RtosEventRing::Interval>& runs);

	const std::vector<RtosNormalizedEvent>& events() const
	{
		return history;
	}

	size_t eventCount() const
	{
		return history.size();
	}

	const RtosIdentityStore& identity() const
	{
		return identities;
	}

	const RtosTraceClock& clock() const
	{
		return traceClock;
	}

	RtosTraceCapabilities capabilities() const
	{
		return traceSource == nullptr ? RtosTraceCapabilities{} : traceSource->capabilities();
	}

	RtosTraceSourceState state() const
	{
		return traceSource == nullptr ? RtosTraceSourceState::NotStarted : traceSource->state();
	}

	const char* sourceName() const
	{
		return traceSource == nullptr ? "none" : traceSource->name();
	}

	uint64_t currentEpoch() const
	{
		return epoch;
	}

	/*
	 * The capabilities this build would like to have and the source does not.
	 *
	 * Named rather than counted: "no ISR events" is something a reader can act
	 * on, "three bits clear" is not. The interface shows this as the reason a
	 * lane or a rule is missing, which is the difference between an empty lane
	 * that means "the target has no interrupts" and one that means "this
	 * carrier does not carry them".
	 */
	std::vector<std::string> missingCapabilities() const;

	/* The events of one lifetime, oldest first. Pointers stay valid until the
	   next pump that appends; a caller that keeps them across a pump is asking
	   for a dangling pointer and the type cannot stop it. */
	std::vector<const RtosNormalizedEvent*> eventsFor(const RtosEntityKey& key) const;

	void clearHistory();

   private:
	std::unique_ptr<IRtosTraceSource> traceSource;

	RtosIdentityStore identities;
	RtosTraceClock traceClock;

	std::vector<RtosNormalizedEvent> history;

	uint64_t epoch = 0;
	uint64_t firstTicks = 0;
	bool haveFirstTicks = false;
	uint32_t droppedTotal = 0;
};

#endif
