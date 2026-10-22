#include "Rtec2Decoder.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace
{
	using json = nlohmann::json;

	std::string goldenPath(const std::string& name, const char* suffix)
	{
		return std::string(RTEC2_GOLDEN_DIR) + "/" + name + suffix;
	}

	std::vector<uint8_t> readBinary(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		return bytes;
	}

	json readJson(const std::string& path)
	{
		std::ifstream file(path);
		json parsed;
		file >> parsed;

		return parsed;
	}

	/*
	 * The same shape the C generator writes, built from what the decoder made
	 * of the bytes. Comparing structures rather than bytes is the point: the
	 * three implementations are allowed to store things differently, and are
	 * not allowed to disagree about what the stream says.
	 */
	json describe(const std::string& name, const Rtec2DecodeResult& result)
	{
		json out;
		out["file"] = name;
		out["records"] = json::array();
		out["unknownTypes"] = result.unknownTypes;
		out["truncatedBytes"] = result.truncatedBytes;

		for (size_t index = 0; index < result.records.size(); index++)
		{
			const Rtec2DecodedRecord& record = result.records[index];

			json entry;
			entry["index"] = index;
			entry["type"] = static_cast<unsigned>(record.event.type);
			entry["typeName"] = toString(record.event.type);
			entry["sequence"] = record.event.sequence;
			entry["subject"] = record.event.subject.rawHandle;
			entry["object"] = record.event.object.rawHandle;
			entry["arg0"] = record.event.arg0;
			entry["arg1"] = record.event.arg1;
			entry["hasTargetTicks"] = record.event.hasTargetTicks ? 1 : 0;
			entry["targetTicks"] = record.event.targetTicks;
			entry["payloadBytes"] = static_cast<unsigned>(record.payloadBytes);

			out["records"].push_back(entry);
		}

		return out;
	}

	void expectGolden(const std::string& name)
	{
		const std::vector<uint8_t> bytes = readBinary(goldenPath(name, ".bin"));
		ASSERT_FALSE(bytes.empty()) << "missing golden vector " << name;

		const Rtec2DecodeResult result = Rtec2Decoder::decode(bytes);
		const json actual = describe(name, result);
		const json expected = readJson(goldenPath(name, ".expected.json"));

		EXPECT_EQ(expected, actual) << "the C++ decoder and the C encoder disagree about " << name;
	}
}  // namespace

TEST(Rtec2DecoderTest, EveryTypeInOneFile)
{
	expectGolden("one-of-each");
}

TEST(Rtec2DecoderTest, ASessionWithAGap)
{
	expectGolden("session");
}

TEST(Rtec2DecoderTest, TheTickCounterGoingOverItsEnd)
{
	expectGolden("wrap");
}

TEST(Rtec2DecoderTest, AnUnknownTypeIsSkippedAndCounted)
{
	expectGolden("unknown-type");

	const Rtec2DecodeResult result = Rtec2Decoder::decode(readBinary(goldenPath("unknown-type", ".bin")));

	/* Skipping is not an error, and it is not silence either: the count is what
	   stops a newer target's stream from looking like a shorter one. */
	EXPECT_EQ(1u, result.unknownTypes);
	EXPECT_EQ(2u, result.records.size());
}

TEST(Rtec2DecoderTest, ARecordCutInHalfIsReportedNotGuessed)
{
	expectGolden("truncated");

	const Rtec2DecodeResult result = Rtec2Decoder::decode(readBinary(goldenPath("truncated", ".bin")));

	EXPECT_EQ(2u, result.records.size());
	EXPECT_EQ(10u, result.truncatedBytes);
}

TEST(Rtec2DecoderTest, TheNamespaceOfAHandleComesFromTheType)
{
	/* A wait is about a task and an object; ... */
	EXPECT_EQ(RtosEntityKind::Task, Rtec2Decoder::subjectKind(RtosTraceEventType::WaitBegin));
	EXPECT_EQ(RtosEntityKind::Sync, Rtec2Decoder::objectKind(RtosTraceEventType::WaitBegin));

	/* ... an interrupt is about neither: its number is not a handle. */
	EXPECT_EQ(RtosEntityKind::None, Rtec2Decoder::subjectKind(RtosTraceEventType::IsrEnter));
	EXPECT_EQ(RtosEntityKind::None, Rtec2Decoder::objectKind(RtosTraceEventType::IsrEnter));

	/* A gap is about the session. */
	EXPECT_EQ(RtosEntityKind::None, Rtec2Decoder::subjectKind(RtosTraceEventType::Gap));
}

TEST(Rtec2DecoderTest, ReservedTypeValuesStayUnknown)
{
	EXPECT_EQ(RtosTraceEventType::Unknown, Rtec2Decoder::typeOf(0x00));
	EXPECT_EQ(RtosTraceEventType::Unknown, Rtec2Decoder::typeOf(0x0F));
	EXPECT_EQ(RtosTraceEventType::Unknown, Rtec2Decoder::typeOf(0x7F));
	EXPECT_EQ(RtosTraceEventType::TaskCreate, Rtec2Decoder::typeOf(0x10));
}

TEST(Rtec2DecoderTest, NoiseNeitherCrashesNorInventsRecords)
{
	/* A decoder that walks a length field on hostile input is the classic way
	   to read past a buffer. The rule is that anything that does not describe a
	   whole record stops the walk and is reported, never allocated. */
	uint32_t state = 12345;

	std::vector<uint8_t> noise(512);

	for (uint8_t& byte : noise)
	{
		state = state * 1103515245u + 12345u;
		byte = static_cast<uint8_t>(state >> 16);
	}

	const Rtec2DecodeResult result = Rtec2Decoder::decode(noise);

	/* Whatever it makes of it, the bytes it did not use are accounted for. */
	EXPECT_LE(result.records.size(), noise.size() / 4);
	EXPECT_LE(result.truncatedBytes, noise.size());
}

TEST(Rtec2DecoderTest, AnEmptyFileIsAnEmptyDecode)
{
	const Rtec2DecodeResult result = Rtec2Decoder::decode(nullptr, 0);

	EXPECT_TRUE(result.records.empty());
	EXPECT_EQ(0u, result.unknownTypes);
	EXPECT_EQ(0u, result.truncatedBytes);
}
