#include "RtosTraceClock.hpp"

namespace
{
	constexpr uint64_t tickRange = 0x100000000ull;
	constexpr uint64_t halfRange = 0x80000000ull;
}

uint64_t RtosTraceClock::extend(uint32_t raw)
{
	if (!haveBase)
	{
		haveBase = true;
		lastRaw = raw;

		return raw;
	}

	/* A counter that went backwards by more than half its range wrapped; one
	   that went backwards by less is an out of order reading, and counting it
	   as a wrap would add sixty five seconds to the timeline for a bug that
	   took none. */
	if (raw < lastRaw && (static_cast<uint64_t>(lastRaw) - raw) > halfRange)
		wrapCount++;

	lastRaw = raw;

	return static_cast<uint64_t>(wrapCount) * tickRange + raw;
}

void RtosTraceClock::noteFrequency(uint64_t atTicks, uint32_t hz)
{
	if (!intervals.empty() && intervals.back().hz == hz)
		return;

	/* A clock sync that arrives earlier than the last one recorded is not a
	   new interval: it belongs to a part of the stream that was already
	   accounted for, and inserting it would make the list unsorted and every
	   lookup after it wrong. */
	if (!intervals.empty() && atTicks < intervals.back().startTicks)
		return;

	Interval interval;
	interval.startTicks = atTicks;
	interval.hz = hz;

	intervals.push_back(interval);
}

uint32_t RtosTraceClock::frequencyAt(uint64_t ticks) const
{
	uint32_t hz = 0;

	for (const Interval& interval : intervals)
	{
		if (interval.startTicks > ticks)
			break;

		hz = interval.hz;
	}

	return hz;
}

std::optional<double> RtosTraceClock::secondsBetween(uint64_t from, uint64_t to) const
{
	if (to < from || intervals.empty())
		return std::nullopt;

	size_t index = intervals.size();

	for (size_t candidate = 0; candidate < intervals.size(); candidate++)
	{
		if (intervals[candidate].startTicks > from)
			break;

		index = candidate;
	}

	if (index == intervals.size())
		return std::nullopt;

	const Interval& interval = intervals[index];

	if (interval.hz == 0)
		return std::nullopt;

	/* The next interval starts somewhere in this span: the duration is a sum of
	   two frequencies and this class does not add them up. Saying "not
	   computable" is the only honest answer. */
	if (index + 1 < intervals.size() && to >= intervals[index + 1].startTicks)
		return std::nullopt;

	return static_cast<double>(to - from) / static_cast<double>(interval.hz);
}

void RtosTraceClock::reset()
{
	lastRaw = 0;
	wrapCount = 0;
	haveBase = false;
	intervals.clear();
}
