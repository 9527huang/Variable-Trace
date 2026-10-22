#ifndef _RTOSMODEL_HPP
#define _RTOSMODEL_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "RtosSnapshot.hpp"
#include "RtosEventHistory.hpp"
#include "RtosRecording.hpp"
#include "RtosTaskRoster.hpp"
#include "RtosTimeline.hpp"

/*
 * The one place a snapshot is published and the one place it is read.
 *
 * The collector builds a whole snapshot and hands it over; the interface asks
 * for the current one every frame. Neither waits for the other, and the
 * interface can never see a round half built.
 *
 * The model has a mutex of its own, and the rule about it is the one the plan
 * fixes: it is never taken while the variable model's lock is held, and the
 * variable model's lock is never taken while this one is held. The two sit on
 * the same acquisition path, and a nesting between them would be a deadlock
 * waiting for one wrong order to be used once.
 *
 * The lock is not for the published snapshot - that is an atomic pointer swap
 * and needs no help. It is for state that gets read, changed and written back:
 * the counters and the reason for the last skipped round, which have to be
 * seen as one set rather than as several values that happen to agree.
 */
class RtosModel
{
   public:
	struct Stats
	{
		uint64_t published = 0;
		uint64_t rejected = 0;
		std::string lastRejectionReason;
		double lastPublishedAt = 0.0;
		double lastRejectedAt = 0.0;
	};

	RtosModel() = default;

	RtosModel(const RtosModel&) = delete;
	RtosModel& operator=(const RtosModel&) = delete;

	/*
	 * Hands over a finished snapshot, which from here on belongs to the model.
	 *
	 * The sequence number and the session are stamped in, so every caller gets
	 * that for free and none of them can forget it. A null pointer is refused:
	 * publishing nothing would quietly clear whatever the interface is showing
	 * and leave it looking like a target with no tasks.
	 */
	void publish(std::shared_ptr<RtosSnapshot> snapshot);

	/* The most recent published snapshot, or nothing before the first one.
	   This takes no lock, which is the whole point of publishing a finished
	   object rather than sharing a container. */
	std::shared_ptr<const RtosSnapshot> current() const;

	/*
	 * Every round that was published, for the timeline.
	 *
	 * It lives here rather than with the sampling loop because this is the
	 * place that already knows a round was published: one that was refused
	 * never reaches the interface, so a timeline built anywhere else would
	 * have to work out which rounds count. The timeline keeps its own lock,
	 * because the interface reads it while the sampling thread writes it.
	 */
	const RtosTimeline& history() const
	{
		return timeline;
	}

	/* The runs the target reported, which the timeline draws instead of the
	   samples when the target keeps an event ring. */
	const RtosEventHistory& events() const
	{
		return eventHistory;
	}

	void appendEventIntervals(const std::vector<RtosEventRing::Interval>& intervals, double keepSeconds)
	{
		eventHistory.append(intervals, keepSeconds);

		if (!recording.isWriting())
			return;

		/* Held for the next round to carry into the file. The runs and the
		   round are produced by the same thread but not in a fixed order, so
		   the ones that arrive between two rounds travel with the second. */
		std::lock_guard<std::mutex> lock(modelMutex);

		pendingEvents.insert(pendingEvents.end(), intervals.begin(), intervals.end());
	}

	void noteLostEvents(uint32_t count)
	{
		eventHistory.noteLostRecords(count);
	}

	/* A new run keeps its own history: its clock starts at zero, and samples
	   from the previous one would sit on an axis that went back to the start. */
	void clearHistory()
	{
		timeline.clear();
		eventHistory.clear();
	}

	/*
	 * Records a round that failed its checks and was therefore not published.
	 *
	 * The previous snapshot is left alone, so what the interface shows stays a
	 * reading of one moment while the reason explains that a round went
	 * missing. Losing a round is not the same as losing the picture.
	 */
	void recordRejectedRound(const std::string& reason, double now);

	Stats stats() const;

	/* The identity every snapshot from this run carries, so that a saved
	   record can be told apart from the next one. */
	std::string sessionId() const;
	void beginSession(const std::string& sessionId);

	/*
	 * Every task this session has seen, in the order they were first found.
	 *
	 * The table is built from this rather than from the newest round, because
	 * a round can miss a task the kernel was still changing when it was read -
	 * and a row that appears and disappears twice a second cannot be read.
	 * A copy is handed out: the sampling thread adds to the roster while the
	 * interface walks it.
	 */
	std::vector<RtosTaskRoster::Entry> rosterEntries() const;

	size_t rosterSize() const;

	/*
	 * Writing a recording.
	 *
	 * The rounds are handed to the file from `publish`, which runs on the
	 * sampling thread: a recording that went through the interface would drop
	 * whichever rounds the interface happened not to draw.
	 */
	bool startRecording(const std::string& path, RtosRecording::Problem& problem);
	void stopRecording();

	bool isRecording() const
	{
		return recording.isWriting();
	}

	RtosRecording::Info recordingStats() const
	{
		return recording.stats();
	}

	/* Where it is going, for the line on screen. Set once by `startRecording`,
	   so reading it without the lock is reading a value that is not changing. */
	const std::string& recordingPath() const
	{
		return recording.path();
	}

	/*
	 * Reading one back.
	 *
	 * Opening fills the timeline from the file and shows the last round; the
	 * interface then moves through the rounds by asking for them. A recording
	 * and a live target are never shown at once: `beginSession` - which is
	 * what starting an acquisition does - ends the playback first.
	 */
	bool openRecording(const std::string& path, RtosRecording::Problem& problem);
	void closePlayback();

	bool isPlayback() const
	{
		return playback;
	}

	/* Whether the file on screen carries switch records. A recording written
	   before they were kept opens and looks complete, and its empty event list
	   means "not recorded" rather than "nothing switched". */
	bool playbackHasEventRecords() const
	{
		return playback && recording.hasEventRecords();
	}

	struct PlaybackInfo
	{
		std::string path;
		size_t rounds = 0;
		size_t current = 0;
		double firstSecond = 0.0;
		double lastSecond = 0.0;
	};

	PlaybackInfo playbackInfo() const;

	/* Shows one round of the recording. Out of range is ignored rather than
	   clamped silently: the interface asks for a round it just read the count
	   of, and a wrong index is a bug worth seeing. */
	void showPlaybackRound(size_t round);

	size_t nearestPlaybackRound(double atSeconds) const;

	/*
	 * One round of the recording, read straight from the file.
	 *
	 * `showPlaybackRound` moves the view to a round; this hands one over
	 * without moving anything, which is what writing the whole recording out
	 * needs: the exporter walks every round while the reader keeps looking at
	 * the one they are on.
	 *
	 * Null when this is not a playback, or the round is out of range, or the
	 * file stopped making sense. A round that cannot be decoded is worth a
	 * skipped line in an export, not an exception in the middle of one.
	 */
	struct PlaybackRound
	{
		std::shared_ptr<RtosSnapshot> snapshot;
		double atSeconds = 0.0;
	};

	PlaybackRound playbackRoundAt(size_t round) const;

	/*
	 * Builds a snapshot out of one enumeration.
	 *
	 * The collector uses this so that the fields the interface depends on are
	 * filled in one place. Whether the round is good enough to publish is the
	 * caller's decision, because only the caller knows whether the reason a
	 * walk failed makes the round unusable or merely incomplete.
	 */
	static std::shared_ptr<RtosSnapshot> fromEnumeration(const RtosTaskEnumeration& enumeration,
	                                                     const std::string& sessionId,
	                                                     double startedAt,
	                                                     double finishedAt,
	                                                     RtosConsistency consistency);

   private:
	std::atomic<std::shared_ptr<const RtosSnapshot>> published{nullptr};

	RtosTimeline timeline;
	RtosEventHistory eventHistory;
	RtosTaskRoster roster;

	/* Runs waiting for the next round to carry them into the file. Emptied on
	   every publish and on every stop, so a recording never opens with runs
	   that belong to the round before it. */
	std::vector<RtosEventRing::Interval> pendingEvents;

	/* The recording, in whichever of its two roles is active. Read and written
	   under `modelMutex` for its flags, and internally for the file itself. */
	mutable RtosRecording recording;
	bool playback = false;
	size_t playbackRound = 0;

	mutable std::mutex modelMutex;
	Stats counters;
	std::string session;
	uint64_t nextSequence = 0;
};

#endif
