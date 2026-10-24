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

	/* A history is there to read either because a source is filling it or
	   because a recording put it back. Both answer the same questions, and the
	   interface should not have to know which - only `isReplay` says so, for the
	   places that must not offer to keep recording. */
	bool attached() const
	{
		return traceSource != nullptr || replaying;
	}

	/* True when the history came out of a file rather than off a probe. */
	bool isReplay() const
	{
		return replaying;
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

	/*
	 * Sets the moment the event clock counts from.
	 *
	 * The event clock starts at the first event that arrives, which is a fine
	 * answer for a recording and a wrong one beside a strip that counts from the
	 * moment acquisition began: the target was running before that, so the two
	 * axes differ by however long it had been up. A source that can say what the
	 * target's counter reads right now - the ring can - calls this before the
	 * first batch is pumped, and the two axes then agree to within one ring.
	 *
	 * Ignored once there is history: re-timing events that a reader has already
	 * seen move under them is worse than an axis that starts late.
	 */
	void noteTimeOrigin(uint32_t rawTicks)
	{
		if (!history.empty())
			return;

		/* Extended, because everything else in here is: the clock folds a 32 bit
		   counter into a 64 bit one so that a session longer than the counter's
		   period does not jump backwards. */
		firstTicks = traceClock.extend(rawTicks);
		haveFirstTicks = true;
	}

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
		if (replaying)
			return replayCapabilities;

		return traceSource == nullptr ? RtosTraceCapabilities{} : traceSource->capabilities();
	}

	RtosTraceSourceState state() const
	{
		return traceSource == nullptr ? RtosTraceSourceState::NotStarted : traceSource->state();
	}

	const char* sourceName() const
	{
		if (replaying)
			return replayName.c_str();

		return traceSource == nullptr ? "none" : traceSource->name();
	}

	/* The source itself, for the diagnostics that only one carrier can answer:
	 * the RTEC2 ring's head and its torn batches are not part of the interface
	 * because a source that does not read a ring has neither. */
	IRtosTraceSource* source() const
	{
		return traceSource.get();
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

	/*
	 * Puts a recorded history back.
	 *
	 * Everything that was decided while the session ran is handed over as it was
	 * written - the events, the identities their keys point at, and what the
	 * source said it could do - because re-deciding any of it here would produce
	 * a second answer to a question the file already answers.
	 *
	 * Replaces whatever the pipeline held: a replay and a live session are never
	 * one history.
	 */
	void restore(std::vector<RtosNormalizedEvent> events, const RtosIdentityStore& identities, const RtosTraceSourceRecord& source);

	/*
	 * The events that arrived since the last call, for writing them down.
	 *
	 * The cursor is the caller's, in events rather than in indexes: the history
	 * drops its oldest entries when it is full, so an index taken now means a
	 * different event later. A cursor that fell behind the retained window is
	 * moved forward and the events in between are not returned - they are gone,
	 * and pretending otherwise would write them down twice.
	 */
	std::vector<RtosNormalizedEvent> eventsSince(uint64_t& cursor) const;

	/* How many events have been applied in this history. A caller that starts
	   writing a recording now takes this as its first cursor, so the file begins
	   at the moment recording began rather than at the start of the session. */
	uint64_t appended() const
	{
		return appendedTotal;
	}

   private:
	std::unique_ptr<IRtosTraceSource> traceSource;

	RtosIdentityStore identities;
	RtosTraceClock traceClock;

	std::vector<RtosNormalizedEvent> history;

	uint64_t epoch = 0;
	uint64_t firstTicks = 0;
	bool haveFirstTicks = false;
	uint32_t droppedTotal = 0;

	/* Every event this pipeline has ever applied, including the ones the limit
	   has since dropped. A count rather than a size, so that a cursor survives
	   the trimming. */
	uint64_t appendedTotal = 0;

	/* Set while a recording is being read: there is no source, and the two
	   answers a source would give are held here instead. */
	bool replaying = false;
	std::string replayName;
	RtosTraceCapabilities replayCapabilities;
};

#endif
