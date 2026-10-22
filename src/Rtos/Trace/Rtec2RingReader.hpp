#ifndef _RTEC2RINGREADER_HPP
#define _RTEC2RINGREADER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Rtec2Decoder.hpp"
#include "RtosTraceTypes.hpp"

/*
 * Reading the target's ring without ever believing half a record.
 *
 * The ring is written by the target while this reads it, so a slot can be
 * caught in the middle of being written. The format answers that with two
 * sentinels per slot (`seqCheck` before the payload, `tailCheck` after it) plus
 * a re-read of `seq` at the end; a slot that fails any of them is a slot that
 * may not be decoded, and - this is the part that matters - the whole batch is
 * abandoned rather than the good prefix being kept.
 *
 * Keeping the prefix would be the natural thing to write and the wrong thing to
 * do: the cursor would then sit after records that were dropped, and every
 * later interval would be computed across a hole nobody counted. The caller
 * retries the same cursor next round.
 *
 * Nothing here touches a probe. The bytes are handed in, which is what lets the
 * sentinel dance be tested with a hand-made slot that flips in the middle.
 */
struct Rtec2ControlBlock
{
	uint16_t wireVersion = 0;
	uint16_t slotBytes = 0;
	uint32_t capacity = 0;
	uint64_t epoch = 0;
	uint32_t targetHz = 0;
	uint32_t head = 0;
	uint32_t tail = 0;
	uint32_t generation = 0;
	uint32_t rejected = 0;
	uint32_t overwritten = 0;
	uint32_t committedSeq = 0;
	uint32_t capabilities = 0;
	uint32_t faultCount = 0;
};

class Rtec2RingReader
{
   public:
	static constexpr size_t controlBlockBytes = 64;
	/* 56, not 48: a slot holds the whole record - 24 byte header, 4 bytes of
	   ticks and up to 12 bytes of payload - plus two sentinels. 48 was the
	   number in the first draft of the document and it could not hold a fault
	   record; the document was corrected rather than the payload shrunk. */
	static constexpr size_t slotBytes = 56;

	/* The magic at the front of the control block: 'R','T','C','2'. */
	static constexpr uint32_t controlMagic = 0x32435452u;

	/* Parses the 64 byte description. A block that does not describe a usable
	   ring is refused with a reason rather than used as far as it makes
	   sense - a wrong capacity reads the wrong memory, and the wrong memory
	   looks like plausible records. */
	static bool parseControlBlock(const uint8_t* bytes, size_t size, Rtec2ControlBlock& block, RtosTraceProblem& problem);

	/* One round of reading. */
	struct Read
	{
		std::vector<Rtec2DecodedRecord> records;

		/* Slots that failed a sentinel check. Non zero means the batch was
		   abandoned: `records` is empty and `committed` is false. */
		uint32_t torn = 0;

		/* Slots skipped because the reader fell more than a ring behind. */
		uint32_t lost = 0;

		/* The writer's head is behind the reader's cursor, which can only
		   happen when the target started again. */
		bool restarted = false;

		/* Where the cursor may move to. Equal to the input when the batch could
		   not be committed. */
		uint32_t committed = 0;
		bool advanced = false;
	};

	/* `slots` is the ring as the probe read it: `capacity` slots of `slotBytes`
	   each. `from` is the reader's cursor; `budget` caps how many slots this
	   round may take. */
	static Read read(const uint8_t* slots, size_t bytes, const Rtec2ControlBlock& block, uint32_t from, uint32_t budget);
};

#endif
