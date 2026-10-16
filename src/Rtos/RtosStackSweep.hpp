#ifndef _RTOSSTACKSWEEP_HPP
#define _RTOSSTACKSWEEP_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "RtosSnapshot.hpp"
#include "RtosStackScanner.hpp"

/*
 * The stack water marks, a few tasks at a time.
 *
 * One scan costs about 4.8 us for every byte of the free part of a stack, and
 * the free part of a stack is hundreds of bytes: a single task is a couple of
 * milliseconds on the probe, and a whole set of them will not fit inside a
 * sampling period. A high water mark also changes slowly - a task that has
 * been running for a minute has already made its deepest call - so the sweep
 * goes round the tasks a few at a time and every reading carries the moment it
 * was taken at.
 *
 * Nothing here reads a clock. The caller passes the time each round in the
 * same units the snapshot's own times use, which keeps this testable with a
 * made up clock instead of a real one.
 *
 * The sweep remembers the last good reading per control block. A scan that
 * fails leaves the previous one in place: an older number with its age beside
 * it says more than a gap, and the age is what keeps it honest.
 */
class RtosStackSweep
{
   public:
	struct Settings
	{
		RtosStackScanSettings scan;

		/* How many stacks one round scans. The round interval is what turns
		   this into the time a whole sweep takes: two per round at the
		   default half second interval is a sweep every few seconds. */
		uint32_t tasksPerRound = 2;

		/* How long a reading is kept for a control block that no round has
		   listed since it was taken.
		
		   A task missing from one round is not a deleted task. A round that
		   could not walk one of the kernel's lists leaves out everything that
		   was on it, and throwing the readings away on that would leave the
		   column half empty for a reason that has nothing to do with the
		   stacks. The lifetime is what keeps the other case from being
		   answered with a stranger's number: a control block freed and handed
		   out again must not inherit the reading of whatever ran there
		   before. */
		double readingLifetimeSeconds = 60.0;
	};

	void step(IRtosMemory& memory, const std::vector<RtosTask>& tasks, const Settings& settings, double nowSeconds);

	/* Writes what the sweep knows into a snapshot: one reading per task that
	   has one, and the summary that says how much of the set it covers and
	   whether the numbers can be trusted at all. */
	void applyTo(RtosSnapshot& snapshot, double nowSeconds) const;

	/* Drops everything: used when the session or the kernel under the view
	   changes, so that readings taken against one target are not shown beside
	   another. */
	void forget();

	size_t readingCount() const
	{
		return known.size();
	}

   private:
	struct Reading
	{
		RtosStackScan scan;

		/* The range the scan was taken over, kept because the percentage is
		   asked of the reading rather than of the round: a round that read no
		   control blocks still knows how big the stack was when it was read. */
		uint32_t stackBytes = 0;

		/* The kernel's own task number, where it keeps one. It survives a task
		   being deleted and another taking its control block, which is the one
		   thing the address cannot tell apart. */
		uint32_t kernelTaskNumber = 0;
		bool hasKernelTaskNumber = false;

		double takenAtSeconds = 0.0;
	};

	std::map<uint32_t, Reading> known;

	/* Where the last round stopped, as an index into the list that round was
	   given. The order of the tasks changes between rounds as they move
	   between lists, so this is a starting point rather than a promise: over
	   a few rounds every task in the list gets its turn. */
	size_t cursor = 0;

	/* Set once the pattern has been seen in any stack. Until then a zero free
	   count could mean a stack that is full or a kernel that fills nothing,
	   and the two cannot be told apart, so nothing is reported. */
	bool fillSeen = false;

	bool started = false;
};

#endif
