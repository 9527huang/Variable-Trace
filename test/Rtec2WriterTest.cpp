#include "Rtec2RingReader.hpp"
#include "rtec2_writer.h"

#include <gtest/gtest.h>

#include <vector>

/*
 * The closed loop: C writes slots, C++ reads them.
 *
 * Everything before this tested one side against a hand-made input. Here the
 * writer and the reader are two implementations of the same document looking at
 * the same bytes, which is the only arrangement that can catch the two of them
 * agreeing on a wrong reading of the format.
 */

namespace
{
	constexpr uint32_t capacity = 16;

	struct Ring
	{
		explicit Ring(uint32_t slots = capacity) : memory(RTEC2_CONTROL_BYTES + slots * RTEC2_SLOT_BYTES, 0)
		{
			rtec2_writer_init(&writer, memory.data(), slots, 7, 0, 1000000u,
			                  RTEC2_CAP_RAW_HANDLE32 | RTEC2_CAP_LIFECYCLE | RTEC2_CAP_TARGET_CLOCK | RTEC2_CAP_EVENT_SEQUENCE);
		}

		Rtec2ControlBlock control() const
		{
			Rtec2ControlBlock block;
			RtosTraceProblem problem = RtosTraceProblem::None;

			EXPECT_TRUE(Rtec2RingReader::parseControlBlock(memory.data(), memory.size(), block, problem)) << describe(problem);

			return block;
		}

		Rtec2RingReader::Read read(uint32_t from, uint32_t budget = 64) const
		{
			return Rtec2RingReader::read(memory.data() + RTEC2_CONTROL_BYTES, memory.size() - RTEC2_CONTROL_BYTES, control(), from, budget);
		}

		std::vector<uint8_t> memory;
		rtec2_writer writer;
	};

	rtec2_event switchIn(uint32_t tcb, uint32_t ticks)
	{
		rtec2_event event;

		event.type = 0x13;
		event.hasTicks = 1;
		event.ticks = ticks;
		event.subject = tcb;
		event.object = 0;
		event.arg0 = 0;
		event.arg1 = 0;
		event.payload = 0;
		event.payloadBytes = 0;

		return event;
	}

	rtec2_result emit(Ring& ring, uint32_t tcb, uint32_t ticks)
	{
		rtec2_event event = switchIn(tcb, ticks);

		return rtec2_writer_emit(&ring.writer, &event);
	}

}  // namespace

TEST(Rtec2WriterTest, WhatTheWriterWritesTheReaderReads)
{
	Ring ring;

	EXPECT_EQ(RTEC2_OK, emit(ring, 0x20001000u, 500));
	EXPECT_EQ(RTEC2_OK, emit(ring, 0x20001040u, 900));

	const Rtec2ControlBlock control = ring.control();
	EXPECT_EQ(3u, control.head);
	EXPECT_EQ(1u, control.tail);

	const Rtec2RingReader::Read read = ring.read(control.tail);

	ASSERT_EQ(2u, read.records.size());
	EXPECT_EQ(RtosTraceEventType::TaskSwitchIn, read.records[0].event.type);
	EXPECT_EQ(0x20001000u, read.records[0].event.subject.rawHandle);
	EXPECT_EQ(500u, read.records[0].event.targetTicks);
	EXPECT_EQ(0x20001040u, read.records[1].event.subject.rawHandle);
	EXPECT_EQ(900u, read.records[1].event.targetTicks);
}

TEST(Rtec2WriterTest, AFaultFrameWithAllTwelveBytesSurvivesTheRoundTrip)
{
	Ring ring;

	const uint8_t payload[12] = {1, 0x03, 0x02, 0x00, 0x10, 0x20, 0x00, 0x08, 0, 0, 0, 0};

	rtec2_event fault;
	fault.type = 0x40;
	fault.hasTicks = 1;
	fault.ticks = 1200;
	fault.subject = 0;
	fault.object = 0;
	fault.arg0 = 0;
	fault.arg1 = 0;
	fault.payload = payload;
	fault.payloadBytes = 12;

	EXPECT_EQ(RTEC2_OK, rtec2_writer_emit(&ring.writer, &fault));

	const Rtec2RingReader::Read read = ring.read(1);

	ASSERT_EQ(1u, read.records.size());
	EXPECT_EQ(RtosTraceEventType::Fault, read.records[0].event.type);
	EXPECT_EQ(12u, read.records[0].payloadBytes);

	/* Twelve bytes is the whole point of the slot being 56 and not 48: a fault
	   frame that lost its last four bytes would lose SP. */
	for (uint8_t index = 0; index < 12; index++)
		EXPECT_EQ(payload[index], read.records[0].payload[index]) << "byte " << int(index);
}

TEST(Rtec2WriterTest, APayloadTooLargeIsRefusedNotCut)
{
	Ring ring;

	const uint8_t payload[13] = {0};

	rtec2_event event = switchIn(0x20001000u, 100);
	event.payload = payload;
	event.payloadBytes = 13;

	EXPECT_EQ(RTEC2_REJECTED, rtec2_writer_emit(&ring.writer, &event));

	/* Nothing was written, and the count says so: a tracer that silently drops
	   is a tracer whose output cannot be trusted to be complete. */
	EXPECT_EQ(1u, ring.control().head);
	EXPECT_EQ(1u, ring.writer.rejected);
}

TEST(Rtec2WriterTest, AFullRingOverwritesAndTheReaderCallsItLost)
{
	Ring ring(capacity);
	ring.writer.overwrite = 1;

	for (uint32_t index = 0; index < 20; index++)
	{
		EXPECT_EQ(index < capacity ? RTEC2_OK : RTEC2_OK_OVERWROTE, emit(ring, 0x20000000u + index, index * 10));
	}

	EXPECT_EQ(4u, ring.writer.overwritten);

	/* The reader is still at one: four records were overwritten before it got
	   to them, and it has to hear that rather than draw one long run. */
	const Rtec2RingReader::Read read = ring.read(1, 64);

	EXPECT_EQ(4u, read.lost);
	EXPECT_EQ(16u, read.records.size());
	EXPECT_EQ(21u, read.committed);
}

TEST(Rtec2WriterTest, AFullRingCanDropInsteadOfOverwrite)
{
	Ring ring(capacity);
	ring.writer.overwrite = 0;

	for (uint32_t index = 0; index < capacity; index++)
		EXPECT_EQ(RTEC2_OK, emit(ring, 0x20001000u, index));

	/* The ring is full and the policy is to drop: the new event is counted and
	   the old ones are left alone. */
	EXPECT_EQ(RTEC2_DROPPED, emit(ring, 0x20001000u, 999));
	EXPECT_EQ(1u, ring.writer.rejected);
	EXPECT_EQ(1u + capacity, ring.control().head);
}

TEST(Rtec2WriterTest, ANewEpochSendsTheHeadBackAndTheReaderNotices)
{
	Ring ring;

	for (uint32_t index = 0; index < 5; index++)
		emit(ring, 0x20001000u, index);

	const uint32_t before = ring.control().head;
	EXPECT_EQ(6u, before);

	rtec2_writer_next_epoch(&ring.writer, 8, 0);

	const Rtec2ControlBlock control = ring.control();
	EXPECT_EQ(1u, control.head);
	EXPECT_EQ(8u, control.epoch);

	/* A reader whose cursor is where it was is now ahead of the writer: that is
	   a restart, and waiting for a head already passed would wait for ever. */
	const Rtec2RingReader::Read read = ring.read(before, 64);
	EXPECT_TRUE(read.restarted);
}

TEST(Rtec2WriterTest, ASlotCaughtMidWriteTakesTheWholeBatchWithIt)
{
	Ring ring;

	for (uint32_t index = 0; index < 3; index++)
		emit(ring, 0x20001000u, index);

	/* Stand in for the target being interrupted halfway through slot 2: its
	   closing sentinel is not yet the check value. */
	uint8_t* second = ring.memory.data() + RTEC2_CONTROL_BYTES + 1 * RTEC2_SLOT_BYTES;
	second[52] = 0xFF;
	second[53] = 0xFF;
	second[54] = 0xFF;
	second[55] = 0xFF;

	const Rtec2RingReader::Read read = ring.read(1);

	/* The first record is intact and is still not taken: a cursor that moved
	   past the hole would make every later interval wrong. */
	EXPECT_TRUE(read.records.empty());
	EXPECT_FALSE(read.advanced);
	EXPECT_EQ(1u, read.committed);
	EXPECT_GT(read.torn, 0u);
}

TEST(Rtec2WriterTest, TheDictionaryTakesShortNamesAndRefusesLongOnes)
{
	std::vector<uint8_t> area(RTEC2_DICTIONARY_SLOTS * RTEC2_DICTIONARY_NAME_BYTES, 0);

	rtec2_dictionary dictionary;
	rtec2_dictionary_init(&dictionary, area.data());

	const uint16_t slot = rtec2_dictionary_add(&dictionary, "worker");
	ASSERT_NE(0xFFFFu, slot);
	EXPECT_EQ(std::string("worker"), std::string(reinterpret_cast<const char*>(area.data() + slot * RTEC2_DICTIONARY_NAME_BYTES)));

	/* Two names that differ only after the fifteenth character would read as
	   one, so a name that does not fit is refused. */
	EXPECT_EQ(0xFFFFu, rtec2_dictionary_add(&dictionary, "a-very-long-task-name"));

	EXPECT_EQ(rtec2_name_hash("worker"), rtec2_name_hash("worker"));
	EXPECT_NE(rtec2_name_hash("worker"), rtec2_name_hash("worker2"));
}
