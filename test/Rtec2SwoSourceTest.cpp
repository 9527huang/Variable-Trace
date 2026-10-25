#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Rtec2Frames.hpp"
#include "Rtec2SwoSource.hpp"

namespace
{
	/* One record, per wire format §3: 24 byte header, optional ticks. */
	std::vector<uint8_t> record(uint8_t type, uint32_t sequence, uint32_t ticks, uint32_t arg0)
	{
		std::vector<uint8_t> out(28, 0);

		out[0] = type;
		out[1] = 0x01; /* hasTargetTicks */

		for (int byte = 0; byte < 4; byte++)
		{
			out[4 + byte] = static_cast<uint8_t>((sequence >> (byte * 8)) & 0xFF);
			out[16 + byte] = static_cast<uint8_t>((arg0 >> (byte * 8)) & 0xFF);
			out[24 + byte] = static_cast<uint8_t>((ticks >> (byte * 8)) & 0xFF);
		}

		return out;
	}

	std::vector<uint8_t> frame(const std::vector<std::vector<uint8_t>>& records)
	{
		std::vector<uint8_t> payload;

		for (const std::vector<uint8_t>& one : records)
			payload.insert(payload.end(), one.begin(), one.end());

		const uint32_t crc = Rtec2FrameReader::crc32(payload.data(), payload.size());

		std::vector<uint8_t> out = {'R', 'T', 2, static_cast<uint8_t>(records.size()),
		                            static_cast<uint8_t>(payload.size() & 0xFF), static_cast<uint8_t>((payload.size() >> 8) & 0xFF),
		                            static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>((crc >> 8) & 0xFF),
		                            static_cast<uint8_t>((crc >> 16) & 0xFF), static_cast<uint8_t>((crc >> 24) & 0xFF)};

		out.insert(out.end(), payload.begin(), payload.end());
		out.push_back(0x0A);

		/* What the probe actually hands over: the frame, wrapped in ITM packets
		   on the record port, four payload bytes each. The source unframes those
		   before it looks for 'R','T' - a test that fed bare frames would pass
		   while the real stream did not. */
		std::vector<uint8_t> wire;

		for (size_t at = 0; at < out.size(); at += 4)
		{
			wire.push_back(0x0B); /* port 1, four byte payload */

			for (size_t byte = 0; byte < 4; byte++)
				wire.push_back(at + byte < out.size() ? out[at + byte] : 0u);
		}

		return wire;
	}
}  // namespace

TEST(Rtec2SwoSourceTest, TheClockSyncTeachesTheUnitEveryOtherRecordNeeds)
{
	/* A stream has no control block, so the only thing that says what the ticks
	   are in is a record. Without it every event arrives with a time nobody can
	   compute, and the view says "no target time" about a number that is
	   right. */
	Rtec2SwoSource source;
	RtosTraceProblem problem = RtosTraceProblem::None;

	ASSERT_TRUE(source.start(problem));

	source.feed(frame({record(0x02, 1, 1000, 1000000),  /* ClockSync: 1 MHz */
	                   record(0x13, 2, 1200, 0)}));    /* TaskSwitchIn */

	const RtosTraceBatch batch = source.poll(0.01);

	ASSERT_EQ(2u, batch.events.size());
	EXPECT_EQ(1000000u, batch.events[0].targetHz);
	EXPECT_EQ(1000000u, batch.events[1].targetHz) << "the switch inherited the last clock";
	EXPECT_EQ(1200u, batch.events[1].targetTicks);
	EXPECT_EQ(1000000u, source.stats().targetHz);
}

TEST(Rtec2SwoSourceTest, AFrameSplitAcrossTwoReadsStillBecomesEvents)
{
	Rtec2SwoSource source;
	RtosTraceProblem problem = RtosTraceProblem::None;

	ASSERT_TRUE(source.start(problem));

	const std::vector<uint8_t> bytes = frame({record(0x02, 1, 10, 1000000), record(0x13, 2, 20, 0)});

	source.feed(bytes.data(), 7);

	EXPECT_TRUE(source.poll(0.01).empty());
	EXPECT_GT(source.pendingBytes(), 0u);

	source.feed(bytes.data() + 7, bytes.size() - 7);

	EXPECT_EQ(2u, source.poll(0.01).events.size());
}

TEST(Rtec2SwoSourceTest, CapabilitiesGrowWithWhatActuallyArrives)
{
	/* A stream carries no capability record anyone can rely on finding - the
	   one at boot is written before the host is listening. What the source can
	   honestly claim is what it has seen. */
	Rtec2SwoSource source;
	RtosTraceProblem problem = RtosTraceProblem::None;

	ASSERT_TRUE(source.start(problem));

	EXPECT_FALSE(source.capabilities().isrEvents());

	source.feed(frame({record(0x02, 1, 10, 1000000), record(0x30, 2, 20, 0), record(0x31, 3, 30, 0)}));
	source.poll(0.01);

	EXPECT_TRUE(source.capabilities().isrEvents());
	EXPECT_FALSE(source.capabilities().heapEvents());
}

TEST(Rtec2SwoSourceTest, ABadFrameCostsItsRecordsAndNothingElse)
{
	Rtec2SwoSource source;
	RtosTraceProblem problem = RtosTraceProblem::None;

	ASSERT_TRUE(source.start(problem));

	std::vector<uint8_t> broken = frame({record(0x13, 1, 10, 0)});

	broken[13] ^= 0xFF;

	source.feed(broken);
	source.feed(frame({record(0x13, 2, 20, 0)}));

	const RtosTraceBatch batch = source.poll(0.01);

	ASSERT_EQ(1u, batch.events.size());
	EXPECT_EQ(20u, batch.events[0].targetTicks);
	EXPECT_EQ(1u, source.stats().badFrames);
}

TEST(Rtec2SwoSourceTest, NothingIsFedBeforeTheSourceIsStarted)
{
	/* The source is attached and started before the carrier is, and a probe
	   that hands over bytes in between must not build a history the pipeline
	   will reject. */
	Rtec2SwoSource source;

	source.feed(frame({record(0x13, 1, 10, 0)}));

	EXPECT_TRUE(source.poll(0.01).empty());
	EXPECT_EQ(RtosTraceSourceState::NotStarted, source.state());
}
