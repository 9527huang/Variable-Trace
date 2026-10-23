#include "RtosTracePipeline.hpp"

#include <string>

RtosTracePipeline::RtosTracePipeline(std::unique_ptr<IRtosTraceSource> source) : traceSource(std::move(source))
{
}

RtosTracePipeline::~RtosTracePipeline()
{
	stop();
}

bool RtosTracePipeline::start(RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	if (traceSource == nullptr)
	{
		problem = RtosTraceProblem::NotAChoice;

		return false;
	}

	if (!traceSource->start(problem))
	{
		/* A source that half started is a probe nobody can take back: close it
		   here rather than leave the next caller to find it busy. */
		traceSource->stop();

		return false;
	}

	clearHistory();

	return true;
}

void RtosTracePipeline::stop()
{
	if (traceSource != nullptr)
		traceSource->stop();
}

RtosTracePipeline::Pump RtosTracePipeline::pump(double budgetSeconds)
{
	Pump result;

	if (traceSource == nullptr)
		return result;

	const RtosTraceBatch batch = traceSource->poll(budgetSeconds);

	result.unknownRecords = batch.unknownRecords;

	/* Nothing from a batch that cannot be committed. Not "the good part of
	   it": the events were decoded against a source that then turned out to be
	   moving, and a history built from half of them is a history that never
	   happened. */
	if (!batch.canCommit())
	{
		result.torn = batch.torn;
		result.readFailed = batch.readFailed;

		return result;
	}

	result.committed = true;

	for (const RtosTraceEvent& incoming : batch.events)
	{
		RtosNormalizedEvent normalized;
		normalized.event = incoming;

		if (incoming.type == RtosTraceEventType::EpochBegin && incoming.epoch != epoch)
		{
			/* A new epoch closes everything: the same address is a different
			   entity, and a generation counted in the old epoch means nothing
			   here. */
			epoch = incoming.epoch;
			identities.clear();
			traceClock.reset();
			haveFirstTicks = false;
		}

		if (incoming.epoch != 0)
			epoch = incoming.epoch;

		normalized.event.epoch = epoch;

		const uint64_t ticks = incoming.hasTargetTicks ? traceClock.extend(incoming.targetTicks) : 0;

		/* The frequency the event was running at, and then any explicit sync.
		   Both are how the clock learns its intervals; without one of them a
		   duration is not computable and the display time stays absent rather
		   than defaulting to zero. */
		if (incoming.hasTargetTicks && incoming.targetHz != 0)
			traceClock.noteFrequency(ticks, incoming.targetHz);

		if (incoming.type == RtosTraceEventType::ClockSync && incoming.arg0 != 0)
			traceClock.noteFrequency(ticks, incoming.arg0);

		if (incoming.hasTargetTicks)
		{
			if (!haveFirstTicks)
			{
				firstTicks = ticks;
				haveFirstTicks = true;
			}

			if (const std::optional<double> seconds = traceClock.secondsBetween(firstTicks, ticks))
			{
				normalized.hasDisplaySeconds = true;
				normalized.displaySeconds = *seconds;
			}
		}

		if (incoming.type == RtosTraceEventType::Gap)
		{
			identities.noteGap();
			result.gaps++;
		}

		if (incoming.subject.kind != RtosEntityKind::None && incoming.subject.rawHandle != 0)
		{
			const RtosIdentityStore::Outcome outcome = identities.observe(incoming.subject, incoming.type, epoch);

			normalized.subject = outcome.key;
			normalized.subjectGenerationUnknown = outcome.openedWithoutCreation || outcome.implicitClose;

			if (const RtosIdentityStore::Entity* entity = identities.find(outcome.key))
				normalized.subjectGenerationUnknown = entity->generationUnknown;

			normalized.afterGap = outcome.afterGap;
		}

		if (incoming.object.kind != RtosEntityKind::None && incoming.object.rawHandle != 0)
		{
			/* Objects get the same treatment; a wait on an object nobody
			   announced is an entity whose generation is a guess, and it must
			   look like one rather than like a fresh first generation. */
			const RtosIdentityStore::Outcome outcome = identities.observe(incoming.object, incoming.type, epoch);

			normalized.object = outcome.key;
		}

		history.push_back(normalized);
		result.applied++;
	}

	/* The cap is on the history, not on the stream: what came in is kept until
	   there is too much of it, and then the oldest goes. A ring of the oldest
	   is worse than this - a capture is read from the start. */
	if (history.size() > eventLimit)
	{
		const size_t excess = history.size() - eventLimit;

		history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(excess));
		droppedTotal += static_cast<uint32_t>(excess);
	}

	result.droppedForSize = droppedTotal;
	result.epoch = epoch;

	return result;
}

std::vector<const RtosNormalizedEvent*> RtosTracePipeline::eventsFor(const RtosEntityKey& key) const
{
	std::vector<const RtosNormalizedEvent*> found;

	if (key.rawHandle == 0)
		return found;

	for (const RtosNormalizedEvent& entry : history)
	{
		if (entry.subject == key || entry.object == key)
			found.push_back(&entry);
	}

	return found;
}

void RtosTracePipeline::clearHistory()
{
	history.clear();
	identities.clear();
	traceClock.reset();
	epoch = 0;
	firstTicks = 0;
	haveFirstTicks = false;
	droppedTotal = 0;
}

std::vector<std::string> RtosTracePipeline::missingCapabilities() const
{
	std::vector<std::string> missing;

	const RtosTraceCapabilities available = capabilities();

	/* Bits zero to ten are the ones the documents name. The reserved ones are
	   not reported: a source is not "missing" something that does not exist
	   yet. */
	for (uint32_t bit = 0; bit <= 10; bit++)
	{
		if (!available.has(bit))
			missing.emplace_back(capabilityName(bit));
	}

	return missing;
}

size_t RtosTracePipeline::pushSwitchRuns(const std::vector<RtosEventRing::Interval>& runs)
{
	if (traceSource == nullptr)
		return 0;

	traceSource->pushSwitchRuns(runs);

	/* Drained at once rather than on the next tick: the caller has just read
	   the ring and will not come back for a while, and leaving the events in
	   the source would put them under whatever epoch the next tick has. */
	return pump(0.0).applied;
}
