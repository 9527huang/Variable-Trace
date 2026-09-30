#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "SerialProtocol.hpp"

/*
 * The wire format belongs to the target driver, so the checks here are mostly
 * about matching it byte for byte rather than about internal consistency: a
 * frame the firmware drops looks exactly like a working one from the host side
 * until nothing comes back.
 *
 * The expected frames below were worked out with a separate implementation of
 * the checksum, from the table and the state machine in serialDriver.h v1.2,
 * not by running this code and pasting the result.
 */
namespace
{
	std::vector<uint8_t> bytes(std::initializer_list<uint8_t> values)
	{
		return std::vector<uint8_t>(values);
	}

	std::string toHex(const std::vector<uint8_t>& data)
	{
		static const char* digits = "0123456789abcdef";
		std::string text;

		for (uint8_t byte : data)
		{
			text.push_back(digits[(byte >> 4) & 0x0F]);
			text.push_back(digits[byte & 0x0F]);
		}

		return text;
	}

	/* Appends the checksum the driver would compute, so that a frame can be made
	   wrong in one place on purpose. */
	std::vector<uint8_t> withChecksum(std::vector<uint8_t> frame)
	{
		const uint16_t checksum = serial::crc16(frame);
		frame.push_back(static_cast<uint8_t>(checksum & 0xFF));
		frame.push_back(static_cast<uint8_t>((checksum >> 8) & 0xFF));
		return frame;
	}

	std::optional<serial::Request> parse(const std::vector<uint8_t>& frame, serial::RequestParser& parser)
	{
		std::optional<serial::Request> result;

		for (uint8_t byte : frame)
		{
			std::optional<serial::Request> step = parser.push(byte);

			if (step.has_value())
				result = step;
		}

		return result;
	}
}  // namespace

TEST(SerialChecksumTest, MatchesTheTableInTheDriver)
{
	/* A published check value for this variant of CRC-16, computed from the same
	   sixteen entry table the driver carries. */
	const std::string text = "123456789";
	EXPECT_EQ(serial::crc16(reinterpret_cast<const uint8_t*>(text.data()), text.size()), 0xC2C5);
}

TEST(SerialChecksumTest, SeededWithAllOnes)
{
	EXPECT_EQ(serial::crc16(nullptr, 0), 0xFFFF);
	EXPECT_EQ(serial::crc16(std::vector<uint8_t>{}), 0xFFFF);
}

TEST(SerialChecksumTest, IsFedOneByteAtATimeTheSameWay)
{
	std::vector<uint8_t> data = bytes({0xAA, 0x55, 0x01, 0x01, 0x00, 0x00, 0x00, 0x20, 0x04});

	uint16_t incremental = 0xFFFF;

	for (uint8_t byte : data)
		incremental = serial::crc16Update(incremental, byte);

	EXPECT_EQ(incremental, serial::crc16(data));
}

TEST(SerialRequestTest, ABulkReadMatchesTheFrameTheTargetExpects)
{
	const std::vector<uint8_t> frame = serial::buildReadRequest({{0x20000000, 4}});
	EXPECT_EQ(toHex(frame), "aa55010100000020" "04" "36ca");
}

TEST(SerialRequestTest, SeveralEntriesFollowEachOther)
{
	std::vector<serial::ReadEntry> entries = {{0x20000000, 4}, {0x20000004, 2}};
	const std::vector<uint8_t> frame = serial::buildReadRequest(entries);
	EXPECT_EQ(toHex(frame), "aa55010200000020" "04" "04000020" "02" "67c0");
}

TEST(SerialRequestTest, AWriteCarriesTheValueLittleEndian)
{
	const uint8_t value[4] = {0x44, 0x33, 0x22, 0x11};
	const std::vector<uint8_t> frame = serial::buildWriteRequest(0x20000000, value, 4);
	EXPECT_EQ(toHex(frame), "aa55020400000020" "44332211" "e7cb");
}

TEST(SerialRequestTest, ABufferReadCarriesNoData)
{
	const std::vector<uint8_t> frame = serial::buildBufferReadRequest(0x20000100, 16);
	EXPECT_EQ(toHex(frame), "aa550310" "00010020" "9ccd");
}

TEST(SerialRequestTest, ABulkReadRefusesWhatTheDriverWouldDrop)
{
	EXPECT_THROW(serial::buildReadRequest({}), std::invalid_argument);

	std::vector<serial::ReadEntry> tooMany;

	for (size_t index = 0; index <= serial::defaultMaxVariables; index++)
		tooMany.push_back({0x20000000 + static_cast<uint32_t>(index) * 4, 4});

	EXPECT_THROW(serial::buildReadRequest(tooMany), std::invalid_argument);

	EXPECT_THROW(serial::buildReadRequest({{0x20000000, 0}}), std::invalid_argument);
	EXPECT_THROW(serial::buildReadRequest({{0x20000000, 5}}), std::invalid_argument);

	/* Ten entries of four bytes exactly fill the driver's answer buffer, and it
	   builds that answer before checking anything else. */
	std::vector<serial::ReadEntry> exactlyFull;

	for (size_t index = 0; index < serial::defaultMaxVariables; index++)
		exactlyFull.push_back({0x20000000 + static_cast<uint32_t>(index) * 4, 4});

	EXPECT_NO_THROW(serial::buildReadRequest(exactlyFull));
}

TEST(SerialRequestTest, AWriteCarriesOneToFourBytes)
{
	const uint8_t value[4] = {1, 2, 3, 4};
	EXPECT_THROW(serial::buildWriteRequest(0x20000000, value, 0), std::invalid_argument);
	EXPECT_THROW(serial::buildWriteRequest(0x20000000, value, 5), std::invalid_argument);
	EXPECT_THROW(serial::buildWriteRequest(0x20000000, nullptr, 4), std::invalid_argument);
	EXPECT_NO_THROW(serial::buildWriteRequest(0x20000000, value, 1));
}

TEST(SerialRequestTest, ABufferReadNeedsASize)
{
	EXPECT_THROW(serial::buildBufferReadRequest(0x20000000, 0), std::invalid_argument);
	EXPECT_NO_THROW(serial::buildBufferReadRequest(0x20000000, 255));
}

TEST(SerialRequestParserTest, ReadsBackWhatWasBuilt)
{
	serial::RequestParser parser;
	const std::vector<uint8_t> frame = serial::buildReadRequest({{0x20000000, 4}, {0x20001000, 2}});

	const std::optional<serial::Request> request = parse(frame, parser);

	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->command, serial::Command::Read);
	ASSERT_EQ(request->entries.size(), 2u);
	EXPECT_EQ(request->entries[0].address, 0x20000000u);
	EXPECT_EQ(request->entries[0].size, 4);
	EXPECT_EQ(request->entries[1].address, 0x20001000u);
	EXPECT_EQ(request->entries[1].size, 2);
	EXPECT_EQ(parser.getCrcErrorCount(), 0u);
	EXPECT_EQ(parser.getDroppedFrameCount(), 0u);
}

TEST(SerialRequestParserTest, ReadsBackAWrite)
{
	serial::RequestParser parser;
	const uint8_t value[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	const std::vector<uint8_t> frame = serial::buildWriteRequest(0x20000010, value, 4);

	const std::optional<serial::Request> request = parse(frame, parser);

	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->command, serial::Command::Write);
	EXPECT_EQ(request->address, 0x20000010u);
	ASSERT_EQ(request->payload.size(), 4u);
	EXPECT_EQ(request->payload[0], 0xDE);
	EXPECT_EQ(request->payload[3], 0xEF);
}

TEST(SerialRequestParserTest, ReadsBackABufferRead)
{
	serial::RequestParser parser;
	const std::vector<uint8_t> frame = serial::buildBufferReadRequest(0x20000400, 200);

	const std::optional<serial::Request> request = parse(frame, parser);

	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->command, serial::Command::BufferRead);
	EXPECT_EQ(request->address, 0x20000400u);
	EXPECT_EQ(request->bufferReadSize, 200);
}

TEST(SerialRequestParserTest, AWrongChecksumDropsTheFrame)
{
	serial::RequestParser parser;
	std::vector<uint8_t> frame = serial::buildReadRequest({{0x20000000, 4}});
	frame.back() ^= 0xFF;

	EXPECT_FALSE(parse(frame, parser).has_value());
	EXPECT_EQ(parser.getCrcErrorCount(), 1u);
}

TEST(SerialRequestParserTest, AnUnknownCommandDropsTheFrame)
{
	serial::RequestParser parser;
	const std::vector<uint8_t> frame = withChecksum(bytes({0xAA, 0x55, 0x09, 0x01, 0x00, 0x00, 0x00, 0x20, 0x04}));

	EXPECT_FALSE(parse(frame, parser).has_value());
	EXPECT_EQ(parser.getDroppedFrameCount(), 1u);
	EXPECT_EQ(parser.getCrcErrorCount(), 0u);
}

TEST(SerialRequestParserTest, AnEntryCountTheDriverRefusesDropsTheFrame)
{
	serial::RequestParser parser;
	const std::vector<uint8_t> frame = withChecksum(bytes({0xAA, 0x55, 0x01, 0x00, 0x00, 0x00, 0x00, 0x20, 0x04}));

	EXPECT_FALSE(parse(frame, parser).has_value());
	EXPECT_EQ(parser.getDroppedFrameCount(), 1u);
}

TEST(SerialRequestParserTest, FindsAFrameAfterLeadingNoise)
{
	serial::RequestParser parser;
	std::vector<uint8_t> stream = bytes({0x00, 0xFF, 0x12, 0x34});
	const std::vector<uint8_t> frame = serial::buildReadRequest({{0x20000000, 4}});
	stream.insert(stream.end(), frame.begin(), frame.end());

	const std::optional<serial::Request> request = parse(stream, parser);

	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(request->entries[0].address, 0x20000000u);
}

TEST(SerialRequestParserTest, ASingleStartByteEndsOnTheFirstOne)
{
	/* The driver does not look back at a byte that broke a start pair, so two
	   start bytes in a row lose the second one. The host never sends that, and
	   the parser matches the target rather than repairing it. */
	serial::RequestParser parser;
	std::vector<uint8_t> stream = bytes({0xAA, 0xAA, 0x55});

	EXPECT_FALSE(parse(stream, parser).has_value());

	const std::vector<uint8_t> frame = serial::buildReadRequest({{0x20000000, 4}});
	std::vector<uint8_t> second = bytes({0xAA, 0xAA, 0x55});
	second.insert(second.end(), frame.begin(), frame.end());

	ASSERT_TRUE(parse(second, parser).has_value());
}

TEST(SerialRequestParserTest, TwoFramesInARowBothArrive)
{
	serial::RequestParser parser;
	std::vector<uint8_t> stream = serial::buildReadRequest({{0x20000000, 4}});
	const uint8_t value[1] = {9};
	const std::vector<uint8_t> second = serial::buildWriteRequest(0x20000000, value, 1);
	stream.insert(stream.end(), second.begin(), second.end());

	size_t seen = 0;

	for (uint8_t byte : stream)
	{
		if (parser.push(byte).has_value())
			seen++;
	}

	EXPECT_EQ(seen, 2u);
}

TEST(SerialResponseReaderTest, CollectsTheDataAndTheTrailingChecksum)
{
	std::vector<uint8_t> values = bytes({0x11, 0x22, 0x33, 0x44});
	const uint16_t checksum = serial::crc16(values);
	values.push_back(static_cast<uint8_t>(checksum & 0xFF));
	values.push_back(static_cast<uint8_t>((checksum >> 8) & 0xFF));

	serial::ResponseReader reader(4, true);
	bool complete = false;

	for (uint8_t byte : values)
		complete = reader.push(byte);

	EXPECT_TRUE(complete);
	EXPECT_TRUE(reader.isChecksumValid());
	ASSERT_EQ(reader.getData().size(), 4u);
	EXPECT_EQ(reader.getData()[0], 0x11);
	EXPECT_EQ(reader.getData()[3], 0x44);
}

TEST(SerialResponseReaderTest, NoticesAValueThatWasCorruptedOnTheWay)
{
	std::vector<uint8_t> values = bytes({0x11, 0x22, 0x33, 0x44});
	const uint16_t checksum = serial::crc16(values);
	values.push_back(static_cast<uint8_t>(checksum & 0xFF));
	values.push_back(static_cast<uint8_t>((checksum >> 8) & 0xFF));
	values[1] ^= 0x01;

	serial::ResponseReader reader(4, true);

	for (uint8_t byte : values)
		reader.push(byte);

	EXPECT_TRUE(reader.isComplete());
	EXPECT_FALSE(reader.isChecksumValid());
}

TEST(SerialResponseReaderTest, StopsAtTheExpectedLength)
{
	serial::ResponseReader reader(4, true);

	/* Four payload bytes, then the two checksum bytes. Only the last of them
	   completes the answer. */
	EXPECT_FALSE(reader.push(0));
	EXPECT_FALSE(reader.push(0));
	EXPECT_FALSE(reader.push(0));
	EXPECT_FALSE(reader.push(0));
	EXPECT_FALSE(reader.isComplete());
	EXPECT_FALSE(reader.push(0));
	EXPECT_TRUE(reader.push(0));
	EXPECT_TRUE(reader.isComplete());

	/* A byte that arrives after the answer is dropped rather than appended, so
	   the collected value keeps the length the request asked for. */
	EXPECT_TRUE(reader.push(0xAB));
	EXPECT_EQ(reader.getData().size(), 4u);
	EXPECT_EQ(reader.getReceivedSize(), 6u);
}

TEST(SerialResponseReaderTest, AReplyWithoutAChecksumEndsAtItsLength)
{
	serial::ResponseReader reader(3, false);
	EXPECT_FALSE(reader.push(0xAA));
	EXPECT_FALSE(reader.push(0xBB));
	EXPECT_TRUE(reader.push(0xCC));

	EXPECT_TRUE(reader.isChecksumValid());
	ASSERT_EQ(reader.getData().size(), 3u);
	EXPECT_EQ(reader.getData()[2], 0xCC);
}

TEST(SerialResponseReaderTest, ABufferReadAnswerCanBeALotOfBytes)
{
	serial::ResponseReader reader(255, false);

	for (int index = 0; index < 254; index++)
		EXPECT_FALSE(reader.push(static_cast<uint8_t>(index)));

	EXPECT_TRUE(reader.push(0xFE));
	EXPECT_EQ(reader.getData().size(), 255u);
}

TEST(SerialResponseSizeTest, AddsUpTheEntries)
{
	EXPECT_EQ(serial::readResponseSize({}), 0u);
	EXPECT_EQ(serial::readResponseSize({{0x20000000, 4}}), 4u);
	EXPECT_EQ(serial::readResponseSize({{0x20000000, 4}, {0x20000004, 1}, {0x20000008, 2}}), 7u);
}
