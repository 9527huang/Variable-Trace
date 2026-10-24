#include "RtosTimeBudget.hpp"

#include <algorithm>

namespace
{
	/* Union of the spans that fall inside the window, in seconds.
	 *
	 * Merged rather than summed: a task whose state was recorded over
	 * overlapping samples would otherwise be counted twice, and an interrupt
	 * nested inside another is time the CPU spent once. */
	double unionSeconds(std::vector<std::pair<double, double>> spans, double from, double to)
	{
		if (!(to > from))
			return 0.0;

		for (std::pair<double, double>& span : spans)
		{
			span.first = std::max(span.first, from);
			span.second = std::min(span.second, to);
		}

		spans.erase(std::remove_if(spans.begin(), spans.end(), [](const std::pair<double, double>& span)
		                           {
			return !(span.second > span.first);
		}),
		            spans.end());

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
}  // namespace

double RtosTimeBudget::taskPercent() const
{
	return windowSeconds > 0.0 ? 100.0 * taskSeconds / windowSeconds : 0.0;
}

double RtosTimeBudget::isrPercent() const
{
	return windowSeconds > 0.0 ? 100.0 * isrSeconds / windowSeconds : 0.0;
}

double RtosTimeBudget::unaccountedPercent() const
{
	return windowSeconds > 0.0 ? 100.0 * unaccountedSeconds / windowSeconds : 0.0;
}

RtosTimeBudget computeTimeBudget(const std::vector<RtosTimeline::StateInterval>& bands, const std::vector<RtosTrackEntry>& isr, double from,
                                 double to)
{
	RtosTimeBudget budget;

	if (!(to > from))
		return budget;

	budget.windowSeconds = to - from;

	/* Every task, whatever its state: a task that is ready or blocked is still
	   a task whose time is accounted for by the kernel. Only "the round did not
	   read it" leaves a hole, and that shows up as uncovered time. */
	std::vector<std::pair<double, double>> taskSpans;
	taskSpans.reserve(bands.size());

	for (const RtosTimeline::StateInterval& band : bands)
	{
		if (!(band.to > band.from))
			continue;

		taskSpans.emplace_back(band.from, band.to);
	}

	budget.hasTask = !taskSpans.empty();
	budget.taskSeconds = unionSeconds(std::move(taskSpans), from, to);

	std::vector<std::pair<double, double>> isrSpans;
	isrSpans.reserve(isr.size());

	for (const RtosTrackEntry& entry : isr)
	{
		if (!entry.hasSeconds || !(entry.toSeconds > entry.fromSeconds))
			continue;

		isrSpans.emplace_back(entry.fromSeconds, entry.toSeconds);
	}

	budget.hasIsr = !isrSpans.empty();
	budget.isrSeconds = unionSeconds(std::move(isrSpans), from, to);

	budget.unaccountedSeconds = budget.windowSeconds - budget.taskSeconds - budget.isrSeconds;

	return budget;
}
