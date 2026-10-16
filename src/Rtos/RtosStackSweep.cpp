#include "RtosStackSweep.hpp"

#include <algorithm>
#include <iterator>

namespace
{
	bool holdsAddress(const std::vector<RtosTask>& tasks, uint32_t tcbAddress)
	{
		for (const RtosTask& task : tasks)
		{
			if (task.tcbAddress == tcbAddress)
				return true;
		}

		return false;
	}
}  // namespace

void RtosStackSweep::step(IRtosMemory& memory, const std::vector<RtosTask>& tasks, const Settings& settings, double nowSeconds)
{
	started = true;

	/* A reading whose control block no round has listed for a while belongs to
	   a task that is gone. One round is not enough to say that: a round that
	   could not walk one of the kernel's lists leaves out everything that was
	   on it, and the readings would go with them. */
	for (auto entry = known.begin(); entry != known.end();)
	{
		const bool listed = holdsAddress(tasks, entry->first);
		const bool stale = (nowSeconds - entry->second.takenAtSeconds) > settings.readingLifetimeSeconds;

		entry = (listed || !stale) ? std::next(entry) : known.erase(entry);
	}

	if (tasks.empty())
	{
		cursor = 0;

		return;
	}

	const uint32_t perRound = std::max(settings.tasksPerRound, 1u);

	size_t index = cursor % tasks.size();
	uint32_t scanned = 0;

	for (size_t visited = 0; visited < tasks.size() && scanned < perRound; visited++)
	{
		index = (cursor + visited) % tasks.size();

		const RtosTask& task = tasks[index];

		/* A task whose stack range was not read this round - a cheap round
		   reads no control blocks - is passed over rather than counted, so
		   the sweep spends its turns on the tasks it can answer for. */
		if (task.tcbAddress == 0 || !task.hasStack || task.stackTop <= task.stackBase)
			continue;

		const uint32_t stackBytes = task.stackTop - task.stackBase;
		const RtosStackScan scan = RtosStackScanner::scan(memory, task.stackBase, stackBytes, settings.scan);

		if (scan.result == RtosStackScan::Result::Ok)
		{
			Reading reading;
			reading.scan = scan;
			reading.stackBytes = stackBytes;
			reading.takenAtSeconds = nowSeconds;
			reading.kernelTaskNumber = task.kernelTaskNumber;
			reading.hasKernelTaskNumber = task.hasKernelTaskNumber;

			known[task.tcbAddress] = reading;

			/* Once the pattern has been seen anywhere, a zero free count is
			   a stack that is really full - which is a finding, and the one
			   this column exists to produce. */
			if (scan.fillSeen)
				fillSeen = true;
		}

		scanned++;
	}

	cursor = (index + 1) % tasks.size();
}

void RtosStackSweep::applyTo(RtosSnapshot& snapshot, double nowSeconds) const
{
	RtosStackScanSummary summary;

	summary.enabled = started;
	summary.fillSeen = fillSeen;
	summary.tasksScanned = 0;
	summary.oldestReadingAgeSeconds = 0.0;

	for (RtosTask& task : snapshot.tasks)
	{
		const auto entry = known.find(task.tcbAddress);

		if (entry == known.end())
			continue;

		const Reading& reading = entry->second;

		/* The address alone does not say the reading belongs to this task. The
		   kernel's own task number does, where the target keeps one, and a
		   control block handed out again comes with a different number. */
		if (reading.hasKernelTaskNumber && task.hasKernelTaskNumber && reading.kernelTaskNumber != task.kernelTaskNumber)
			continue;

		const double age = std::max(nowSeconds - reading.takenAtSeconds, 0.0);

		summary.tasksScanned++;
		summary.oldestReadingAgeSeconds = std::max(summary.oldestReadingAgeSeconds, age);

		/* Until the fill pattern has been seen, a free count of zero cannot be
		   told from a kernel that fills nothing, and every number in the
		   column would be that same zero. Nothing is claimed until then, and
		   the summary carries the reason. */
		if (!fillSeen)
			continue;

		task.hasStackWatermark = true;
		task.stackFreeBytes = reading.scan.freeBytes;
		task.stackWatermarkAgeSeconds = age;
		task.stackWatermarkReachedCap = reading.scan.reachedCap;
		task.stackWatermarkStackBytes = reading.stackBytes;
	}

	snapshot.stackScan = summary;
}

void RtosStackSweep::forget()
{
	known.clear();
	cursor = 0;
	fillSeen = false;
	started = false;
}
