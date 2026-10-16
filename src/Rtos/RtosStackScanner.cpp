#include "RtosStackScanner.hpp"

#include <algorithm>

RtosStackScan RtosStackScanner::scan(IRtosMemory& memory, uint32_t stackBase, uint32_t stackBytes, const RtosStackScanSettings& settings)
{
	RtosStackScan result;

	if (stackBase == 0 || stackBytes == 0)
	{
		result.result = RtosStackScan::Result::NoStack;

		return result;
	}

	const uint32_t chunk = std::clamp(settings.chunkBytes, 1u, maxChunkBytes);
	const uint32_t limit = std::min(stackBytes, std::max(settings.maxBytes, 1u));

	uint32_t offset = 0;

	while (offset < limit)
	{
		const uint32_t wanted = std::min(chunk, limit - offset);
		uint8_t buffer[maxChunkBytes] = {};

		if (!memory.read(stackBase + offset, buffer, wanted))
		{
			/* What was read before this point was all pattern, so the free
			   count is known to be at least that. Reported as a failure with
			   the part that was read, rather than as a smaller answer that
			   looks like an exact one. */
			result.result = RtosStackScan::Result::ReadingFailed;
			result.scannedBytes = offset;
			result.freeBytes = offset;
			result.fillSeen = offset > 0;

			return result;
		}

		result.scannedBytes = offset + wanted;

		for (uint32_t index = 0; index < wanted; index++)
		{
			if (buffer[index] == settings.fillByte)
				continue;

			/* The first byte the task overwrote. Everything below it still
			   held the pattern, which is the count the kernel's own high
			   water mark would give: the entries before this one in the same
			   chunk, or the whole of the chunks before it. */
			result.freeBytes = offset + index;
			result.fillSeen = (offset > 0) || (index > 0);

			return result;
		}

		offset += wanted;
	}

	/* The whole of what was allowed still held the pattern: a stack that has
	   not been used at all, or one whose free part runs past the cap. */
	result.freeBytes = limit;
	result.reachedCap = limit < stackBytes;
	result.fillSeen = true;

	return result;
}
