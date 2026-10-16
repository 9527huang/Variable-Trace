#ifndef _RTOSSTACKSCANNER_HPP
#define _RTOSSTACKSCANNER_HPP

#include <cstdint>

#include "RtosMemory.hpp"

/*
 * How much of a task stack was ever used, worked out on this side.
 *
 * A kernel that fills a new stack with a pattern leaves a record of its own
 * deepest call in the stack itself: everything the task has touched no longer
 * holds the pattern, and the boundary between the two is the high water mark.
 * Reading it costs nothing on the target - no option to turn on beyond the one
 * that makes the kernel fill the stack in the first place, and no code to run
 * there - which is what makes it a column this view can have without asking
 * anything of the firmware it is looking at.
 *
 * The scan starts at the bottom of the stack because that is where the unused
 * part sits: a stack grows down, so the words that were never touched are the
 * ones at the lowest addresses. It stops at the first byte that is not the
 * pattern, and that offset is the answer.
 *
 * A stack grows down on most ports but not on all. What this reads on a port
 * that grows up is the wrong end, and the answer it gives is zero free bytes
 * rather than a wrong number - see `fillSeen` for how the caller tells that
 * apart from a stack that really is full.
 */
struct RtosStackScan
{
	enum class Result
	{
		Ok,
		/* No stack range to scan: the control block was not read this round,
		   or the kernel does not record where the stack ends. */
		NoStack,
		/* A read failed part way through. What was scanned so far is still
		   the pattern, so the answer would be an understatement of the use. */
		ReadingFailed,
	};

	Result result = Result::Ok;

	/* How much of the stack was looked at, and how much of it still held the
	   pattern. `freeBytes` is the kernel's own high water mark, in bytes. */
	uint32_t scannedBytes = 0;
	uint32_t freeBytes = 0;

	/* The pattern was seen at least once while scanning this stack.
	 *
	 * False with a zero `freeBytes` has two readings that look the same: a
	 * stack that is full to the brim, and a kernel that does not fill its
	 * stacks at all. The difference matters, because the second means the
	 * number is meaningless for every task, so it is carried rather than
	 * resolved here. */
	bool fillSeen = false;

	/* The scan stopped at the cap with the pattern still unbroken, so the
	   answer is "at least this much is free" rather than an exact count. */
	bool reachedCap = false;

	uint32_t usedBytes(uint32_t stackBytes) const
	{
		return stackBytes > freeBytes ? stackBytes - freeBytes : 0;
	}
};

/*
 * What a scan costs.
 *
 * A read costs a little over 300 us to start and about 4.8 us a byte after
 * that on the link this was measured on, so the chunk size is a real trade:
 * a bigger chunk reads past the boundary and pays for bytes nobody needed, a
 * smaller one pays for round trips instead. At those two numbers a round trip
 * is worth about 64 bytes, so a chunk of a little over that is where the two
 * costs cross.
 */
struct RtosStackScanSettings
{
	uint8_t fillByte = 0xA5;
	uint32_t chunkBytes = 128;

	/* The most of one stack to read in a single scan. A stack that is full to
	   the brim would otherwise be read to its end, and the cost of that is
	   paid in the sampling loop. A scan that stops here says so through
	   `reachedCap` instead of reporting the cap as the answer. */
	uint32_t maxBytes = 1024;
};

class RtosStackScanner
{
   public:
	/* The largest chunk this will ask for, whatever the settings say. The
	   buffer lives on the stack of the sampling thread, and a probe handed a
	   request bigger than it can serve fails rather than splitting it. */
	static constexpr uint32_t maxChunkBytes = 256;

	static RtosStackScan scan(IRtosMemory& memory, uint32_t stackBase, uint32_t stackBytes, const RtosStackScanSettings& settings);
};

#endif
