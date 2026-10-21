#include "RtosModel.hpp"

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
	}

	/* Kept before the snapshot is handed to the interface, so that a reader
	   which has the new round can also see it in the timeline. */
	timeline.add(*snapshot);

	/* And written down before it is handed over, for the same reason: what the
	   file holds is what the view was shown, one round at a time. */
	if (recording.isWriting())
		recording.write(*snapshot);

	published.store(std::move(snapshot));
}

bool RtosModel::startRecording(const std::string& path, RtosRecording::Problem& problem)
{
	problem = RtosRecording::Problem::None;

	if (playback)
	{
		problem = RtosRecording::Problem::AnotherRecordingIsOpen;

		return false;
	}

	return recording.begin(path, problem);
}

void RtosModel::stopRecording()
{
	recording.finish();
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

	for (size_t round = 0; round < rounds; round++)
	{
		std::shared_ptr<RtosSnapshot> snapshot = recording.roundAt(round);

		if (snapshot == nullptr)
			continue;

		timeline.add(*snapshot);
		last = std::move(snapshot);
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
