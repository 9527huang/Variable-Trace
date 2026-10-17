#include "RtosEventHistory.hpp"

#include <algorithm>

void RtosEventHistory::append(const std::vector<RtosEventRing::Interval>& batch, double keepSeconds)
{
	if (batch.empty())
		return;

	std::lock_guard<std::mutex> guard(mtx);

	for (const RtosEventRing::Interval& interval : batch)
	{
		/* The run that was still going when the last batch ended arrives again
		   in this one. Joining them keeps a seam out of the middle of a run. */
		if (!intervals.empty())
		{
			RtosEventRing::Interval& last = intervals.back();

			if (last.tcbAddress == interval.tcbAddress && interval.fromSeconds <= last.toSeconds + 1e-9)
			{
				last.toSeconds = std::max(last.toSeconds, interval.toSeconds);

				continue;
			}
		}

		intervals.push_back(interval);
	}

	const double newest = intervals.back().toSeconds;

	/* Windows are what a run is worth; a target with nothing to say should not
	   fill memory with saying it. */
	while (!intervals.empty() && intervals.front().toSeconds < newest - keepSeconds)
		intervals.pop_front();
}

void RtosEventHistory::copyRange(double fromSeconds, double toSeconds, std::vector<RtosEventRing::Interval>& into) const
{
	into.clear();

	std::lock_guard<std::mutex> guard(mtx);

	for (const RtosEventRing::Interval& interval : intervals)
	{
		if (interval.toSeconds < fromSeconds)
			continue;

		if (interval.fromSeconds > toSeconds)
			break;

		into.push_back(interval);
	}
}

void RtosEventHistory::clear()
{
	std::lock_guard<std::mutex> guard(mtx);

	intervals.clear();
	lost = 0;
}

size_t RtosEventHistory::size() const
{
	std::lock_guard<std::mutex> guard(mtx);

	return intervals.size();
}

void RtosEventHistory::noteLostRecords(uint32_t count)
{
	if (count == 0)
		return;

	std::lock_guard<std::mutex> guard(mtx);

	lost += count;
}

double RtosEventHistory::oldestSeconds() const
{
	std::lock_guard<std::mutex> guard(mtx);

	return intervals.empty() ? 0.0 : intervals.front().fromSeconds;
}

double RtosEventHistory::newestSeconds() const
{
	std::lock_guard<std::mutex> guard(mtx);

	return intervals.empty() ? 0.0 : intervals.back().toSeconds;
}

uint32_t RtosEventHistory::lostRecords() const
{
	std::lock_guard<std::mutex> guard(mtx);

	return lost;
}
