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


/*
 * What can be said about where the time went.
 *
 * A kernel only counts per task run time when it was built to. That is a
 * build option rather than a fault, so the three states are kept apart and
 * said plainly: no counter in the control block at all, a counter that is
 * there but not moving, and a working one. The middle state is worth naming
 * because a column of zeroes would read as "nothing is running".
 */
struct RtosCpuLoadSummary
{
	/* The target keeps a per task run time counter in its control block. */
	bool supported = false;

	/* Counters were read two rounds in a row and something moved. */
	bool counting = false;

	/* How many tasks this snapshot carries a share for, and the length of the
	   window every share was worked out over. The window is one number for all
	   of them by construction: a share is a fraction of what the tasks used
	   between the same two moments. */
	uint32_t tasksMeasured = 0;
	double windowSeconds = 0.0;
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

	/* Where the time went, when the target counts it. */
	RtosCpuLoadSummary cpuLoad;

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

	/*
	 * How many places a round may catch the target moving before it is worth
	 * saying so in a warning colour.
	 *
	 * The lists are walked one after another while the target moves tasks
	 * between them, so a few places where the reading caught it in the act are
	 * the normal state of a running target rather than a fault. A warning that
	 * is on every round is a warning nobody reads, which is worse than none:
	 * it costs the reader the one round where something really did go wrong.
	 *
	 * The limit grows with the round - a kernel with more lists can expect
	 * more of them to move - and has a floor, because a small round catching
	 * two or three things moving is worth a look on its own.
	 */
	uint32_t inconsistentPlaceLimit() const
	{
		const uint32_t share = listsWalked / placesPerList;

		return share > minimumInconsistentPlaces ? share : minimumInconsistentPlaces;
	}

	/* Whether this round moved in enough places that its parts should not be
	   read as one moment. */
	bool consistencyIsShaky() const
	{
		return inconsistencies.size() > inconsistentPlaceLimit();
	}

	/* One place per this many walked lists is still an ordinary round. */
	static constexpr uint32_t placesPerList = 4;

	static constexpr uint32_t minimumInconsistentPlaces = 3;

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
