#ifndef _RTOSMODEL_HPP
#define _RTOSMODEL_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "RtosSnapshot.hpp"
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

	mutable std::mutex modelMutex;
	Stats counters;
	std::string session;
	uint64_t nextSequence = 0;
};

#endif
