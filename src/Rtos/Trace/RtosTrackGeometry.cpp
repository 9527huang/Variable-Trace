#include "RtosTrackGeometry.hpp"

#include <algorithm>
#include <cmath>

RtosTrackLayout layoutIntervals(const std::vector<RtosTrackEntry>& entries, double from, double to, float width, float minPixels)
{
	RtosTrackLayout layout;

	if (width <= 0.0f || !(to > from))
		return layout;

	if (minPixels <= 0.0f)
		minPixels = 1.0f;

	const double span = to - from;

	for (const RtosTrackEntry& entry : entries)
	{
		/* A span needs both ends. One end without a time is not half a span, it
		   is no span - and a mark drawn at the start of it would be a mark at a
		   time nobody measured. */
		if (!entry.hasSeconds || entry.fromSeconds > entry.toSeconds)
		{
			layout.untimed++;

			continue;
		}

		const double start = entry.fromSeconds;
		const double end = entry.toSeconds;

		/* Entirely outside: before the window, or after it. Both counted, so
		   that a band holding four of five intervals can say so. */
		if (end < from || start > to)
		{
			layout.outside++;

			continue;
		}

		/* Clipped to the window, not dropped: an interval that began before the
		   left edge is still holding the CPU at the left edge, and the picture
		   should show that. */
		const double visibleStart = std::max(start, from);
		const double visibleEnd = std::min(end, to);

		RtosTrackMark mark;

		mark.left = static_cast<float>((visibleStart - from) / span) * width;
		mark.right = static_cast<float>((visibleEnd - from) / span) * width;
		mark.depth = entry.depth == 0 ? 1 : entry.depth;

		/* The floor. An interrupt is microseconds and the window is seconds, so
		   without this most of them round to a width of zero and the band reads
		   as empty while the target was in fact taking an interrupt every
		   twenty milliseconds. */
		if (mark.right - mark.left < minPixels)
			mark.right = mark.left + minPixels;

		/* The floor can push the last mark past the edge; a mark that starts at
		   the very end has no room at all and would draw over the border. */
		if (mark.left > width)
		{
			layout.outside++;

			continue;
		}

		mark.right = std::min(mark.right, width);

		layout.marks.push_back(mark);
	}

	return layout;
}

double coveredSeconds(const std::vector<RtosTrackEntry>& entries, double from, double to)
{
	if (!(to > from))
		return 0.0;

	/* Clipped to the window and sorted by start. An interval that began before
	   the left edge is still covering the CPU at that edge, so it contributes
	   from there; one that ends after the right edge contributes up to it. */
	std::vector<std::pair<double, double>> spans;

	spans.reserve(entries.size());

	for (const RtosTrackEntry& entry : entries)
	{
		if (!entry.hasSeconds || entry.toSeconds < entry.fromSeconds)
			continue;

		const double start = std::max(entry.fromSeconds, from);
		const double end = std::min(entry.toSeconds, to);

		if (end > start)
			spans.emplace_back(start, end);
	}

	std::sort(spans.begin(), spans.end());

	double covered = 0.0;
	double reach = from;

	for (const std::pair<double, double>& span : spans)
	{
		const double start = std::max(span.first, reach);

		if (span.second > start)
		{
			covered += span.second - start;
			reach = span.second;
		}
	}

	return covered;
}
