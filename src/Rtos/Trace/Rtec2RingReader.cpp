#include "Rtec2RingReader.hpp"

#include <cstring>

namespace
{
	uint16_t readU16(const uint8_t* bytes)
	{
		return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
	}

	uint32_t readU32(const uint8_t* bytes)
	{
		return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
		       (static_cast<uint32_t>(bytes[3]) << 24);
	}

	bool isPowerOfTwo(uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	/* One slot, if both sentinels and the sequence agree. */
	bool slotHoldsRecord(const uint8_t* slot, uint32_t sequence, Rtec2DecodedRecord& decoded)
	{
		const uint32_t seq = readU32(slot);
		const uint32_t seqCheck = readU32(slot + 4);
		const uint32_t tailCheck = readU32(slot + 52);

		if (seq != sequence || seqCheck != ~sequence || tailCheck != ~sequence)
			return false;

		/* The record body is the same 24 byte header the stream format uses,
		   starting after the two sentinels; 44 bytes are available before the
		   closing sentinel, and the largest record is 41. */
		const uint8_t* body = slot + 8;

		uint8_t flags = body[1];
		size_t needed = 24;

		if ((flags & 0x01u) != 0)
			needed += 4;

		const uint8_t payloadBytes = (flags & 0x02u) != 0 ? body[needed] : 0;

		if ((flags & 0x02u) != 0 && payloadBytes > 12)
			return false;

		(void)seqCheck;

		/* Decoding goes through the same function the stream path uses: one
		   decoder, so a fix in one place is a fix in both. */
		const Rtec2DecodeResult result = Rtec2Decoder::decode(body, 24 + ((flags & 0x01u) != 0 ? 4 : 0) + ((flags & 0x02u) != 0 ? 1u + payloadBytes : 0));

		if (result.records.size() != 1 || result.truncatedBytes != 0)
			return false;

		decoded = result.records.front();

		return true;
	}
}  // namespace

bool Rtec2RingReader::parseControlBlock(const uint8_t* bytes, size_t size, Rtec2ControlBlock& block, RtosTraceProblem& problem)
{
	problem = RtosTraceProblem::None;

	if (bytes == nullptr || size < controlBlockBytes)
	{
		problem = RtosTraceProblem::ControlBlockMissing;

		return false;
	}

	if (readU32(bytes) != controlMagic)
	{
		problem = RtosTraceProblem::NotAChoice;

		return false;
	}

	block.wireVersion = readU16(bytes + 4);
	block.slotBytes = readU16(bytes + 6);

	/* The wire version is checked before anything is read with it: a newer
	   target may well have moved every field after this one. */
	if (block.wireVersion != 2)
	{
		problem = RtosTraceProblem::UnsupportedWireVersion;

		return false;
	}

	if (block.slotBytes != slotBytes)
	{
		problem = RtosTraceProblem::ControlBlockCorrupt;

		return false;
	}

	block.capacity = readU32(bytes + 8);
	block.epoch = (static_cast<uint64_t>(readU32(bytes + 16)) << 32) | readU32(bytes + 12);
	block.targetHz = readU32(bytes + 20);
	block.head = readU32(bytes + 24);
	block.tail = readU32(bytes + 28);
	block.generation = readU32(bytes + 32);
	block.rejected = readU32(bytes + 36);
	block.overwritten = readU32(bytes + 40);
	block.committedSeq = readU32(bytes + 44);
	block.capabilities = readU32(bytes + 48);
	block.faultCount = readU32(bytes + 52);

	if (!isPowerOfTwo(block.capacity) || block.capacity < 16)
	{
		/* A capacity that is not a power of two makes the index arithmetic
		   produce slots that are not part of the ring, which then decode into
		   plausible records. Refused rather than clamped. */
		problem = RtosTraceProblem::ControlBlockCorrupt;

		return false;
	}

	return true;
}

Rtec2RingReader::Read Rtec2RingReader::read(const uint8_t* slots, size_t bytes, const Rtec2ControlBlock& block, uint32_t from, uint32_t budget)
{
	Read result;

	result.committed = from;

	if (slots == nullptr || block.capacity == 0)
		return result;

	const size_t available = bytes / slotBytes;

	if (available < block.capacity)
		return result;

	/* The writer went backwards: it started again, and its ring begins at zero.
	   Waiting for a head already passed would wait for ever. */
	if (static_cast<int32_t>(block.head - from) < 0)
	{
		result.restarted = true;

		return result;
	}

	uint32_t count = block.head - from;

	if (budget != 0 && count > budget)
		count = budget;

	if (count == 0)
	{
		result.advanced = true;

		return result;
	}

	/* More than a ring behind: the oldest records are gone. The count is
	   reported so that the interval across the hole is not drawn as one run. */
	if (count > block.capacity)
	{
		result.lost = count - block.capacity;
		from = block.head - block.capacity;
		count = block.capacity;
	}

	std::vector<Rtec2DecodedRecord> decoded;
	decoded.reserve(count);

	for (uint32_t index = 0; index < count; index++)
	{
		const uint32_t sequence = from + index;
		const uint8_t* slot = slots + static_cast<size_t>(sequence & (block.capacity - 1)) * slotBytes;

		Rtec2DecodedRecord record;

		if (!slotHoldsRecord(slot, sequence, record))
		{
			/* One bad slot abandons the batch. Keeping the good prefix would
			   move the cursor past records that were dropped, and every later
			   interval would be measured across an uncounted hole. */
			result.torn = count - index;
			decoded.clear();

			return result;
		}

		decoded.push_back(record);
	}

	result.records = std::move(decoded);
	result.committed = from + count;
	result.advanced = true;

	return result;
}
