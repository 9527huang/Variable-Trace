#ifndef _RTOSTASKENUMERATOR_HPP
#define _RTOSTASKENUMERATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "RtosListWalker.hpp"
#include "RtosProfileBase.hpp"

/*
 * What a task is doing, in one vocabulary.
 *
 * Nothing above this line knows how a kernel spells its states. A profile says
 * which list means what, and a task lands in one of these seven by the list its
 * state item was found on. A task on no list at all is reported as unknown
 * rather than guessed at, because a guess there is indistinguishable from an
 * answer.
 */
enum class RtosTaskState
{
	Running,
	Ready,
	Blocked,
	Suspended,
	/* Ready, but the scheduler is suspended so nothing is running yet. */
	PendingReady,
	Deleted,
	Unknown,
};

const char* toString(RtosTaskState state);

/*
 * One scale for both directions.
 *
 * FreeRTOS numbers priorities upwards and Zephyr downwards, and a table that
 * showed raw numbers would have a reader misjudge one kernel after working on
 * the other. Ordering lanes and answering "is A above B" go through this, so
 * the direction stops at the profile layer.
 *
 * The result is signed on purpose: a downward numbering has to be able to sit
 * below zero so that the ranking is a plain comparison in either case.
 */
int64_t normalizedPriority(uint32_t rawPriority, bool higherNumberIsHigherPriority);

struct RtosTask
{
	uint32_t tcbAddress = 0;
	uint32_t stateListItem = 0;

	/*
	 * The list the state item is on.
	 *
	 * Every list item points back at the list that holds it, and this is that
	 * pointer for the item the walk read. It is always one of the kernel's own
	 * lists - ready, delayed, suspended, terminated - because that is what a
	 * state item is for. Which object a task waits on is not in this one; it is
	 * in the event item, below.
	 */
	uint32_t waitingList = 0;

	/*
	 * The object wait list the event item is on, or zero when the task waits on
	 * no object at all.
	 *
	 * A kernel puts a task that blocks on a queue, semaphore or mutex into two
	 * places at once: its state item onto one of the kernel's own lists, and its
	 * event item onto the object's wait list. So only the event item can answer
	 * what a task is waiting for, and only the control block holds it - which is
	 * why a round that skipped the control blocks has no answer here and says so
	 * through `hasEventList` instead of through a zero.
	 *
	 * A zero is a real answer here, and a load bearing one: read together with a
	 * state item on the suspended list, it is how the kernel itself tells a
	 * suspended task from one blocked with no timeout in sight.
	 */
	uint32_t eventList = 0;
	bool hasEventList = false;

	/* The name of the object it is waiting on. Empty means no synchronisation
	   object was named for it - either it waits on none, or it waits on one the
	   kernel keeps no registry entry for. */
	std::string waitingOn;

	RtosTaskState state = RtosTaskState::Unknown;

	uint32_t rawPriority = 0;
	/* False for a task that was only ever seen on a list that does not carry
	   the priority. Zero would otherwise be read as "the lowest priority". */
	bool hasPriority = false;
	int64_t priorityRank = 0;

	/* On a delay list this is the tick the task is due to wake at. */
	uint32_t waitValue = 0;
	bool hasWaitTime = false;

	/* The fields below are not on any list: they live in the control block and
	   a round has to read it to have them. Each carries its own flag, because
	   a round that only walked the lists is a normal round and its zeros must
	   not be read as answers. */

	/* The name as the control block holds it, empty when it was not read. */
	std::string name;

	/* Lowest and highest address of the task stack. */
	uint32_t stackBase = 0;
	uint32_t stackTop = 0;
	bool hasStack = false;

	/* The priority the task was created with. Under priority inheritance this
	   differs from `rawPriority`, and that difference is the diagnostic. */
	uint32_t basePriority = 0;
	bool hasBasePriority = false;

	/* The kernel's own task number, which survives a task being deleted and
	   another taking its place. Needs configUSE_TRACE_FACILITY. */
	uint32_t kernelTaskNumber = 0;
	bool hasKernelTaskNumber = false;

	/* The key of the list the task was found on, so a caller can say which
	   list a state came from without knowing what a kernel calls it. */
	std::string sourceList;
};

struct RtosTaskEnumeration
{
	std::vector<RtosTask> tasks;

	/* One sentence per list that could not be walked, in the words
	   `RtosListWalker::describe` produced. */
	std::vector<std::string> problems;

	/*
	 * Places where what was read does not belong to one moment.
	 *
	 * These are not reads that failed. The target moved a node between the
	 * time one list was walked and the time the next one was, and the round
	 * caught the difference - a task on two lists, a list that declares more
	 * items than it yielded, a chain that led back into itself mid-change.
	 *
	 * Kept apart from `problems` because the two should end differently.
	 * What is in `problems` means the round cannot stand as a reading at all.
	 * What is here means it is a reading, and a good one, that is not
	 * strictly of one instant - which is exactly the distinction the
	 * consistency level on the snapshot exists to carry.
	 */
	std::vector<std::string> inconsistencies;

	/* False only when something could not be read, or a field the walk needs
	   has no offset. A round that merely caught the target moving stays
	   complete: it is published, and marked. */
	bool complete = true;
	uint32_t listsWalked = 0;
	uint32_t listsFailed = 0;

	/* A control block seen on more than one list. A kernel never puts one
	   there, so seeing it means the snapshot caught a change in progress and
	   the round cannot be treated as a reading of one moment. */
	uint32_t duplicates = 0;

	const RtosTask* find(uint32_t tcbAddress) const;
};

class RtosTaskEnumerator
{
   public:
	/* One list to walk, and what being on it means. */
	struct ListSource
	{
		std::string key;
		uint32_t address = 0;
		RtosTaskState state = RtosTaskState::Unknown;
		/* A ready list keeps the priority in the item value. */
		bool carriesPriority = false;
		/* A delay list keeps the wake tick there. */
		bool carriesWaitTime = false;
	};

	/*
	 * Walks every list and answers with one task per control block.
	 *
	 * `currentTcbAddress` is the value of the kernel's current task pointer,
	 * not its address. A ready task whose control block equals it is running,
	 * which is the only thing that separates the two states.
	 */
	static RtosTaskEnumeration enumerate(IRtosMemory& memory,
	                                     const RtosProfileBase::Resolution& layout,
	                                     const std::vector<ListSource>& sources,
	                                     uint32_t currentTcbAddress,
	                                     bool higherNumberIsHigherPriority,
	                                     const RtosWalkLimits& limits = {});

	/*
	 * Folds a second pass into the first.
	 *
	 * The kernel's own lists do not hold every task. One blocked on a queue or
	 * a semaphore has its state item on that object's wait list instead, and
	 * can only be found by walking those - which needs the objects, and the
	 * objects need a pass of their own. So the walk happens in two stages and
	 * the answers are joined here.
	 *
	 * A control block seen by both passes is kept once, as it was found by the
	 * first: that pass knew which lists carry a priority and a wait time, and
	 * the second one knows only that the task is blocked. Whichever lists
	 * contributed, one task is one row.
	 */
	static void absorb(RtosTaskEnumeration& into, const RtosTaskEnumeration& from);
};

#endif
