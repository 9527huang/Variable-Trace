#ifndef _RTOSTRACEWINDOW_HPP
#define _RTOSTRACEWINDOW_HPP

#include <cstdint>
#include <vector>

#include "RtosTraceReducer.hpp"

/*
 * Asking a track "what is in this time window", without walking everything.
 *
 * The strip is a few hundred pixels wide and the history can be a hundred
 * thousand entries; drawn one per entry, most of them land on the same column
 * and the rest cost more than the frame is worth. Each column is therefore one
 * bucket, and what a bucket keeps is what a reader needs from a column it cannot
 * resolve: how many, how long the longest was, and - for the two tracks that
 * must never be hidden by this - whether anything happened at all.
 *
 * The rule that cannot be traded away (plan §6.3): **faults and gaps are never
 * sampled out.** A bucket that contains a fault says so no matter how many
 * other entries share the column, and the count travels with it. An aggregation
 * that dropped them would draw the quietest possible picture of the worst
 * possible moment.
 */
class RtosTraceWindow
{
   public:
	struct Options
	{
		double fromSeconds = 0.0;
		double toSeconds = 0.0;

		/* How many columns the strip has. One bucket per column. */
		int pixels = 400;
	};

	struct Bucket
	{
		/* The column's own edges, so that the bands line up with the axis
		   rather than with whichever entry happened to fall in them. */
		double fromSeconds = 0.0;
		double toSeconds = 0.0;

		uint32_t count = 0;

		/* The longest interval in this column, in seconds. Zero when nothing
		   here carried a duration - which is not the same as a zero length
		   interval, and the caller has `hasDuration` for that. */
		double longestSeconds = 0.0;
		bool hasDuration = false;

		/* Set when a fault or a gap is in this column. They are never dropped
		   by the aggregation, so a column with one always shows something. */
		bool hasFault = false;
		bool hasGap = false;

		/* The worst fault class in the column, when there is one. */
		uint8_t worstFaultClass = 0;
	};

	struct Result
	{
		std::vector<Bucket> buckets;

		/* Entries whose span touched the window, before aggregation. */
		uint32_t matched = 0;

		/* Entries that exist but have no target time, so they cannot be placed
		   on an axis. Counted, never silently folded into "nothing here". */
		uint32_t untimed = 0;
	};

	/* `entries` must be in history order; the result keeps that order. */
	static Result query(const std::vector<RtosTrackEntry>& entries, const Options& options);

	/* Fault and gap only. Everything else may be aggregated into one column and
	   represented by a count. */
	static bool neverAggregated(RtosTrackEntry::Kind kind);
};

#endif
