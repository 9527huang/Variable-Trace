#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Rtec2Frames.hpp"

namespace
{
	/* One record, as wire format §3 lays it out: 24 bytes, ticks, no payload. */
	void appendRecord(std::vector<uint8_t>& out, uint8_t type, uint32_t sequence, uint32_t ticks)
	{
		const size_t at = out.size();

		out.resize(at + 28, 0);

		out[at] = type;
		out[at + 1] = 0x01; /* hasTargetTicks */

		for (int byte = 0; byte < 4; byte++)
		{
			out[at + 4 + byte] = static_cast<uint8_t>((sequence >> (byte * 8)) & 0xFF);
			out[at + 24 + byte] = static_cast<uint8_t>((ticks >> (byte * 8)) & 0xFF);
		}
	}

	std::vector<uint8_t> makeFrame(const std::vector<std::vector<uint8_t>>& records)
	{
		std::vector<uint8_t> payload;

		for (const std::vector<uint8_t>& record : records)
			payload.insert(payload.end(), record.begin(), record.end());

		const uint32_t crc = Rtec2FrameReader::crc32(payload.data(), payload.size());

		std::vector<uint8_t> frame = {'R', 'T', 2, static_cast<uint8_t>(records.size()),
		                              static_cast<uint8_t>(payload.size() & 0xFF), static_cast<uint8_t>((payload.size() >> 8) & 0xFF),
		                              static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>((crc >> 8) & 0xFF),
		                              static_cast<uint8_t>((crc >> 16) & 0xFF), static_cast<uint8_t>((crc >> 24) & 0xFF)};

		frame.insert(frame.end(), payload.begin(), payload.end());
		frame.push_back(0x0A);

		return frame;
	}

	std::vector<std::vector<uint8_t>> twoRecords()
	{
		std::vector<std::vector<uint8_t>> records(2);

		appendRecord(records[0], 0x13, 1, 1000);
		appendRecord(records[1], 0x30, 2, 1040);

		return records;
	}
}  // namespace

TEST(Rtec2FramesTest, TheCrcIsTheOneTheWireFormatNames)
{
	/* The check value for "123456789" is 0x0376E6E7. Without it a CRC that only
	   agrees with itself would pass every round trip in this file. */
	const char* text = "123456789";

	EXPECT_EQ(0x0376E6E7u, Rtec2FrameReader::crc32(reinterpret_cast<const uint8_t*>(text), 9));
}

TEST(Rtec2FramesTest, AWholeFrameComesBackWithItsRecords)
{
	Rtec2FrameReader reader;

	reader.feed(makeFrame(twoRecords()));

	const std::vector<Rtec2Frame> frames = reader.take();

	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(2u, frames[0].count);
	EXPECT_EQ(2u, frames[0].version);
	EXPECT_EQ(56u, frames[0].payload.size());
	EXPECT_EQ(0u, reader.pendingBytes());
	EXPECT_EQ(1u, reader.stats().frames);
}

TEST(Rtec2FramesTest, AFrameThatArrivesInPiecesIsHeldUntilItsLastByte)
{
	Rtec2FrameReader reader;
	const std::vector<uint8_t> frame = makeFrame(twoRecords());

	/* Byte at a time, which is what a probe hands over when the wire is slow. */
	for (size_t index = 0; index + 1 < frame.size(); index++)
	{
		reader.feed(&frame[index], 1);

		EXPECT_TRUE(reader.take().empty()) << "a frame was handed over at byte " << index;
	}

	reader.feed(&frame[frame.size() - 1], 1);

	EXPECT_EQ(1u, reader.take().size());
}

TEST(Rtec2FramesTest, ABadCrcCostsTheFrameAndNotTheNextOne)
{
	std::vector<uint8_t> broken = makeFrame(twoRecords());

	broken[12] ^= 0xFF; /* a byte of the payload */

	Rtec2FrameReader reader;

	reader.feed(broken);
	reader.feed(makeFrame(twoRecords()));

	const std::vector<Rtec2Frame> frames = reader.take();

	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(1u, reader.stats().crcFailures);
}

TEST(Rtec2FramesTest, AFrameWithoutItsEndMarkerIsRefused)
{
	std::vector<uint8_t> broken = makeFrame(twoRecords());

	broken.back() = 0x00;

	Rtec2FrameReader reader;

	reader.feed(broken);

	EXPECT_TRUE(reader.take().empty());
	EXPECT_EQ(1u, reader.stats().badEnd);
}

TEST(Rtec2FramesTest, ALengthOverTheCapIsNotAFrame)
{
	/* The cap is not tidiness: a length read from a stream that has slipped a
	   byte will be enormous, and reserving for it is the whole problem. */
	std::vector<uint8_t> bogus = {'R', 'T', 2, 1, 0xFF, 0xFF, 0, 0, 0, 0};

	Rtec2FrameReader reader;

	reader.feed(bogus);
	reader.feed(makeFrame(twoRecords()));

	EXPECT_EQ(1u, reader.take().size());
	EXPECT_EQ(1u, reader.stats().badLength);
}

TEST(Rtec2FramesTest, JunkBeforeTheSyncIsCountedAndDropped)
{
	Rtec2FrameReader reader;
	std::vector<uint8_t> bytes(37, 0x5A);

	const std::vector<uint8_t> frame = makeFrame(twoRecords());

	bytes.insert(bytes.end(), frame.begin(), frame.end());

	reader.feed(bytes);

	EXPECT_EQ(1u, reader.take().size());
	EXPECT_EQ(37u, reader.stats().resyncBytes);
}

TEST(Rtec2FramesTest, APayloadThatDoesNotHoldTheRecordsItClaimsIsRefused)
{
	/* The count is the writer's promise and the payload is the evidence; a
	   frame where they disagree is a frame whose writer and reader are not
	   talking about the same format. */
	std::vector<uint8_t> frame = makeFrame(twoRecords());

	frame[3] = 3; /* claims three records, holds two */

	const uint32_t crc = Rtec2FrameReader::crc32(frame.data() + 10, frame.size() - 11);

	frame[6] = static_cast<uint8_t>(crc & 0xFF);
	frame[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
	frame[8] = static_cast<uint8_t>((crc >> 16) & 0xFF);
	frame[9] = static_cast<uint8_t>((crc >> 24) & 0xFF);

	Rtec2FrameReader reader;

	reader.feed(frame);

	EXPECT_TRUE(reader.take().empty());
	EXPECT_EQ(1u, reader.stats().badPayload);
}

TEST(Rtec2FramesTest, ManyFramesInOneReadAllComeOut)
{
	/* What a probe actually hands over: a block that holds several frames, or
	   the end of one and the start of the next. */
	Rtec2FrameReader reader;
	std::vector<uint8_t> bytes;

	for (int index = 0; index < 5; index++)
	{
		const std::vector<uint8_t> frame = makeFrame(twoRecords());

		bytes.insert(bytes.end(), frame.begin(), frame.end());
	}

	reader.feed(bytes);

	const std::vector<Rtec2Frame> frames = reader.take();

	EXPECT_EQ(5u, frames.size());
	EXPECT_EQ(0u, reader.stats().crcFailures);
	EXPECT_EQ(0u, reader.pendingBytes());
}
