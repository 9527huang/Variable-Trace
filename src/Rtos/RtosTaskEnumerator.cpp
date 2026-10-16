#include "RtosTaskEnumerator.hpp"

#include <cstdio>
#include <utility>

namespace
{
	std::string hexOf(uint32_t value)
	{
		char buffer[16] = {};

		std::snprintf(buffer, sizeof(buffer), "%08X", value);

		return buffer;
	}
}  // namespace

const char* toString(RtosTaskState state)
{
	switch (state)
	{
		case RtosTaskState::Running:
			return "running";

		case RtosTaskState::Ready:
			return "ready";

		case RtosTaskState::Blocked:
			return "blocked";

		case RtosTaskState::Suspended:
			return "suspended";

		case RtosTaskState::PendingReady:
			return "pending ready";

		case RtosTaskState::Deleted:
			return "deleted";

		case RtosTaskState::Unknown:
		default:
			return "unknown";
	}
}

int64_t normalizedPriority(uint32_t rawPriority, bool higherNumberIsHigherPriority)
{
	if (higherNumberIsHigherPriority)
		return static_cast<int64_t>(rawPriority);

	/* A downward numbering ranks below zero so that a larger result means a
	   higher priority in both directions, and no caller has to carry the
	   direction around to compare two tasks. */
	return -static_cast<int64_t>(rawPriority);
}

const RtosTask* RtosTaskEnumeration::find(uint32_t tcbAddress) const
{
	for (const RtosTask& task : tasks)
	{
		if (task.tcbAddress == tcbAddress)
			return &task;
	}

	return nullptr;
}

RtosTaskEnumeration RtosTaskEnumerator::enumerate(IRtosMemory& memory,
                                                  const RtosProfileBase::Resolution& layout,
                                                  const std::vector<ListSource>& sources,
                                                  uint32_t currentTcbAddress,
                                                  bool higherNumberIsHigherPriority,
                                                  const RtosWalkLimits& limits)
{
	RtosTaskEnumeration result;

	for (const ListSource& source : sources)
	{
		RtosListWalker::Walk walk = RtosListWalker::walk(memory, layout, source.address, limits);

		/*
		 * A list caught while it was being changed is read again.
		 *
		 * The kernel rewrites some of these lists every tick - the delayed
		 * list gains and loses an entry whenever a task starts or stops
		 * waiting - so a single attempt lands in the middle of an edit often
		 * enough to matter. Measured on the fixture target, one attempt left
		 * the delayed list unreadable in eight rounds out of ten, and the
		 * table then showed nine of its fourteen tasks; a second attempt is
		 * enough to make that rare, and the cost is one more pass over a list
		 * the round was going to walk anyway.
		 *
		 * Only these two outcomes are retried. A chain that folded back on
		 * itself, or whose backward pointers disagreed, moved while it was
		 * being read; a failed read or a missing offset will not fix itself,
		 * and reading it again would only spend the round's budget twice.
		 */
		if (walk.result == RtosListWalker::Result::Loop || walk.result == RtosListWalker::Result::LinksDisagree)
			walk = RtosListWalker::walk(memory, layout, source.address, limits);

		result.listsWalked++;

		if (!walk.ok())
		{
			result.listsFailed++;

			const std::string sentence = source.key + ": " + RtosListWalker::describe(walk.result);

			/*
			 * A chain that led back into itself, or whose backward pointers
			 * disagreed, was read while the target was changing it. What came
			 * back is still real - the items were read off the target - it
			 * just does not all belong to one moment.
			 *
			 * A read that failed, or a field with no offset, is a different
			 * matter: there is nothing to hand back, and the offset one will
			 * not fix itself on the next round.
			 */
			if (walk.result == RtosListWalker::Result::Loop || walk.result == RtosListWalker::Result::LinksDisagree)
			{
				result.inconsistencies.push_back(sentence);

				/*
				 * The items already read are kept, and the walk that failed is
				 * not the last word on the list.
				 *
				 * Every item in `walk.items` was checked against its
				 * predecessor before it was kept, and the one that failed the
				 * check is not among them: what is there is what the target
				 * really said, up to the point where it moved. Throwing that
				 * away costs the round every task that was on that list - and
				 * the delayed list is rewritten every tick, so on a running
				 * target it is the list that most often moves while it is
				 * being read. Measured on the fixture target, the round came
				 * back with nine of its fourteen tasks, and the CPU shares
				 * were then scaled up by whatever the missing ones had used.
				 */
			}
			else
			{
				result.complete = false;
				result.problems.push_back(sentence);

				/* Nothing to hand back: a read failed, or a field has no
				   offset, and neither fixes itself by being read again. */
				continue;
			}
		}

		if (walk.countDisagrees)
		{
			/* The chain ended properly but the list says it holds a different
			   number of items than it yielded, which is what a change landing
			   mid-walk looks like. The items are used and the round is
			   published; the difference is recorded as a moment that moved. */
			result.inconsistencies.push_back(source.key + ": the list declares " + std::to_string(walk.declaredCount) + " items and " + std::to_string(walk.items.size()) + " were found");
		}

		for (const RtosListWalker::Item& item : walk.items)
		{
			RtosTask task;
			task.tcbAddress = item.owner;
			task.stateListItem = item.address;
			task.waitingList = item.container;
			task.state = source.state;
			task.sourceList = source.key;

			if (source.carriesPriority)
			{
				task.rawPriority = item.value;
				task.hasPriority = true;
				task.priorityRank = normalizedPriority(task.rawPriority, higherNumberIsHigherPriority);
			}

			if (source.carriesWaitTime)
			{
				task.waitValue = item.value;
				task.hasWaitTime = true;
			}

			/* Ready and running are the same list. Which of the two a task is
			   comes from the kernel's current task pointer and from nothing
			   else, which is why this is checked after the state was taken
			   from the list. */
			if (source.state == RtosTaskState::Ready && item.owner == currentTcbAddress)
				task.state = RtosTaskState::Running;

			const RtosTask* existing = result.find(task.tcbAddress);

			if (existing != nullptr)
			{
				/* One control block on two lists is a change caught mid
				   flight, not a task to report twice. The other list saw it
				   before it moved; this one saw it after. */
				result.duplicates++;
				result.inconsistencies.push_back("the task at 0x" + hexOf(task.tcbAddress) + " was found on both " + existing->sourceList + " and " + source.key);

				continue;
			}

			result.tasks.push_back(std::move(task));
		}
	}

	return result;
}

void RtosTaskEnumerator::absorb(RtosTaskEnumeration& into, const RtosTaskEnumeration& from)
{
	for (const RtosTask& task : from.tasks)
	{
		/* Already known means the first pass found it on a kernel list, where
		   the item value carries a priority or a wake tick. That reading is
		   the better one, so it stands. */
		if (into.find(task.tcbAddress) != nullptr)
			continue;

		into.tasks.push_back(task);
	}

	/* Lists, not tasks: both passes walked lists and both count them. */
	into.listsWalked += from.listsWalked;
	into.listsFailed += from.listsFailed;
	into.duplicates += from.duplicates;

	/* The second pass can fail in ways the first did not, and a round is only
	   as complete as its least complete part. */
	into.problems.insert(into.problems.end(), from.problems.begin(), from.problems.end());
	into.inconsistencies.insert(into.inconsistencies.end(), from.inconsistencies.begin(), from.inconsistencies.end());

	if (!from.complete)
		into.complete = false;
}
