#include "Rtec2RingReader.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
	void putU16(uint8_t* out, uint16_t value)
	{
		out[0] = static_cast<uint8_t>(value & 0xFF);
		out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
	}

	void putU32(uint8_t* out, uint32_t value)
	{
		out[0] = static_cast<uint8_t>(value & 0xFF);
		out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
		out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
		out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
	}

	/* The record body: the same 24 byte header the stream format uses. */
	size_t writeBody(uint8_t* body, uint8_t type, uint32_t sequence, uint32_t subject, uint32_t object, uint32_t arg0, uint32_t arg1,
	                 bool ticks, uint32_t ticksValue)
	{
		body[0] = type;
		body[1] = ticks ? 0x01 : 0x00;
		putU16(body + 2, 0);
		putU32(body + 4, sequence);
		putU32(body + 8, subject);
		putU32(body + 12, object);
		putU32(body + 16, arg0);
		putU32(body + 20, arg1);

		size_t used = 24;

		if (ticks)
		{
			putU32(body + used, ticksValue);
			used += 4;
		}

		return used;
	}

	void writeSlot(uint8_t* slot, uint32_t sequence, uint8_t type, uint32_t subject, uint32_t ticksValue, bool breakTailCheck = false,
	               bool breakSeqCheck = false)
	{
		std::memset(slot, 0, Rtec2RingReader::slotBytes);

		putU32(slot, sequence);
		putU32(slot + 4, breakSeqCheck ? 0xDEADBEEF : ~sequence);

		writeBody(slot + 8, type, sequence, subject, 0, 0, 0, true, ticksValue);

		putU32(slot + 52, breakTailCheck ? 0xDEADBEEF : ~sequence);
	}

	Rtec2ControlBlock makeBlock(uint32_t capacity, uint32_t head, uint32_t tail, uint64_t epoch = 1)
	{
		Rtec2ControlBlock block;
		block.wireVersion = 2;
		block.slotBytes = static_cast<uint16_t>(Rtec2RingReader::slotBytes);
		block.capacity = capacity;
		block.epoch = epoch;
		block.targetHz = 1000000;
		block.head = head;
		block.tail = tail;

		return block;
	}

	std::vector<uint8_t> makeControlBytes(const Rtec2ControlBlock& block)
	{
		std::vector<uint8_t> bytes(Rtec2RingReader::controlBlockBytes, 0);

		putU32(bytes.data(), Rtec2RingReader::controlMagic);
		putU16(bytes.data() + 4, block.wireVersion);
		putU16(bytes.data() + 6, block.slotBytes);
		putU32(bytes.data() + 8, block.capacity);
		putU32(bytes.data() + 12, static_cast<uint32_t>(block.epoch & 0xFFFFFFFFu));
		putU32(bytes.data() + 16, static_cast<uint32_t>(block.epoch >> 32));
		putU32(bytes.data() + 20, block.targetHz);
		putU32(bytes.data() + 24, block.head);
		putU32(bytes.data() + 28, block.tail);

		return bytes;
	}

	std::vector<uint8_t> makeRing(uint32_t capacity)
	{
		return std::vector<uint8_t>(Rtec2RingReader::slotBytes * capacity, 0);
	}
}  // namespace

TEST(Rtec2RingReaderTest, AControlBlockThatDescribesAUsableRingIsAccepted)
{
	const Rtec2ControlBlock block = makeBlock(16, 5, 2, 3);

	std::vector<uint8_t> bytes = makeControlBytes(block);
	Rtec2ControlBlock parsed;
	RtosTraceProblem problem = RtosTraceProblem::None;

	ASSERT_TRUE(Rtec2RingReader::parseControlBlock(bytes.data(), bytes.size(), parsed, problem));

	EXPECT_EQ(2, parsed.wireVersion);
	EXPECT_EQ(Rtec2RingReader::slotBytes, parsed.slotBytes);
	EXPECT_EQ(16u, parsed.capacity);
	EXPECT_EQ(3u, parsed.epoch);
	EXPECT_EQ(5u, parsed.head);
	EXPECT_EQ(2u, parsed.tail);
}

TEST(Rtec2RingReaderTest, AControlBlockThatCannotBeUsedSaysWhy)
{
	Rtec2ControlBlock parsed;
	RtosTraceProblem problem = RtosTraceProblem::None;

	std::vector<uint8_t> wrongMagic(Rtec2RingReader::controlBlockBytes, 0);
	EXPECT_FALSE(Rtec2RingReader::parseControlBlock(wrongMagic.data(), wrongMagic.size(), parsed, problem));
	EXPECT_EQ(RtosTraceProblem::NotAChoice, problem);

	Rtec2ControlBlock block = makeBlock(16, 0, 0);
	block.wireVersion = 1;
	std::vector<uint8_t> oldWire = makeControlBytes(block);
	EXPECT_FALSE(Rtec2RingReader::parseControlBlock(oldWire.data(), oldWire.size(), parsed, problem));
	EXPECT_EQ(RtosTraceProblem::UnsupportedWireVersion, problem);

	/* A capacity that is not a power of two indexes slots outside the ring;
	   what it finds there decodes into plausible records. Refused. */
	block = makeBlock(20, 0, 0);
	std::vector<uint8_t> badCapacity = makeControlBytes(block);
	EXPECT_FALSE(Rtec2RingReader::parseControlBlock(badCapacity.data(), badCapacity.size(), parsed, problem));
	EXPECT_EQ(RtosTraceProblem::ControlBlockCorrupt, problem);

	block = makeBlock(16, 0, 0);
	block.slotBytes = 48;
	std::vector<uint8_t> badSlot = makeControlBytes(block);
	EXPECT_FALSE(Rtec2RingReader::parseControlBlock(badSlot.data(), badSlot.size(), parsed, problem));
	EXPECT_EQ(RtosTraceProblem::ControlBlockCorrupt, problem);
}

TEST(Rtec2RingReaderTest, ThreeCompleteSlotsAreThreeRecordsAndTheCursorMoves)
{
	std::vector<uint8_t> ring = makeRing(16);

	writeSlot(ring.data() + 0 * Rtec2RingReader::slotBytes, 0, 0x13, 0x1000, 100);
	writeSlot(ring.data() + 1 * Rtec2RingReader::slotBytes, 1, 0x13, 0x1040, 200);
	writeSlot(ring.data() + 2 * Rtec2RingReader::slotBytes, 2, 0x13, 0x1000, 300);

	const Rtec2ControlBlock block = makeBlock(16, 3, 0);
	const Rtec2RingReader::Read read = Rtec2RingReader::read(ring.data(), ring.size(), block, 0, 10);

	ASSERT_EQ(3u, read.records.size());
	EXPECT_EQ(0x1000u, read.records[0].event.subject.rawHandle);
	EXPECT_EQ(200u, read.records[1].event.targetTicks);
	EXPECT_EQ(3u, read.committed);
	EXPECT_TRUE(read.advanced);
	EXPECT_EQ(0u, read.torn);
}

TEST(Rtec2RingReaderTest, OneBadSentinelAbandonsTheWholeBatch)
{
	std::vector<uint8_t> ring = makeRing(16);

	writeSlot(ring.data() + 0 * Rtec2RingReader::slotBytes, 0, 0x13, 0x1000, 100);

	/* The second slot was caught mid write: its closing sentinel is not the
	   check value yet. */
	writeSlot(ring.data() + 1 * Rtec2RingReader::slotBytes, 1, 0x13, 0x1040, 200, true);

	writeSlot(ring.data() + 2 * Rtec2RingReader::slotBytes, 2, 0x13, 0x1000, 300);

	const Rtec2ControlBlock block = makeBlock(16, 3, 0);
	const Rtec2RingReader::Read read = Rtec2RingReader::read(ring.data(), ring.size(), block, 0, 10);

	/* Keeping the first record would move the cursor past the one that was
	   dropped, and every later interval would be measured across a hole nobody
	   counted. Nothing is taken, and the cursor stays where it was. */
	EXPECT_TRUE(read.records.empty());
	EXPECT_EQ(2u, read.torn);
	EXPECT_EQ(0u, read.committed);
	EXPECT_FALSE(read.advanced);
}

TEST(Rtec2RingReaderTest, AHeadBehindTheCursorMeansTheTargetStartedAgain)
{
	std::vector<uint8_t> ring = makeRing(16);
	const Rtec2ControlBlock block = makeBlock(16, 2, 0);

	const Rtec2RingReader::Read read = Rtec2RingReader::read(ring.data(), ring.size(), block, 40, 10);

	EXPECT_TRUE(read.restarted);
	EXPECT_TRUE(read.records.empty());
	EXPECT_EQ(40u, read.committed);
}

TEST(Rtec2RingReaderTest, TheBudgetCapsHowMuchOneRoundTakes)
{
	std::vector<uint8_t> ring = makeRing(16);

	for (uint32_t index = 0; index < 5; index++)
		writeSlot(ring.data() + index * Rtec2RingReader::slotBytes, index, 0x13, 0x1000 + index, 100 * index);

	const Rtec2ControlBlock block = makeBlock(16, 5, 0);
	const Rtec2RingReader::Read read = Rtec2RingReader::read(ring.data(), ring.size(), block, 0, 2);

	ASSERT_EQ(2u, read.records.size());
	EXPECT_EQ(2u, read.committed);
}

TEST(Rtec2RingReaderTest, FallingMoreThanARingBehindIsCountedAsLost)
{
	std::vector<uint8_t> ring = makeRing(16);

	/* The cursor is 20 records behind a ring that holds 16: the oldest four are
	   gone, and saying so is the difference between a gap and a long run. */
	const Rtec2ControlBlock block = makeBlock(16, 20, 0);

	for (uint32_t index = 4; index < 20; index++)
		writeSlot(ring.data() + (index & 15) * Rtec2RingReader::slotBytes, index, 0x13, 0x1000, 100 * index);

	const Rtec2RingReader::Read read = Rtec2RingReader::read(ring.data(), ring.size(), block, 0, 100);

	EXPECT_EQ(4u, read.lost);
	EXPECT_EQ(16u, read.records.size());
	EXPECT_EQ(20u, read.committed);
}

TEST(Rtec2RingReaderTest, ARingShorterThanItsDescriptionIsNotRead)
{
	std::vector<uint8_t> ring = makeRing(4);  /* the block claims 16 */
	const Rtec2ControlBlock block = makeBlock(16, 3, 0);

	const Rtec2RingReader::Read read = Rtec2RingReader::read(ring.data(), ring.size(), block, 0, 10);

	EXPECT_TRUE(read.records.empty());
	EXPECT_FALSE(read.advanced);
	EXPECT_EQ(0u, read.committed);
}

/*
 * The windowed read, which is what the acquisition thread actually uses: a
 * contiguous run starting at the cursor, instead of the whole ring.
 *
 * The saving is the point - 3.5 KB a round to collect five records is more time
 * than a round has - but the behaviour has to be identical, so these check the
 * three answers that matter: a clean run, a run that would wrap, and a buffer
 * shorter than the records inside it.
 */
TEST(Rtec2RingReaderTest, AWindowDecodesOnlyTheRunItWasGiven)
{
	const uint32_t capacity = 8;
	std::vector<uint8_t> window(capacity * Rtec2RingReader::slotBytes, 0);

	/* Sequence 10 and 11, in the first two slots of the window. */
	writeSlot(window.data() + 0 * Rtec2RingReader::slotBytes, 10, 0x13, 0x1000, 1000);
	writeSlot(window.data() + 1 * Rtec2RingReader::slotBytes, 11, 0x13, 0x1040, 2000);

	const Rtec2ControlBlock block = makeBlock(16, 12, 0);

	const Rtec2RingReader::Read read = Rtec2RingReader::readWindow(window.data(), 2 * Rtec2RingReader::slotBytes, block, 10, 32);

	EXPECT_FALSE(read.restarted);
	EXPECT_EQ(0u, read.torn);
	ASSERT_EQ(2u, read.records.size());
	EXPECT_EQ(0x1000u, read.records[0].event.subject.rawHandle);
	EXPECT_EQ(12u, read.committed);
	EXPECT_TRUE(read.advanced);
}

TEST(Rtec2RingReaderTest, AWindowThatWouldWrapIsRefusedRatherThanReordered)
{
	const uint32_t capacity = 8;
	std::vector<uint8_t> window(4 * Rtec2RingReader::slotBytes, 0);

	writeSlot(window.data() + 0 * Rtec2RingReader::slotBytes, 6, 0x13, 0x1000, 1000);
	writeSlot(window.data() + 1 * Rtec2RingReader::slotBytes, 7, 0x13, 0x1000, 1100);

	/* Two slots from 6 end at 8, the ring's end, and that is fine: */
	const Rtec2RingReader::Read fits = Rtec2RingReader::readWindow(window.data(), 2 * Rtec2RingReader::slotBytes, makeBlock(capacity, 8, 0), 6, 2);

	EXPECT_EQ(0u, fits.torn);
	EXPECT_EQ(2u, fits.records.size());

	/* Three would wrap, which the caller promised not to ask for. Refused
	   rather than reordered - the sentinels would not catch it, because a
	   wrapped window still gets every sequence right. */
	const Rtec2RingReader::Read wrapped = Rtec2RingReader::readWindow(window.data(), 3 * Rtec2RingReader::slotBytes, makeBlock(capacity, 9, 0), 6, 3);

	EXPECT_NE(0u, wrapped.torn);
	EXPECT_TRUE(wrapped.records.empty());
	EXPECT_EQ(6u, wrapped.committed);
}

TEST(Rtec2RingReaderTest, AWindowShorterThanTheRecordsIsJustShort)
{
	std::vector<uint8_t> window(2 * Rtec2RingReader::slotBytes, 0);

	writeSlot(window.data() + 0 * Rtec2RingReader::slotBytes, 3, 0x13, 0x1000, 1000);
	writeSlot(window.data() + 1 * Rtec2RingReader::slotBytes, 4, 0x13, 0x1000, 1100);

	/* Five records are waiting but only two slots were read: take the two, and
	   leave the rest for the next round. A short read is not a torn one. */
	const Rtec2ControlBlock block = makeBlock(16, 8, 0);

	const Rtec2RingReader::Read read = Rtec2RingReader::readWindow(window.data(), window.size(), block, 3, 32);

	EXPECT_EQ(0u, read.torn);
	ASSERT_EQ(2u, read.records.size());
	EXPECT_EQ(5u, read.committed);
}
