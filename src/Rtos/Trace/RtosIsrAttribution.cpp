#include "RtosIsrAttribution.hpp"

#include <algorithm>

RtosIsrAttribution attributeIsr(const std::vector<RtosTimeline::StateInterval>& running, const std::vector<RtosTrackEntry>& isr)
{
	RtosIsrAttribution result;

	for (const RtosTrackEntry& entry : isr)
	{
		/* An interval with no time cannot be placed on an axis at all, so it
		   cannot be attributed either. It is already counted as untimed by the
		   band; counting it again here would double the complaint. */
		if (!entry.hasSeconds)
			continue;

		const double at = entry.fromSeconds;

		/* The last interval that began at or before `at`. Binary search, not a
		   scan: a session holds thousands of interrupts and thousands of
		   intervals, and a linear search per interrupt is ten million comparisons
		   a frame.
		   
		   The end is exclusive - a task that stopped at 2.0 was not running at
		   2.0, the next one began there - which is also what stops a boundary
		   from being attributed twice. */
		const auto found = std::upper_bound(running.begin(), running.end(), at, [](double seconds, const RtosTimeline::StateInterval& interval)
		                                    {
			return seconds < interval.from;
		});

		if (found == running.begin())
		{
			result.unattributed++;

			continue;
		}

		const RtosTimeline::StateInterval& candidate = *(found - 1);

		if (!(at < candidate.to))
		{
			result.unattributed++;

			continue;
		}

		RtosIsrHit hit;

		hit.tcbAddress = candidate.tcbAddress;
		hit.seconds = at;
		hit.depth = entry.depth == 0 ? 1 : entry.depth;

		result.hits.push_back(hit);
	}

	return result;
}
