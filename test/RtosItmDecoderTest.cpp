#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "RtosItmDecoder.hpp"

namespace
{
	/* A software-source packet: the port in bits 7:3, the size code in bits 1:0,
	   where three means four bytes. One byte of payload is what the target
	   writes for each byte of a record. */
	uint8_t sourceHeader(uint8_t port, uint8_t size)
	{
		return static_cast<uint8_t>((port << 3) | size);
	}

	void appendRecord(std::vector<uint8_t>& stream, uint32_t tcb, uint32_t timeUs)
	{
		const uint8_t bytes[8] = {static_cast<uint8_t>(tcb), static_cast<uint8_t>(tcb >> 8), static_cast<uint8_t>(tcb >> 16),
		                          static_cast<uint8_t>(tcb >> 24), static_cast<uint8_t>(timeUs), static_cast<uint8_t>(timeUs >> 8),
		                          static_cast<uint8_t>(timeUs >> 16), static_cast<uint8_t>(timeUs >> 24)};

		for (const uint8_t byte : bytes)
		{
			stream.push_back(sourceHeader(RtosItmDecoder::recordPort, 1));
			stream.push_back(byte);
		}
	}
}  // namespace

TEST(RtosItmDecoder, ARecordWrittenOneByteAtATimeComesBackWhole)
{
	std::vector<uint8_t> stream;
	appendRecord(stream, 0x20000100, 123456);

	RtosItmDecoder decoder;
	decoder.feed(stream.data(), stream.size());

	const std::vector<RtosEventRing::Record> records = decoder.take();

	ASSERT_EQ(records.size(), 1u);
	EXPECT_EQ(records[0].tcbAddress, 0x20000100u);
	EXPECT_EQ(records[0].timeUs, 123456u);
}

TEST(RtosItmDecoder, TimestampsBetweenRecordsAreSteppedOver)
{
	std::vector<uint8_t> stream;

	appendRecord(stream, 0x100, 1000);

	/* A local timestamp: a zero low nibble and a non-zero middle, with one
	   continuation byte after it. */
	stream.push_back(0x90);
	stream.push_back(0x81);
	stream.push_back(0x02);

	appendRecord(stream, 0x200, 2000);

	RtosItmDecoder decoder;
	decoder.feed(stream.data(), stream.size());

	const std::vector<RtosEventRing::Record> records = decoder.take();

	ASSERT_EQ(records.size(), 2u);
	EXPECT_EQ(records[0].tcbAddress, 0x100u);
	EXPECT_EQ(records[1].tcbAddress, 0x200u);
}

TEST(RtosItmDecoder, AnotherPortsBytesDoNotBecomeRecords)
{
	std::vector<uint8_t> stream;

	appendRecord(stream, 0x100, 1000);

	/* Four bytes on port 3, then a record on ours. */
	stream.push_back(sourceHeader(3, 3));
	stream.push_back(0xAA);
	stream.push_back(0xBB);
	stream.push_back(0xCC);
	stream.push_back(0xDD);

	appendRecord(stream, 0x200, 2000);

	RtosItmDecoder decoder;
	decoder.feed(stream.data(), stream.size());

	const std::vector<RtosEventRing::Record> records = decoder.take();

	ASSERT_EQ(records.size(), 2u);
	EXPECT_EQ(records[1].tcbAddress, 0x200u);
}

TEST(RtosItmDecoder, ARecordSplitAcrossTwoReadsWaitsForItsOtherHalf)
{
	std::vector<uint8_t> stream;
	appendRecord(stream, 0x300, 3000);

	RtosItmDecoder decoder;

	decoder.feed(stream.data(), 8);
	EXPECT_TRUE(decoder.take().empty());

	decoder.feed(stream.data() + 8, stream.size() - 8);

	const std::vector<RtosEventRing::Record> records = decoder.take();

	ASSERT_EQ(records.size(), 1u);
	EXPECT_EQ(records[0].tcbAddress, 0x300u);
}

TEST(RtosItmDecoder, SyncZerosAreHarmless)
{
	std::vector<uint8_t> stream{0x00, 0x00, 0x00};
	appendRecord(stream, 0x400, 4000);
	stream.push_back(0x00);
	stream.push_back(0x00);

	RtosItmDecoder decoder;
	decoder.feed(stream.data(), stream.size());

	const std::vector<RtosEventRing::Record> records = decoder.take();

	ASSERT_EQ(records.size(), 1u);
	EXPECT_EQ(records[0].timeUs, 4000u);
}

TEST(RtosItmDecoder, TooFewBytesMakeNoRecord)
{
	std::vector<uint8_t> stream;
	stream.push_back(sourceHeader(RtosItmDecoder::recordPort, 1));
	stream.push_back(0x11);

	RtosItmDecoder decoder;
	decoder.feed(stream.data(), stream.size());

	EXPECT_TRUE(decoder.take().empty());
}
