#include "TraceAdapters.hpp"

namespace
{
	/* The record this legacy path carries is a switch in: one task, and the
	   target's microsecond counter at the moment it happened. Microseconds are
	   a real target clock, so the event keeps them as ticks at 1 MHz rather
	   than pretending the host's arrival time is the target's. */
	constexpr uint32_t microsecondHz = 1000000u;

	RtosTraceEvent switchInFrom(const RtosEventRing::Record& record, uint64_t epoch)
	{
		RtosTraceEvent event;

		event.epoch = epoch;
		event.type = RtosTraceEventType::TaskSwitchIn;

		event.subject.kind = RtosEntityKind::Task;
		event.subject.rawHandle = record.tcbAddress;

		/* The address is the target's own 32 bit handle - there is nothing
		   inferred about it. What is missing is the generation, and that is the
		   store's call, not this one's. */
		event.subject.quality = record.tcbAddress == 0 ? RtosIdentityQuality::Unknown : RtosIdentityQuality::Exact;

		event.hasTargetTicks = true;
		event.targetTicks = record.timeUs;
		event.targetHz = microsecondHz;

		/* No sequence on this carrier, so the number is a local counter: it
		   orders the events and claims nothing about what the target sent. */
		event.quality = RtosEvidenceQuality::Good;

		return event;
	}

	RtosTraceBatch batchFrom(std::deque<RtosTraceEvent>& pending, uint64_t& cursor, RtosTraceSourceState state, RtosTraceCapabilities capabilities)
	{
		RtosTraceBatch batch;

		batch.cursorBefore = cursor;

		while (!pending.empty())
		{
			batch.events.push_back(pending.front());
			pending.pop_front();
			cursor++;
		}

		batch.cursorAfter = cursor;
		batch.state = state;
		batch.capabilities = capabilities;

		return batch;
	}

	RtosTraceCapabilities legacyCapabilities(bool lossDetection)
	{
		RtosTraceCapabilities capabilities;
		capabilities.set(0, true);   /* rawHandle32: the record carries a real address */
		capabilities.set(3, true);   /* targetClock: microseconds from the target */
		capabilities.set(5, lossDetection);
		capabilities.set(4, false);  /* no sequence number on either carrier */
		capabilities.set(1, false);  /* no lifecycle events */
		capabilities.set(2, false);  /* no generations */

		return capabilities;
	}
}  // namespace

void MtfTraceAdapter::feed(const uint8_t* bytes, size_t size)
{
	decoder.feed(bytes, size);

	for (const RtosEventRing::Record& record : decoder.take())
		pending.push_back(record);
}

void MtfTraceAdapter::pushRecords(const std::vector<RtosEventRing::Record>& records)
{
	for (const RtosEventRing::Record& record : records)
		pending.push_back(record);
}

bool MtfTraceAdapter::start(RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	decoder.clear();
	pending.clear();
	cursor = 0;
	sourceState = RtosTraceSourceState::Degraded;

	/* Degraded, not running: this carrier gives switches and nothing else. A
	   view built on it must say so rather than show empty ISR lanes as if the
	   target had no interrupts. */
	return true;
}

void MtfTraceAdapter::stop()
{
	sourceState = RtosTraceSourceState::Stopped;
}

void MtfTraceAdapter::requestNewEpoch()
{
	newEpochWanted = true;
}

RtosTraceCapabilities MtfTraceAdapter::capabilities() const
{
	return legacyCapabilities(false);
}

RtosTraceSourceState MtfTraceAdapter::state() const
{
	return sourceState;
}

RtosTraceBatch MtfTraceAdapter::poll(double budgetSeconds)
{
	(void)budgetSeconds;

	/* A new epoch on this carrier can only mean "start the numbering again":
	   the stream has no epoch of its own to ask for. It is recorded as an
	   event so that the model clears its identities at the right place. */
	if (newEpochWanted)
	{
		newEpochWanted = false;
		epoch++;

		RtosTraceBatch opening;
		opening.cursorBefore = cursor;

		RtosTraceEvent begin;
		begin.epoch = epoch;
		begin.type = RtosTraceEventType::EpochBegin;
		begin.quality = RtosEvidenceQuality::Good;
		opening.events.push_back(begin);
		cursor++;

		/* The queue follows it in the same batch: the epoch and the events it
		   governs must never be split across two pumps, or one of them lands
		   under an epoch that is not its own. */
		while (!pending.empty())
		{
			opening.events.push_back(switchInFrom(pending.front(), epoch));
			pending.pop_front();
			cursor++;
		}

		opening.cursorAfter = cursor;
		opening.state = sourceState;
		opening.capabilities = capabilities();

		return opening;
	}

	std::deque<RtosTraceEvent> events;

	for (const RtosEventRing::Record& record : pending)
		events.push_back(switchInFrom(record, epoch));

	pending.clear();

	return batchFrom(events, cursor, sourceState, capabilities());
}

void SwdTraceAdapter::pushRecords(const std::vector<RtosEventRing::Record>& records, uint32_t lost, bool restarted)
{
	if (restarted)
	{
		/* The target's ring went back to zero: a new run, with its own clock.
		   Continuing the old numbering would join two sessions. */
		epoch++;

		RtosTraceEvent begin;
		begin.epoch = epoch;
		begin.type = RtosTraceEventType::EpochBegin;
		begin.quality = RtosEvidenceQuality::Good;
		pending.push_back(begin);
	}

	if (lost > 0)
	{
		lostSoFar += lost;

		/* The gap goes in where it happened - before the records that survived
		   it - so that the interval across it is closed rather than stretched. */
		RtosTraceEvent gap;
		gap.epoch = epoch;
		gap.type = RtosTraceEventType::Gap;
		gap.arg0 = lost;
		gap.arg1 = static_cast<uint32_t>(RtosGapReason::RingOverwritten);
		gap.quality = RtosEvidenceQuality::Partial;
		pending.push_back(gap);
	}

	for (const RtosEventRing::Record& record : records)
		pending.push_back(switchInFrom(record, epoch));
}

void SwdTraceAdapter::pushSwitchRuns(const std::vector<RtosEventRing::Interval>& runs)
{
	for (const RtosEventRing::Interval& run : runs)
	{
		/* The runs were already placed on the session's clock by the caller, so
		   the event carries that moment as ticks at 1 MHz rather than inventing
		   a target counter nobody read. */
		RtosTraceEvent event;

		event.epoch = epoch;
		event.type = RtosTraceEventType::TaskSwitchIn;
		event.subject.kind = RtosEntityKind::Task;
		event.subject.rawHandle = run.tcbAddress;
		event.subject.quality = run.tcbAddress == 0 ? RtosIdentityQuality::Unknown : RtosIdentityQuality::Exact;
		event.hasTargetTicks = true;
		event.targetTicks = static_cast<uint32_t>(run.fromSeconds * 1000000.0);
		event.targetHz = microsecondHz;
		event.quality = RtosEvidenceQuality::Good;

		pending.push_back(event);
	}
}

void SwdTraceAdapter::pushRecords(const std::vector<RtosEventRing::Record>& records)
{
	pushRecords(records, 0, false);
}

bool SwdTraceAdapter::start(RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	pending.clear();
	cursor = 0;
	lostSoFar = 0;
	sourceState = RtosTraceSourceState::Degraded;

	return true;
}

void SwdTraceAdapter::stop()
{
	sourceState = RtosTraceSourceState::Stopped;
}

void SwdTraceAdapter::requestNewEpoch()
{
	/* Same as the ITM path: the ring has no epoch field, so the host numbers
	   them and clears its identities on the event. */
	epoch++;
}

RtosTraceCapabilities SwdTraceAdapter::capabilities() const
{
	return legacyCapabilities(true);
}

RtosTraceSourceState SwdTraceAdapter::state() const
{
	return sourceState;
}

RtosTraceBatch SwdTraceAdapter::poll(double budgetSeconds)
{
	(void)budgetSeconds;

	return batchFrom(pending, cursor, sourceState, capabilities());
}
