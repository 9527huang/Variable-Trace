#ifndef _RTOSISRATTRIBUTION_HPP
#define _RTOSISRATTRIBUTION_HPP

#include <cstdint>
#include <vector>

#include "RtosTimeline.hpp"
#include "RtosTraceReducer.hpp"

/*
 * Which task an interrupt landed on.
 *
 * The strip shows one row per task and the ISR band shows when interrupts
 * happened, and the two are read side by side without either saying which task
 * was running when a given interrupt arrived. That is the question the pair
 * exists to answer: "this task keeps being interrupted" is a fact about a task,
 * and the strip alone cannot state it.
 *
 * The answer is one horizontal position and one row number, so it is matched
 * here rather than in the drawing code - a picture that puts a mark on the wrong
 * lane is a picture nobody can check.
 */

struct RtosIsrHit
{
	/* The task that was running when the interrupt began. Zero when no task was
	   running at that instant - an idle tick, or the window's edge. */
	uint32_t tcbAddress = 0;

	/* When the interrupt began, in the same seconds the strip uses. */
	double seconds = 0.0;

	uint8_t depth = 1;
};

struct RtosIsrAttribution
{
	std::vector<RtosIsrHit> hits;

	/* Interrupts that began while no task was recorded as running. Counted, not
	   dropped: a target whose rounds miss the running task would otherwise show
	   an empty attribution with no sign that anything had been looked at. */
	uint32_t unattributed = 0;
};

/*
 * `running` must be the intervals whose state is Running, in ascending order of
 * `from` - `RtosTimeline::stateIntervals` returns them per task, so the caller
 * filters and sorts once. `isr` is the projection's ISR track.
 */
RtosIsrAttribution attributeIsr(const std::vector<RtosTimeline::StateInterval>& running, const std::vector<RtosTrackEntry>& isr);

#endif
