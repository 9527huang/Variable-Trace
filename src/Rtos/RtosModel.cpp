#include "RtosModel.hpp"

void RtosModel::publish(std::shared_ptr<RtosSnapshot> snapshot)
{
	/* Refusing an empty publish is not defensive tidiness: storing it would
	   throw away a good snapshot and leave the interface showing an empty
	   task table, which reads as a target that has no tasks rather than as a
	   round that was lost. */
	if (snapshot == nullptr)
		return;

	{
		std::lock_guard<std::mutex> lock(modelMutex);

		snapshot->sequence = ++nextSequence;

		if (snapshot->sessionId.empty())
			snapshot->sessionId = session;

		counters.published++;
		counters.lastPublishedAt = snapshot->captureFinishedAt;
	}

	published.store(std::move(snapshot));
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
	std::lock_guard<std::mutex> lock(modelMutex);

	session = sessionId;
	nextSequence = 0;
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
