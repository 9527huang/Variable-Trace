#include "RtosCpuLoad.hpp"

#include <algorithm>
#include <utility>

namespace
{
	struct Share
	{
		size_t index = 0;
		uint32_t used = 0;
	};
}  // namespace

void RtosCpuLoad::step(std::vector<RtosTask>& tasks, RtosCpuLoadSummary& summary, double nowSeconds)
{
	std::map<uint32_t, Sample> current;
	std::vector<Share> shares;

	uint64_t total = 0;
	double window = 0.0;

	for (size_t index = 0; index < tasks.size(); index++)
	{
		RtosTask& task = tasks[index];

		if (!task.hasRunTimeCounter)
			continue;

		Sample sample;
		sample.counter = task.runTimeCounter;
		sample.kernelTaskNumber = task.kernelTaskNumber;
		sample.hasKernelTaskNumber = task.hasKernelTaskNumber;
		sample.takenAtSeconds = nowSeconds;

		current[task.tcbAddress] = sample;

		const auto before = previous.find(task.tcbAddress);

		if (before == previous.end())
			continue;

		if (before->second.hasKernelTaskNumber && task.hasKernelTaskNumber && before->second.kernelTaskNumber != task.kernelTaskNumber)
			continue;

		/* Unsigned on purpose: the counter is a machine word a timer
		   increments, so it wraps, and the difference is right either way. */
		const uint32_t used = task.runTimeCounter - before->second.counter;

		Share share;
		share.index = index;
		share.used = used;

		shares.push_back(share);
		total += used;

		window = std::max(window, nowSeconds - before->second.takenAtSeconds);
	}

	previous = std::move(current);

	summary.tasksMeasured = static_cast<uint32_t>(shares.size());
	summary.windowSeconds = window;

	/* Nothing moved between the two rounds. Either the counter's timer is not
	   running, or two rounds were taken inside one tick of it. A column of
	   zeroes would claim there was time to divide and that it went nowhere. */
	summary.counting = total > 0;

	if (!summary.counting)
		return;

	for (const Share& share : shares)
	{
		tasks[share.index].hasCpuShare = true;
		tasks[share.index].cpuSharePercent = 100.0 * static_cast<double>(share.used) / static_cast<double>(total);
	}
}

void RtosCpuLoad::forget()
{
	previous.clear();
}
