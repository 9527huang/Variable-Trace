#ifndef _RTOSTRACECLOCK_HPP
#define _RTOSTRACECLOCK_HPP

#include <cstdint>
#include <optional>
#include <vector>

/*
 * Turning a free running 32 bit counter into time that can be trusted.
 *
 * Two things happen to a target counter that a naive difference gets wrong.
 * It wraps, so a subtraction across the wrap returns a huge positive number
 * instead of a small one. And its frequency changes - a clock switch, tickless
 * idle, a DWT that stopped - so a difference computed with one frequency is
 * wrong for the part of the interval that ran at another.
 *
 * Both are handled the same way: the ticks are extended to 64 bits by counting
 * wraps, and the frequency is recorded as intervals. A duration is computable
 * only when both ends fall in one interval. Across a change the answer is "not
 * computable", which the caller reports as such - a duration that is merely
 * wrong is worse than no duration, because it looks like a measurement.
 */
class RtosTraceClock
{
   public:
	/* One stretch of ticks that all ran at the same frequency. */
	struct Interval
	{
		uint64_t startTicks = 0;
		uint32_t hz = 0;
	};

	/* Extends one raw reading, counting wraps as they happen.
	 *
	 * The wrap test is the usual one: the counter went backwards, and by more
	 * than half the range. Anything smaller is a small step backwards, which
	 * for a monotonic counter means the reading is out of order - a different
	 * problem, reported as such rather than dressed up as a wrap. */
	uint64_t extend(uint32_t raw);

	/* Records that from `atTicks` onwards the counter runs at `hz`. Called on
	 * every `ClockSync`. A frequency equal to the current one is not a new
	 * interval: the point of the list is to know when a duration stops being
	 * computable, and nothing changed. */
	void noteFrequency(uint64_t atTicks, uint32_t hz);

	/* Frequency in force at a point, or 0 when nothing was ever recorded. */
	uint32_t frequencyAt(uint64_t ticks) const;

	/*
	 * Seconds between two extended readings.
	 *
	 * Empty when no frequency is known for the interval, when the two ends fall
	 * in different intervals, or when `to` comes before `from`. Every one of
	 * those is a question the data cannot answer, and the caller has rules that
	 * say what to do with an unanswerable question.
	 */
	std::optional<double> secondsBetween(uint64_t from, uint64_t to) const;

	/* How many wraps have been counted. Surfaced because a session that wrapped
	 * forty times is a long session, and a reader should be able to say so. */
	uint32_t wraps() const
	{
		return wrapCount;
	}

	/* Whether any reading has been extended yet. Before the first one there is
	 * no reference point, so the first reading defines the base. */
	bool hasBase() const
	{
		return haveBase;
	}

	void reset();

   private:
	uint32_t lastRaw = 0;
	uint32_t wrapCount = 0;
	bool haveBase = false;

	std::vector<Interval> intervals;
};

#endif
