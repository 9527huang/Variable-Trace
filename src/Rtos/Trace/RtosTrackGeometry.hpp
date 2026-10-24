#ifndef _RTOSTRACKGEOMETRY_HPP
#define _RTOSTRACKGEOMETRY_HPP

#include <cstdint>
#include <vector>

#include "RtosTraceReducer.hpp"

/*
 * Where a track's intervals land, in pixels.
 *
 * The other tracks are counted: one mark per pixel column, the column's
 * brightness saying how many events fell in it. That is right for faults and
 * losses, which have no length, and wrong for interrupts, which do - an
 * interrupt is a span, and drawing it as a dot throws away the two things
 * anyone asks about it: how long it held the CPU, and whether another one
 * nested inside it.
 *
 * Placing the spans is arithmetic, so it lives here rather than in the panel.
 * In a drawing routine a wrong minimum width or an unclipped interval looks
 * like a rendering quirk; here it is a test that fails.
 */

struct RtosTrackMark
{
	/* Pixels from the left edge of the band. `right` is exclusive. */
	float left = 0.0f;
	float right = 0.0f;

	/* 1 for an interrupt that was not interrupted, 2 and up for the ones that
	   nested inside it. The panel uses it to tell "one interrupt" from "one
	   interrupt inside another", which a single colour cannot. */
	uint8_t depth = 1;
};

struct RtosTrackLayout
{
	std::vector<RtosTrackMark> marks;

	/* Entries the window does not cover. Counted rather than dropped silently:
	   a band that draws four of five intervals should say why. */
	uint32_t outside = 0;

	/* Entries with no time on both ends. They cannot be placed, and inventing
	   a position would put a mark where nothing happened. */
	uint32_t untimed = 0;
};

/* `width` is the band's width in pixels, `minPixels` the narrowest a mark may
   be drawn - an interrupt at eight milliseconds per pixel is a fraction of one,
   and a fraction rounds to nothing. */
RtosTrackLayout layoutIntervals(const std::vector<RtosTrackEntry>& entries, double from, double to, float width, float minPixels);

/*
 * How much of the window these intervals cover, in seconds.
 *
 * Not the sum of the durations: an interrupt that nested inside another has its
 * time counted once by the CPU and would be counted twice by a sum. The spans
 * are merged first, which is the whole of the arithmetic here - and it is here
 * rather than in the panel because a sum that looks right and is double is the
 * kind of mistake a picture cannot show.
 */
double coveredSeconds(const std::vector<RtosTrackEntry>& entries, double from, double to);

#endif
