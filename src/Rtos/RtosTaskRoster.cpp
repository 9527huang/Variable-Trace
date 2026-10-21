#include "RtosTaskRoster.hpp"

void RtosTaskRoster::observe(const RtosSnapshot& snapshot)
{
	for (const RtosTask& task : snapshot.tasks)
	{
		/* An address of zero is a task the round could not place - it has no
		   control block to be known by, and every such task would land on the
		   same row. */
		if (task.tcbAddress == 0)
			continue;

		const auto found = index.find(task.tcbAddress);
		size_t slot = 0;

		if (found == index.end())
		{
			Entry entry;

			entry.tcbAddress = task.tcbAddress;
			entry.lastSeenAt = snapshot.captureStartedAt;

			slot = known.size();
			index.emplace(entry.tcbAddress, slot);
			known.push_back(std::move(entry));
		}
		else
			slot = found->second;

		Entry& entry = known[slot];

		/* The name is taken again every round rather than only when the entry
		   is new: a task can be renamed, and the roster is where the newest
		   spelling lives. */
		if (!task.name.empty())
			entry.name = task.name;

		if (!task.sourceList.empty())
			entry.sourceList = task.sourceList;

		entry.lastSeenAt = snapshot.captureStartedAt;
		entry.roundsSeen++;

		/* The rest is identity, so it is taken only when the round read it:
		   a value already known is not wiped by a round that could not. */
		if (task.hasStack)
		{
			entry.stackBase = task.stackBase;
			entry.stackTop = task.stackTop;
			entry.hasStack = true;
		}

		if (task.hasBasePriority)
		{
			entry.basePriority = task.basePriority;
			entry.hasBasePriority = true;
		}

		if (task.hasKernelTaskNumber)
		{
			entry.kernelTaskNumber = task.kernelTaskNumber;
			entry.hasKernelTaskNumber = true;
		}
	}
}

void RtosTaskRoster::clear()
{
	known.clear();
	index.clear();
}

const RtosTaskRoster::Entry* RtosTaskRoster::find(uint32_t tcbAddress) const
{
	const auto found = index.find(tcbAddress);

	if (found == index.end())
		return nullptr;

	return &known[found->second];
}
