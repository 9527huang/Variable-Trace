#ifndef _RTOSTIMEBUDGET_HPP
#define _RTOSTIMEBUDGET_HPP

#include <cstdint>
#include <vector>

#include "RtosTimeline.hpp"
#include "RtosTraceReducer.hpp"

/*
 * Where the window's time went.
 *
 * The task table says how the time the tasks used divides between them, and the
 * ISR band says how much of the window the interrupts held. Neither answers the
 * question that decides what to look at next: do those two account for the whole
 * window, or is some of it missing?
 *
 * Missing time is a finding, not a rounding error. It is rounds that did not
 * cover the target, events that were lost, or a core that was not running any of
 * this - and a panel that quietly normalises it away hides the one number that
 * says the observation itself is incomplete.
 *
 * The two are exclusive by construction: while an interrupt runs, no task runs.
 * So they can be added, and what is left over is the gap. When they overlap the
 * data is wrong somewhere, which is also worth saying rather than clamping in
 * silence.
 */

struct RtosTimeBudget
{
	double windowSeconds = 0.0;

	double taskSeconds = 0.0;
	double isrSeconds = 0.0;

	bool hasTask = false;
	bool hasIsr = false;

	/* Window time covered by neither. Negative when the two overlap, which
	   cannot happen in a real trace and means one of them is wrong. */
	double unaccountedSeconds = 0.0;

	double taskPercent() const;
	double isrPercent() const;
	double unaccountedPercent() const;

	/* The two accounts overlap by more than rounding. */
	bool overlaps() const
	{
		return unaccountedSeconds < -1e-6;
	}
};

/* `bands` is the timeline's state intervals, `isr` the projection's ISR track. */
RtosTimeBudget computeTimeBudget(const std::vector<RtosTimeline::StateInterval>& bands, const std::vector<RtosTrackEntry>& isr, double from,
                                 double to);

#endif
