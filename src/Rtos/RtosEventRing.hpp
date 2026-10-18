#ifndef _RTOSEVENTRING_HPP
#define _RTOSEVENTRING_HPP

#include <cstdint>
#include <vector>

/*
 * The switch records the target keeps, turned into runs.
 *
 * The target writes eight bytes a switch - which task, and when - into a ring
 * that overwrites its oldest record when it fills. This side answers two
 * questions about that ring: which records a reader has not seen yet, and what
 * the records say each task was doing.
 *
 * The second is the point of path B. A sampled timeline can only say "task X
 * was running at 1.20 s"; a sequence of switches says "task X ran from 1.20 s
 * to 1.27 s", which is what a reader asking about a scheduling problem is
 * actually looking for.
 *
 * Nothing here reads the target. Both halves are arithmetic over numbers the
 * caller collected, which is why they can be held to tests.
 */
class RtosEventRing
{
   public:
	/* A record as the target writes it: the control block of the task switched
	   in, and the target's microseconds since it started. */
	struct Record
	{
		uint32_t tcbAddress = 0;
		uint32_t timeUs = 0;
	};

	/* A task's run, from the switch that started it to the switch that ended
	   it. The last one in a batch is still open, and ends where the caller says
	   the batch ends. */
	struct Interval
	{
		uint32_t tcbAddress = 0;
		double fromSeconds = 0.0;
		double toSeconds = 0.0;
	};

	/*
	 * What to read next.
	 *
	 * Head counts records written and never goes back, so a reader that keeps
	 * its own copy can tell both what it missed and what is still there. The
	 * ring holds only `capacity` of them, and the caller only has the budget to
	 * read a few each round - so what is returned is the newest `budget` of what
	 * is still held, and `lost` counts everything skipped, whether the ring
	 * overwrote it or the budget passed over it. A reader that is told it lost
	 * records can say so; a reader that is not told will draw a gap as a run.
	 */
	struct Window
	{
		uint32_t first = 0;
		uint32_t count = 0;
		uint32_t lost = 0;

		/*
		 * Set when the reader's place was ahead of the writer's counter, which
		 * cannot happen unless the target started again: its ring begins at
		 * zero, and a reader that kept its place would wait for a head it has
		 * already passed - for ever, because what it reads is everything after
		 * its place and there is nothing after it. When this is set the reader
		 * has been put back to the beginning and the caller should treat the
		 * target as new: its clocks start again too.
		 */
		bool restarted = false;
	};

	static Window windowFor(uint32_t head, uint32_t read, uint32_t capacity, uint32_t budget);

	/*
	 * The runs the records describe, oldest first.
	 *
	 * `anchorUs` and `anchorSeconds` are the same moment said twice, once in the
	 * target's microseconds and once on the host's clock: the two views share
	 * one time axis, and this is how a record lands on it.
	 *
	 * The target's counter wraps every seventy-one minutes or so, which the
	 * unsigned difference between two records handles by itself - the same
	 * trick the run time counters rely on. It assumes no two records in one
	 * batch are more than seventy-one minutes apart, which is true of a batch
	 * read every round and would not be true of a reader that came back
	 * tomorrow.
	 */
	static std::vector<Interval> intervalsFrom(const std::vector<Record>& records, uint32_t anchorUs, double anchorSeconds, double untilSeconds);
};

#endif
