#ifndef _RTOSEVENTHISTORY_HPP
#define _RTOSEVENTHISTORY_HPP

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "RtosEventRing.hpp"

/*
 * The runs the target reported, kept for the timeline to draw.
 *
 * A round reads a batch of switch records and turns them into runs; this keeps
 * the runs, because the timeline shows minutes while a batch is a fraction of a
 * second. What is kept is bounded by time rather than by count: a run is worth
 * as much as the window it falls in, and a quiet target would otherwise keep
 * hours of nothing.
 *
 * Two batches meet at the one run that was still going when the first ended, so
 * that run arrives again in the second and would be drawn as two - a seam of a
 * frame's width in the middle of it. Appending joins them instead.
 *
 * Read from the interface thread and written from the sampling thread, like the
 * sampled timeline and for the same reason.
 */
class RtosEventHistory
{
   public:
	/* Adds a batch, oldest first, and forgets anything older than
	   `newest - keepSeconds`. */
	void append(const std::vector<RtosEventRing::Interval>& intervals, double keepSeconds);

	/* Copies the runs that overlap the range, oldest first. A run that starts
	   before the range is copied with its own start: the caller clips. */
	void copyRange(double fromSeconds, double toSeconds, std::vector<RtosEventRing::Interval>& into) const;

	void clear();

	size_t size() const;

	/* How many records never made it across - overwritten in the ring or
	   passed over by a round's budget. A picture with a hole in it has to say
	   so, and this is the number it says it with. */
	void noteLostRecords(uint32_t count);

	uint32_t lostRecords() const;

   private:
	mutable std::mutex mtx;
	std::deque<RtosEventRing::Interval> intervals;
	uint32_t lost = 0;
};

#endif
