#include "RtosCpuLoad.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace
{
	struct Share
	{
		size_t index = 0;

		/* Counts per second, not counts: see the note below about why the
		   spans differ. */
		double rate = 0.0;
	};
}  // namespace

void RtosCpuLoad::step(std::vector<RtosTask>& tasks, RtosCpuLoadSummary& summary, const Settings& settings, double nowSeconds)
{
	RoundCounters current;
	current.takenAtSeconds = nowSeconds;

	for (const RtosTask& task : tasks)
	{
		if (!task.hasRunTimeCounter)
			continue;

		Sample sample;
		sample.counter = task.runTimeCounter;
		sample.kernelTaskNumber = task.kernelTaskNumber;
		sample.hasKernelTaskNumber = task.hasKernelTaskNumber;

		current.counters[task.tcbAddress] = sample;
	}

	rounds.push_back(std::move(current));

	/* One more round than the window needs, so that a full window always has
	   something older than the round being measured to compare against. */
	const size_t keep = static_cast<size_t>(std::max(settings.windowRounds, 1u)) + 1;

	while (rounds.size() > keep)
		rounds.pop_front();

	/*
	 * The flags are cleared before they are set.
	 *
	 * A task with no share this round must not keep showing the one it had
	 * last round: a window that reached back past it, or a counter that
	 * stopped, would leave a number on screen that belongs to an earlier
	 * window and says nothing about this one.
	 */
	for (RtosTask& task : tasks)
		task.hasCpuShare = false;

	summary.tasksMeasured = 0;
	summary.windowSeconds = 0.0;
	summary.counting = false;

	if (rounds.size() < 2)
	{
		/* One round is a set of absolute counters, and an absolute counter
		   says nothing. */
		return;
	}

	const RoundCounters& latest = rounds.back();

	/* Every address the window has seen, which is the closest thing to a count
	   of the tasks the kernel is running: this round alone would only say how
	   many it managed to read. */
	std::set<uint32_t> known;

	for (const RoundCounters& round : rounds)
	{
		for (const auto& entry : round.counters)
			known.insert(entry.first);
	}

	summary.tasksKnown = static_cast<uint32_t>(known.size());

	std::vector<Share> shares;
	double total = 0.0;

	for (size_t index = 0; index < tasks.size(); index++)
	{
		const RtosTask& task = tasks[index];

		if (!task.hasRunTimeCounter)
			continue;

		/*
		 * The oldest round inside the window that holds this task.
		 *
		 * Not the oldest round, full stop. A round can miss a task: one walk of
		 * the kernel's lists fails, or a task moves between two of them at the
		 * wrong moment, and everything that was on that list is left out. Were
		 * every task measured against the same round, the ones that round
		 * missed would drop out of the whole - and everything else would be
		 * scaled up by exactly what they used. That is how a measurement
		 * quietly becomes a wrong answer, and it is not a small error: the
		 * busy tasks are the ones most likely to be missing, so the rest would
		 * be scaled up by the busiest part of the target.
		 */
		bool found = false;
		uint32_t referenceCounter = 0;
		double referenceAt = 0.0;

		for (size_t older = 0; older + 1 < rounds.size() && !found; older++)
		{
			const RoundCounters& round = rounds[older];
			const auto entry = round.counters.find(task.tcbAddress);

			if (entry == round.counters.end())
				continue;

			/* The address does not say the counter belongs to this task: a
			   control block freed and handed out again keeps the address and
			   changes the kernel's task number. */
			if (entry->second.hasKernelTaskNumber && task.hasKernelTaskNumber && entry->second.kernelTaskNumber != task.kernelTaskNumber)
				continue;

			found = true;
			referenceCounter = entry->second.counter;
			referenceAt = round.takenAtSeconds;
		}

		if (!found)
			continue;

		const double span = latest.takenAtSeconds - referenceAt;

		if (span <= 0.0)
			continue;

		/* Unsigned on purpose: the counter is a machine word a timer
		   increments, so it wraps, and the difference is right either way. */
		const uint32_t used = task.runTimeCounter - referenceCounter;

		/*
		 * A rate, because the spans differ.
		 *
		 * Two tasks measured over different lengths of time cannot be added up
		 * as they stand: a task the window only just caught would be counted
		 * against a fraction of what a task present throughout used. Dividing
		 * each by its own span puts them on one scale, and a share of the sum
		 * is a share of what the tasks used per second.
		 */
		Share share;
		share.index = index;
		share.rate = static_cast<double>(used) / span;

		shares.push_back(share);
		total += share.rate;

		/* The shortest span is what the window can honestly be called: a task
		   that had to be measured over less of it did not get the same
		   smoothing, and the summary should not claim more than the least
		   smoothed share has. */
		if (summary.windowSeconds == 0.0 || span < summary.windowSeconds)
			summary.windowSeconds = span;
	}

	summary.tasksMeasured = static_cast<uint32_t>(shares.size());

	/*
	 * The tasks this round could not read go into the whole at what they were
	 * last seen to use.
	 *
	 * They cannot be measured - there is no counter for a task the round did
	 * not find - but a share of a set that is missing them is too large, and
	 * missing them is not rare: on the fixture target the delayed list is
	 * caught mid-change often enough that a round comes back with eight of its
	 * fourteen tasks, and the busiest task then read 92 per cent against a
	 * ceiling of about 40. Counting them at their last rate keeps the whole
	 * close to what was really used; the two counts beside it say how much of
	 * it was estimated rather than measured.
	 */
	std::set<uint32_t> measured;

	for (const Share& share : shares)
		measured.insert(tasks[share.index].tcbAddress);

	summary.tasksEstimated = 0;

	for (const uint32_t address : known)
	{
		if (measured.count(address) != 0)
			continue;

		const auto rate = lastRate.find(address);

		if (rate == lastRate.end())
			continue;

		total += rate->second;
		summary.tasksEstimated++;
	}

	/* Remembered for the rounds that come back without some of these tasks,
	   and pruned so that a control block handed out again does not carry the
	   rate of whatever ran there before. */
	lastRate.clear();

	for (const Share& share : shares)
		lastRate[tasks[share.index].tcbAddress] = share.rate;

	/* Nothing moved across the window. Either the counter's timer is not
	   running, or the whole window fell inside one tick of it. A column of
	   zeroes would claim there was time to divide and that it went nowhere. */
	summary.counting = total > 0.0;

	if (!summary.counting)
		return;

	for (const Share& share : shares)
	{
		tasks[share.index].hasCpuShare = true;
		tasks[share.index].cpuSharePercent = 100.0 * share.rate / total;
	}
}

void RtosCpuLoad::forget()
{
	rounds.clear();
	lastRate.clear();
}
