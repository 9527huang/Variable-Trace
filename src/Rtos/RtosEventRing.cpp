#include "RtosEventRing.hpp"

#include <algorithm>

RtosEventRing::Window RtosEventRing::windowFor(uint32_t head, uint32_t read, uint32_t capacity, uint32_t budget)
{
	Window window;

	if (capacity == 0)
		return window;

	/* Nothing before this survived the ring. */
	const uint32_t oldestHeld = head > capacity ? head - capacity : 0u;

	/* What the reader has not seen that is still there. */
	const uint32_t from = std::max(read, oldestHeld);

	if (head <= from)
		return window;

	uint32_t count = head - from;

	/* Records that fell out of the ring before the reader got to them. */
	window.lost = from - read;

	if (count > budget)
	{
		/* More waiting than there is budget to read: take the newest, because
		   the reader is watching what is happening now, and count the rest as
		   lost so that the gap is reported rather than drawn. */
		window.lost += count - budget;
		count = budget;
	}

	window.first = head - count;
	window.count = count;

	return window;
}

std::vector<RtosEventRing::Interval> RtosEventRing::intervalsFrom(const std::vector<Record>& records, uint32_t anchorUs, double anchorSeconds,
                                                                  double untilSeconds)
{
	std::vector<Interval> intervals;

	if (records.empty())
		return intervals;

	uint64_t elapsedUs = 0;
	uint32_t previousUs = anchorUs;

	double previousAt = anchorSeconds;
	uint32_t previousTask = 0;
	bool havePrevious = false;

	for (const Record& record : records)
	{
		/* Unsigned, so a wrap comes out as the small step forward it is. */
		elapsedUs += static_cast<uint32_t>(record.timeUs - previousUs);
		previousUs = record.timeUs;

		const double at = anchorSeconds + static_cast<double>(elapsedUs) / 1000000.0;

		if (!havePrevious)
		{
			previousAt = at;
			previousTask = record.tcbAddress;
			havePrevious = true;

			continue;
		}

		/* A switch to the task already running is not a new run: the run it is
		   part of began where the first of them did. */
		if (record.tcbAddress == previousTask)
			continue;

		if (at > previousAt)
		{
			Interval interval;
			interval.tcbAddress = previousTask;
			interval.fromSeconds = previousAt;
			interval.toSeconds = at;

			intervals.push_back(interval);
		}

		previousAt = at;
		previousTask = record.tcbAddress;
	}

	/* The task switched in last is still running as far as this batch knows. A
	   batch that ends before it started - the caller's clock went backwards -
	   leaves it open rather than reporting a run of negative length. */
	if (havePrevious && untilSeconds > previousAt)
	{
		Interval interval;
		interval.tcbAddress = previousTask;
		interval.fromSeconds = previousAt;
		interval.toSeconds = untilSeconds;

		intervals.push_back(interval);
	}

	return intervals;
}
