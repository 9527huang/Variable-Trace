#include "RtosModel.hpp"

void RtosModel::attachTraceSource(std::unique_ptr<IRtosTraceSource> source)
{
	/* Replacing a source stops the old one first: two sources on one probe is
	   the one thing the lease exists to prevent, and doing it quietly here
	   would make the lease a lie. */
	if (tracePipeline != nullptr)
		tracePipeline->stop();

	tracePipeline = std::make_unique<RtosTracePipeline>(std::move(source));
}

void RtosModel::publish(std::shared_ptr<RtosSnapshot> snapshot)
{
	/* Refusing an empty publish is not defensive tidiness: storing it would
	   throw away a good snapshot and leave the interface showing an empty
	   task table, which reads as a target that has no tasks rather than as a
	   round that was lost. */
	if (snapshot == nullptr)
		return;

	/* A live round reaching a view that is showing a recording would leave two
	   targets in one history. Starting an acquisition ends the playback, so
	   this only catches the case where something else drove the sampler. */
	if (playback)
		return;

	{
		std::lock_guard<std::mutex> lock(modelMutex);

		snapshot->sequence = ++nextSequence;

		if (snapshot->sessionId.empty())
			snapshot->sessionId = session;

		counters.published++;
		counters.lastPublishedAt = snapshot->captureFinishedAt;

		/* The roster grows with the run, not with the round: a round that could
		   not read a task must not take its row away. */
		roster.observe(*snapshot);
	}

	/* Kept before the snapshot is handed to the interface, so that a reader
	   which has the new round can also see it in the timeline. */
	timeline.add(*snapshot);

	/* And written down before it is handed over, for the same reason: what the
	   file holds is what the view was shown, one round at a time. The runs that
	   arrived since the last round go with it, so a file read back has lanes
	   and an event list rather than only the table. */
	if (recording.isWriting())
	{
		std::vector<RtosEventRing::Interval> events;

		{
			std::lock_guard<std::mutex> lock(modelMutex);
			events.swap(pendingEvents);
		}

		recording.write(*snapshot, events);

		/* The events behind the bands, the budget and the fault list. The runs
		   above say when a task held the CPU; these say what happened, which is
		   what every answer about interrupts is made from.

		   Written after the round, and only for the events that arrived since the
		   last one: a batch belongs to the round it was published with, so a file
		   read back has the same order the view had. */
		if (tracePipeline != nullptr)
		{
			recording.writeTraceEvents(tracePipeline->eventsSince(recordedEvents), sourceRecord());

			/* The dictionary goes in every so often as well as at the end, so
			   that a recording cut off by a crash still has names for its
			   events. Sixty four rounds is about half a minute. */
			if (snapshot->sequence % 64u == 0u)
				recording.writeIdentities(tracePipeline->identity());
		}
	}

	published.store(std::move(snapshot));
}

bool RtosModel::startRecording(const std::string& path, RtosRecording::Problem& problem, RtosRecording::Format format)
{
	problem = RtosRecording::Problem::None;

	if (playback)
	{
		problem = RtosRecording::Problem::AnotherRecordingIsOpen;

		return false;
	}

	{
		std::lock_guard<std::mutex> lock(modelMutex);
		pendingEvents.clear();
	}

	if (!recording.begin(path, problem, format))
		return false;

	/* Everything already applied belongs to the session before the file, so the
	   file starts here rather than at the beginning of the history. */
	recordedEvents = tracePipeline == nullptr ? 0 : tracePipeline->appended();

	return true;
}

void RtosModel::stopRecording()
{
	/* One last dictionary: the entities that appeared since the last periodic
	   one are the ones the final rounds refer to. */
	if (tracePipeline != nullptr && tracePipeline->attached())
		recording.writeIdentities(tracePipeline->identity());

	recording.finish();
	recordedEvents = 0;
}

RtosTraceSourceRecord RtosModel::sourceRecord() const
{
	RtosTraceSourceRecord record;

	if (tracePipeline == nullptr)
		return record;

	record.name = tracePipeline->sourceName();
	record.capabilities = tracePipeline->capabilities();

	return record;
}

bool RtosModel::openRecording(const std::string& path, RtosRecording::Problem& problem)
{
	problem = RtosRecording::Problem::None;

	if (recording.isWriting())
	{
		problem = RtosRecording::Problem::ARecordingIsBeingWritten;

		return false;
	}

	if (!recording.open(path, problem))
		return false;

	/* A recording is a new history: what was on the axis belonged to the
	   target that is no longer attached. */
	timeline.clear();
	eventHistory.clear();

	{
		std::lock_guard<std::mutex> lock(modelMutex);

		session = "recording";
		nextSequence = 0;
		playback = true;
		playbackRound = 0;

		/* A recording's roster is the whole file: a task that appears in one
		   round of it gets a row for the whole of it, so scrubbing does not add
		   and remove rows either. */
		roster.clear();
	}

	/*
	 * The events, before the rounds are walked: the history the bands and the
	 * budget are made from is the file's own, and putting it back here means the
	 * reader sees the same tracks offline as the session had on the bench.
	 *
	 * A file without them is not a session without events. The pipeline is left
	 * empty and not replaying, so the bands say "this recording does not carry
	 * them" rather than showing whatever the last live session left behind.
	 */
	const RtosRecording::TraceContent trace = recording.traceContent();

	if (trace.present)
	{
		if (tracePipeline == nullptr)
			tracePipeline = std::make_unique<RtosTracePipeline>(nullptr);

		tracePipeline->restore(trace.events, trace.identities, trace.source);
	}
	else if (tracePipeline != nullptr)
	{
		tracePipeline->clearHistory();
	}

	const size_t rounds = recording.roundCount();

	timeline.ensureCapacity(rounds);

	/* Every round goes into the timeline, because the lanes are the whole file:
	   a strip that stopped at the ring's capacity would describe the last
	   minute of a ten minute recording. This is the one expensive step of
	   opening - a decode per round - and the interface reports how long it
	   took, because on a long recording it is a second or two of a frozen
	   window. */
	std::shared_ptr<RtosSnapshot> last;

	/* Long enough for every run in the file: the history prunes by a window,
	   and a window shorter than the recording would drop the beginning of the
	   lanes the file was brought here to show. */
	const double keepSeconds = recording.info().durationSeconds() + 60.0;

	for (size_t round = 0; round < rounds; round++)
	{
		RtosRecording::Round content = recording.roundWithEvents(round);

		if (content.snapshot == nullptr)
			continue;

		timeline.add(*content.snapshot);

		/* Fed through the same append the live view uses, so the seam between
		   two rounds is joined here exactly as it was joined there. */
		if (!content.events.empty())
			eventHistory.append(content.events, keepSeconds);

		{
			std::lock_guard<std::mutex> lock(modelMutex);
			roster.observe(*content.snapshot);
		}

		last = std::move(content.snapshot);
	}

	if (last != nullptr)
	{
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			playbackRound = rounds == 0 ? 0 : rounds - 1;
		}

		published.store(std::move(last));
	}

	return true;
}

void RtosModel::closePlayback()
{
	if (!playback && !recording.isOpen())
		return;

	recording.close();

	{
		std::lock_guard<std::mutex> lock(modelMutex);

		playback = false;
		playbackRound = 0;
	}

	timeline.clear();
	eventHistory.clear();

	if (tracePipeline != nullptr)
		tracePipeline->clearHistory();

	{
		std::lock_guard<std::mutex> lock(modelMutex);
		roster.clear();
		pendingEvents.clear();
	}

	published.store(nullptr);
}

RtosModel::PlaybackInfo RtosModel::playbackInfo() const
{
	PlaybackInfo info;

	info.path = recording.path();
	info.rounds = recording.roundCount();

	{
		std::lock_guard<std::mutex> lock(modelMutex);
		info.current = playbackRound;
	}

	const RtosRecording::Info recordingInfo = recording.info();
	info.firstSecond = recordingInfo.firstSecond;
	info.lastSecond = recordingInfo.lastSecond;

	return info;
}

void RtosModel::showPlaybackRound(size_t round)
{
	if (!playback)
		return;

	std::shared_ptr<RtosSnapshot> snapshot = recording.roundAt(round);

	if (snapshot == nullptr)
		return;

	{
		std::lock_guard<std::mutex> lock(modelMutex);
		playbackRound = round;
	}

	published.store(std::move(snapshot));
}

size_t RtosModel::nearestPlaybackRound(double atSeconds) const
{
	if (!playback)
		return 0;

	return recording.nearestRound(atSeconds);
}

RtosModel::PlaybackRound RtosModel::playbackRoundAt(size_t round) const
{
	PlaybackRound result;

	if (!playback || round >= recording.roundCount())
		return result;

	result.snapshot = recording.roundAt(round);
	result.atSeconds = recording.atSeconds(round);

	return result;
}

std::shared_ptr<const RtosSnapshot> RtosModel::current() const
{
	return published.load();
}

void RtosModel::recordRejectedRound(const std::string& reason, double now)
{
	std::lock_guard<std::mutex> lock(modelMutex);

	counters.rejected++;
	counters.lastRejectionReason = reason;
	counters.lastRejectedAt = now;
}

RtosModel::Stats RtosModel::stats() const
{
	std::lock_guard<std::mutex> lock(modelMutex);

	return counters;
}

std::vector<RtosTaskRoster::Entry> RtosModel::rosterEntries() const
{
	std::lock_guard<std::mutex> lock(modelMutex);

	return roster.entries();
}

size_t RtosModel::rosterSize() const
{
	std::lock_guard<std::mutex> lock(modelMutex);

	return roster.size();
}

std::string RtosModel::sessionId() const
{
	std::lock_guard<std::mutex> lock(modelMutex);

	return session;
}

void RtosModel::beginSession(const std::string& sessionId)
{
	/* A new session is a new history: two runs drawn on one axis would be a
	   picture of neither. A recording being read is one of those runs, so
	   starting an acquisition puts the file away first. */
	closePlayback();

	/* And a recording being written belongs to the run it started in. The
	   clock restarts with a session, so rounds written on both sides of one
	   would put two runs on one axis in the *file* - and the file's whole
	   point is that its axis can be read back. */
	recording.finish();

	std::lock_guard<std::mutex> lock(modelMutex);

	session = sessionId;
	nextSequence = 0;

	timeline.clear();
	eventHistory.clear();
	roster.clear();
}

std::shared_ptr<RtosSnapshot> RtosModel::fromEnumeration(const RtosTaskEnumeration& enumeration,
                                                        const std::string& sessionId,
                                                        double startedAt,
                                                        double finishedAt,
                                                        RtosConsistency consistency)
{
	auto snapshot = std::make_shared<RtosSnapshot>();

	snapshot->tasks = enumeration.tasks;
	snapshot->problems = enumeration.problems;
	snapshot->sessionId = sessionId;
	snapshot->consistency = consistency;
	snapshot->captureStartedAt = startedAt;
	snapshot->captureFinishedAt = finishedAt;
	snapshot->captureSpanSeconds = finishedAt - startedAt;

	return snapshot;
}
