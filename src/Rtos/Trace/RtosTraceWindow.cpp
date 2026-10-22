#include "RtosTraceWindow.hpp"

#include <algorithm>
#include <cmath>

bool RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind kind)
{
	/* A fault is one moment that must not be averaged away; a gap is the reason
	   every number after it is suspect. Both are single entries rather than
	   intervals, and both are the first thing a reader looks for. */
	return kind == RtosTrackEntry::Kind::Fault || kind == RtosTrackEntry::Kind::Gap;
}

RtosTraceWindow::Result RtosTraceWindow::query(const std::vector<RtosTrackEntry>& entries, const Options& options)
{
	Result result;

	const double span = options.toSeconds - options.fromSeconds;

	if (span <= 0.0 || options.pixels <= 0)
		return result;

	result.buckets.resize(static_cast<size_t>(options.pixels));

	const double column = span / static_cast<double>(options.pixels);

	for (int index = 0; index < options.pixels; index++)
	{
		result.buckets[static_cast<size_t>(index)].fromSeconds = options.fromSeconds + column * index;
		result.buckets[static_cast<size_t>(index)].toSeconds = options.fromSeconds + column * (index + 1);
	}

	for (const RtosTrackEntry& entry : entries)
	{
		if (!entry.hasSeconds)
		{
			/* Nothing to place on an axis. Counted so that the interface can say
			   "twelve entries have no time" instead of drawing a shorter track
			   and letting the reader wonder where they went. */
			result.untimed++;
			continue;
		}

		/* An interval that starts before the window and ends inside it is in the
		   window: it was running here. Both ends are compared, not just the
		   start. */
		if (entry.toSeconds < options.fromSeconds || entry.fromSeconds > options.toSeconds)
			continue;

		result.matched++;

		double at = std::max(entry.fromSeconds, options.fromSeconds);
		int bucketIndex = static_cast<int>((at - options.fromSeconds) / column);

		bucketIndex = std::clamp(bucketIndex, 0, options.pixels - 1);

		Bucket& bucket = result.buckets[static_cast<size_t>(bucketIndex)];

		bucket.count++;

		const double duration = entry.toSeconds - entry.fromSeconds;

		if (duration > 0.0)
		{
			if (!bucket.hasDuration || duration > bucket.longestSeconds)
			{
				bucket.longestSeconds = duration;
				bucket.hasDuration = true;
			}
		}

		if (entry.kind == RtosTrackEntry::Kind::Fault)
		{
			bucket.hasFault = true;

			/* The worst in the column, not the first: a bus fault behind a
			   usage fault is still a bus fault. */
			if (entry.faultClass > bucket.worstFaultClass)
				bucket.worstFaultClass = entry.faultClass;
		}

		if (entry.kind == RtosTrackEntry::Kind::Gap)
			bucket.hasGap = true;
	}

	return result;
}
