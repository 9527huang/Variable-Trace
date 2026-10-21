#include "RtosEventQuery.hpp"

#include <algorithm>

RtosEventQuery::Selection RtosEventQuery::select(const std::vector<RtosEventRing::Interval>& runs, uint32_t task, size_t limit)
{
	Selection selection;

	/* Counted over everything, kept only the newest few: walking the vector
	   twice rather than once keeps the newest runs without a deque or a
	   running erase, and the vector is already in time order. */
	auto matches = [task](const RtosEventRing::Interval& run) { return task == 0 || run.tcbAddress == task; };

	for (const RtosEventRing::Interval& run : runs)
	{
		if (matches(run))
			selection.matched++;
	}

	if (selection.matched == 0)
		return selection;

	const size_t keep = limit == 0 ? selection.matched : std::min(limit, selection.matched);

	selection.rows.reserve(keep);

	/* From the back: the newest are the ones a limit keeps, and the tail is
	   where they are. Collected in reverse and turned round once, because the
	   list is read oldest first. */
	for (auto run = runs.rbegin(); run != runs.rend() && selection.rows.size() < keep; ++run)
	{
		if (!matches(*run))
			continue;

		Row row;
		row.tcbAddress = run->tcbAddress;
		row.fromSeconds = run->fromSeconds;
		row.toSeconds = run->toSeconds;

		selection.rows.push_back(row);
	}

	std::reverse(selection.rows.begin(), selection.rows.end());

	return selection;
}
