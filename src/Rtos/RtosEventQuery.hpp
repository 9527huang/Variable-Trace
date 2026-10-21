#ifndef _RTOSEVENTQUERY_HPP
#define _RTOSEVENTQUERY_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RtosEventRing.hpp"

/*
 * Which switch records the list should hold.
 *
 * The ring yields runs; the list asks narrower questions of them - one task's
 * runs, the newest few hundred, how many there were before the cut. That is
 * arithmetic over a vector and it lives here rather than in the panel, because
 * a rule that decides which rows a reader sees is worth a test and the panel
 * cannot be built into one.
 *
 * The limit is a cut and not a filter: the reader is told both numbers ("600 of
 * 8123"), so a run that fell off the end is not silently gone. Dropping old
 * rows without saying so is the same lie as a list that stops scrolling.
 */
class RtosEventQuery
{
   public:
	/*
	 * One row of the list.
	 *
	 * Its own type rather than the ring's Interval, because this is what the
	 * list shows and the ring is what the target wrote: a field the row grows
	 * later - the state the run started in, a selection mark - belongs to the
	 * view and not to the record.
	 */
	struct Row
	{
		uint32_t tcbAddress = 0;
		double fromSeconds = 0.0;
		double toSeconds = 0.0;

		double ranForSeconds() const
		{
			return toSeconds - fromSeconds;
		}
	};

	struct Selection
	{
		/* Runs that matched, before the limit was applied. The list says "600
		   of 8123" with these two numbers, and the count is the half that a
		   reader needs in order to know there is more. */
		size_t matched = 0;

		/* The last `limit` of them, oldest first: the list reads the way the
		   strip does, left to right, earlier to later. */
		std::vector<Row> rows;
	};

	/*
	 * The runs to show.
	 *
	 * `task` zero means every task - no run has that control block, the same
	 * convention the selection uses. `limit` zero means no limit, for the
	 * caller that wants the count and nothing else.
	 */
	static Selection select(const std::vector<RtosEventRing::Interval>& runs, uint32_t task, size_t limit);
};

#endif
