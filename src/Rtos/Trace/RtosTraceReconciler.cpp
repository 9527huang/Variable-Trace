#include "RtosTraceReconciler.hpp"

#include <algorithm>

const char* toString(RtosReconcileFinding::Kind kind)
{
	switch (kind)
	{
		case RtosReconcileFinding::Kind::SnapshotTaskNotInEvents:
			return "snapshot-task-not-in-events";

		case RtosReconcileFinding::Kind::EventEntityNotInSnapshot:
			return "event-entity-not-in-snapshot";

		case RtosReconcileFinding::Kind::NameDiffers:
			return "name-differs";

		default:
			return "unknown";
	}
}

std::vector<RtosReconcileFinding> RtosTraceReconciler::compare(const RtosSnapshot& snapshot, const RtosIdentityStore& identity, uint64_t epoch)
{
	std::vector<RtosReconcileFinding> findings;

	/* The snapshot's view first: a task it can see and the events never
	   mentioned. Right after attaching that is simply "the creation happened
	   before we started", which is why it is a finding and not an error. */
	for (const RtosTask& task : snapshot.tasks)
	{
		if (task.tcbAddress == 0)
			continue;

		const RtosIdentityStore::Entity* known = identity.openEntityFor(RtosEntityKind::Task, task.tcbAddress);

		if (known == nullptr)
		{
			RtosReconcileFinding finding;
			finding.kind = RtosReconcileFinding::Kind::SnapshotTaskNotInEvents;
			finding.tcbAddress = task.tcbAddress;
			finding.key.epoch = epoch;
			finding.key.kind = RtosEntityKind::Task;
			finding.key.rawHandle = task.tcbAddress;
			finding.snapshotName = task.name;

			findings.push_back(finding);

			continue;
		}

		/* Both know it. If they call it different things, that is worth saying
		   and not worth fixing: the snapshot's name is the present, and the
		   event's name is what it was called then. */
		if (!task.name.empty() && !known->name.empty() && task.name != known->name)
		{
			RtosReconcileFinding finding;
			finding.kind = RtosReconcileFinding::Kind::NameDiffers;
			finding.tcbAddress = task.tcbAddress;
			finding.key = known->key;
			finding.snapshotName = task.name;
			finding.eventName = known->name;

			findings.push_back(finding);
		}
	}

	/* Then the other direction: a lifetime the events opened and this round did
	   not show. A round that missed a task is normal - the table says "/" and
	   the event history says it exists - so this is reported as a fact, not as
	   a deletion. A closed lifetime is not reported at all: it ended, and a
	   round that does not show it is right. */
	for (const auto& entry : identity.all())
	{
		if (entry.second.closed || entry.first.kind != RtosEntityKind::Task)
			continue;

		if (snapshot.find(entry.first.rawHandle) != nullptr)
			continue;

		RtosReconcileFinding finding;
		finding.kind = RtosReconcileFinding::Kind::EventEntityNotInSnapshot;
		finding.tcbAddress = entry.first.rawHandle;
		finding.key = entry.first;
		finding.eventName = entry.second.name;

		findings.push_back(finding);
	}

	/* Stable order, so that a list of them does not reshuffle between rounds
	   while the target is not doing anything. */
	std::sort(findings.begin(), findings.end(), [](const RtosReconcileFinding& left, const RtosReconcileFinding& right)
	          {
		          if (left.tcbAddress != right.tcbAddress)
			          return left.tcbAddress < right.tcbAddress;

		          return static_cast<int>(left.kind) < static_cast<int>(right.kind);
	          });

	return findings;
}
