#ifndef _RTOSSNAPSHOT_HPP
#define _RTOSSNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosSyncEnumerator.hpp"
#include "RtosTaskEnumerator.hpp"

/*
 * How far a published snapshot can be trusted.
 *
 * Validation cannot make reading a running target atomic. The item counts can
 * agree before and after, every link can point where it should, and the round
 * can still have caught one task being deleted and another created in its
 * place. What the checks buy is a level rather than a guarantee, so the level
 * travels with the data instead of being decided wherever the data is shown.
 *
 * There is no "inconsistent" value here on purpose. A round that fails its
 * checks is not published at all: the previous snapshot stays, and the model
 * records that a round went missing. A half snapshot presented as a whole one
 * is what the level exists to prevent.
 */
enum class RtosConsistency
{
	/* Read while the target was stopped, or from a snapshot mechanism the
	   target itself provides. */
	Consistent,
	/* Read while the target ran. This is what a round can claim, and it holds
	   whether or not the round also caught the target moving: the places
	   where it did are listed in `inconsistencies`, one by one, rather than
	   being folded into this level. A round that could not be read at all is
	   not published, so it never reaches here. */
	BestEffort,
};

inline const char* toString(RtosConsistency consistency)
{
	switch (consistency)
	{
		case RtosConsistency::Consistent:
			return "consistent";

		case RtosConsistency::BestEffort:
		default:
			return "read while running";
	}
}

/*
 * How old one layer of the data is.
 *
 * Fields refresh at different rates: the current task pointer can be polled
 * every round while names and stack ranges are read once a second. A snapshot
 * carrying one age would have to claim the slowest layer's age for everything
 * or hide the difference, so each layer carries its own.
 */
struct RtosLayerAge
{
	std::string layer;
	double ageSeconds = 0.0;
};

/*
 * One round of task state, complete and immutable.
 *
 * The collector builds a whole new one each round and swaps it in. Nothing
 * modifies a snapshot after it was published, which is what lets the interface
 * read it every frame without a lock and without ever seeing half a round.
 */
/*
 * What the stack sweep holds, and how far it can be trusted.
 *
 * The water marks are read a few tasks at a time, so a snapshot carries
 * readings of several ages and covers part of the set. Both are part of the
 * answer rather than defects to hide: a column of numbers with no hint of when
 * they were taken invites a decision they cannot support.
 */
struct RtosStackScanSummary
{
	/* The sweep has run at least once. Before that there is nothing to say
	   about stacks, and an empty column is what is expected. */
	bool enabled = false;

	/* The fill pattern was found in some stack, so a free count means what it
	   says. False while readings exist says the kernel fills nothing: a build
	   option rather than a fault in the target, so it is reported plainly and
	   not as a problem, and the column claims nothing. */
	bool fillSeen = false;

	/* How many tasks this snapshot carries a reading for, and the age of the
	   oldest of them. */
	uint32_t tasksScanned = 0;
	double oldestReadingAgeSeconds = 0.0;
};


struct RtosSnapshot
{
	std::vector<RtosTask> tasks;

	/* The queues, semaphores and mutexes the kernel's registry knows about.
	   Empty when the kernel has no registry, which is a build option rather
	   than a failure: `problems` is where a failure would be said. */
	std::vector<RtosSyncObject> syncObjects;

	/* Whether the kernel keeps a registry at all.
	 *
	 * An empty `syncObjects` has two causes that look identical from outside:
	 * a kernel built with the registry switched off, which can never list its
	 * queues, and one whose registry is simply empty because nothing was
	 * registered. A caller that showed the first as "no objects" would be
	 * reporting a build option as a fact about the target. */
	bool queueRegistryAvailable = false;

	std::vector<RtosLayerAge> layerAges;

	/* The stack water marks: how much of each stack is free, and how much of
	   the set this round managed to cover. */
	RtosStackScanSummary stackScan;

	/* One sentence per thing that went wrong while the round was collected.
	   A round can be usable and still have something worth saying: a list that
	   declares more items than it yielded, for instance. */
	std::vector<std::string> problems;

	/*
	 * Places where the round caught the target moving.
	 *
	 * An entry here does not mean a wrong reading. It means two parts of the
	 * snapshot were taken at different moments, which a snapshot of a running
	 * system cannot avoid: the round walks eight lists one after another,
	 * while the target moves tasks between them.
	 *
	 * It is kept apart from `problems` and carried on the snapshot rather
	 * than used to withhold it, because what to do about it depends on the
	 * question being asked. A curve of queue depths does not care. A claim
	 * about which task held a mutex at one instant does.
	 */
	std::vector<std::string> inconsistencies;

	RtosConsistency consistency = RtosConsistency::BestEffort;

	/* How many kernel lists the round walked. Reported because a walk that
	   silently found nothing looks exactly like a walk over an empty list, and
	   the only way to tell the two apart from outside is to count them. */
	uint32_t listsWalked = 0;
	uint32_t listsFailed = 0;

	/* Seconds on a monotonic clock, matching how the rest of the program
	   counts time.

	   These three are not decoration. A round that had to be split to stay
	   inside the probe budget is finished over a span of wall clock, and a
	   snapshot that showed one timestamp would claim the pieces came from one
	   moment. */
	double captureStartedAt = 0.0;
	double captureFinishedAt = 0.0;
	double captureSpanSeconds = 0.0;

	std::string sessionId;
	uint64_t sequence = 0;

	const RtosTask* find(uint32_t tcbAddress) const
	{
		for (const RtosTask& task : tasks)
		{
			if (task.tcbAddress == tcbAddress)
				return &task;
		}

		return nullptr;
	}

	size_t taskCount() const
	{
		return tasks.size();
	}

	size_t syncObjectCount() const
	{
		return syncObjects.size();
	}

	bool empty() const
	{
		return tasks.empty();
	}

	/* True when every task in the round was found on a list. A task on no list
	   is reported as unknown rather than left out, so one of those means the
	   round cannot claim to have accounted for the whole kernel. */
	bool everyTaskWasOnAList() const
	{
		for (const RtosTask& task : tasks)
		{
			if (task.state == RtosTaskState::Unknown)
				return false;
		}

		return true;
	}
};

#endif
