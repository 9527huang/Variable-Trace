#include "RtosRecordingV3.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	class RtosRecordingV3Test : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			directory = std::filesystem::temp_directory_path() / "variable-trace-v3-test";
			std::filesystem::remove_all(directory);
			std::filesystem::create_directories(directory);
		}

		void TearDown() override
		{
			std::filesystem::remove_all(directory);
		}

		std::string path(const std::string& name) const
		{
			return (directory / name).string();
		}

		/* Writes one file with `count` blocks and returns its path. */
		std::string writeThree(const std::string& name, uint32_t count)
		{
			const std::string file = path(name);

			RtosRecordingV3 writer;
			RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

			EXPECT_TRUE(writer.create(file, 1700000000, problem)) << RtosRecordingV3::describe(problem);

			for (uint32_t index = 1; index <= count; index++)
			{
				const std::vector<uint8_t> payload = {'p', 'a', 'y', '0' + static_cast<uint8_t>(index % 10)};

				EXPECT_TRUE(writer.append(RtosRecordingV3::BlockType::NormalizedEvents, payload, 1, problem))
				    << RtosRecordingV3::describe(problem);
			}

			writer.close();

			return file;
		}

		/* Flips one byte of the file, for the corruption cases. */
		void poke(const std::string& file, uint64_t at, uint8_t value)
		{
			std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
			ASSERT_TRUE(stream.is_open());

			stream.seekp(static_cast<std::streamoff>(at), std::ios::beg);
			stream.put(static_cast<char>(value));
			stream.close();
		}

		std::filesystem::path directory;
	};
}  // namespace

TEST_F(RtosRecordingV3Test, TheCrcIsTheStandardOne)
{
	/* CRC-32/MPEG-2 check value. Hard coded on purpose: a CRC that only agrees
	   with itself would let a wrong polynomial pass every round trip test. */
	const char* check = "123456789";

	EXPECT_EQ(0x0376E6E7u, RtosRecordingV3::crc32(reinterpret_cast<const uint8_t*>(check), 9));
	/* No bytes: the initial value, which is a fact worth pinning because a
	   reader that started from zero would accept blocks with a zero CRC. */
	EXPECT_EQ(0xFFFFFFFFu, RtosRecordingV3::crc32(nullptr, 0));
}

TEST_F(RtosRecordingV3Test, EveryBlockComesBackWithItsTypeAndPayload)
{
	const std::string file = writeThree("three.vtrec3", 3);

	EXPECT_TRUE(RtosRecordingV3::looksLikeV3(file));

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(reader.open(file, contents, problem)) << RtosRecordingV3::describe(problem);

	ASSERT_EQ(3u, contents.blocks.size());
	EXPECT_FALSE(contents.stoppedEarly);

	for (uint32_t index = 0; index < 3; index++)
	{
		EXPECT_EQ(RtosRecordingV3::BlockType::NormalizedEvents, contents.blocks[index].type);
		EXPECT_EQ(index + 1, contents.blocks[index].sequence);
		ASSERT_EQ(4u, contents.blocks[index].payload.size());
		EXPECT_EQ('0' + static_cast<uint8_t>((index + 1) % 10), contents.blocks[index].payload[3]);
	}
}

TEST_F(RtosRecordingV3Test, ABlockThatDoesNotMatchItsCrcStopsTheReadThere)
{
	const std::string file = writeThree("crc.vtrec3", 3);

	/* Block 2's payload starts at 32 + 24 for block 1 (4 bytes) + 24 for the
	   header of block 2. Corrupting one byte of it is what a copy that went
	   wrong looks like. */
	poke(file, 32 + 24 + 4 + 24 + 1, 0xFF);

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(reader.open(file, contents, problem)) << RtosRecordingV3::describe(problem);

	EXPECT_EQ(1u, contents.blocks.size());
	EXPECT_TRUE(contents.stoppedEarly);
	EXPECT_EQ(std::string("crc"), contents.stoppedBecause);
}

TEST_F(RtosRecordingV3Test, ABlockThatWasNeverCommittedIsTheTailOfACrash)
{
	const std::string file = writeThree("crash.vtrec3", 3);

	/* The commit word of block 3 is at 32 + 2×(24+4) + 20. Clearing it is what
	   the disk holds when the process died between the payload and the commit. */
	poke(file, 32 + 2 * (24 + 4) + 20, 0x00);
	poke(file, 32 + 2 * (24 + 4) + 21, 0x00);
	poke(file, 32 + 2 * (24 + 4) + 22, 0x00);
	poke(file, 32 + 2 * (24 + 4) + 23, 0x00);

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(reader.open(file, contents, problem)) << RtosRecordingV3::describe(problem);

	/* Two blocks survived, and the third is not "partially there": it never
	   happened. */
	EXPECT_EQ(2u, contents.blocks.size());
	EXPECT_TRUE(contents.stoppedEarly);
	EXPECT_EQ(std::string("uncommitted block"), contents.stoppedBecause);
}

TEST_F(RtosRecordingV3Test, AFileWhoseLastPayloadIsShortIsStoppedNotPatched)
{
	const std::string file = writeThree("short.vtrec3", 2);

	std::error_code error;
	const uintmax_t size = std::filesystem::file_size(file, error);
	ASSERT_FALSE(error);

	/* Cut the last two bytes: the block header now promises more than the file
	   holds, which is what a copy interrupted at the end looks like. */
	std::filesystem::resize_file(file, size - 2);

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(reader.open(file, contents, problem)) << RtosRecordingV3::describe(problem);

	EXPECT_EQ(1u, contents.blocks.size());
	EXPECT_TRUE(contents.stoppedEarly);
	EXPECT_EQ(std::string("payload past the end of the file"), contents.stoppedBecause);
}

TEST_F(RtosRecordingV3Test, AnOlderRecordingIsNamedAsSuchRatherThanParsed)
{
	/* The first eight bytes of a v1/v2 file: the old magic, then a version. A v3
	   reader that tried anyway would report a corrupt block instead of "this is
	   the old format, use the old reader". */
	const std::string file = path("old.vtrec");

	{
		std::ofstream out(file, std::ios::binary);
		const char magic[8] = {'V', 'T', 'R', 'E', 'C', 'O', 'R', 'D'};
		const uint32_t version = 2;

		out.write(magic, sizeof(magic));
		out.write(reinterpret_cast<const char*>(&version), sizeof(version));
	}

	EXPECT_FALSE(RtosRecordingV3::looksLikeV3(file));

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	EXPECT_FALSE(reader.open(file, contents, problem));
	EXPECT_EQ(RtosRecordingV3::Problem::NotAV3File, problem);
}

TEST_F(RtosRecordingV3Test, AFileWithNoCommittedBlockIsNotARecording)
{
	const std::string file = path("header-only.vtrec3");

	RtosRecordingV3 writer;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(writer.create(file, 1700000000, problem));
	writer.close();

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;

	EXPECT_FALSE(reader.open(file, contents, problem));
	EXPECT_EQ(RtosRecordingV3::Problem::NoBlocks, problem);
}

TEST_F(RtosRecordingV3Test, WritingWithoutCreatingSaysSo)
{
	RtosRecordingV3 writer;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	const std::vector<uint8_t> payload = {1, 2, 3};

	EXPECT_FALSE(writer.append(RtosRecordingV3::BlockType::RawTrace, payload, 1, problem));
	EXPECT_EQ(RtosRecordingV3::Problem::NotWritable, problem);
}

TEST_F(RtosRecordingV3Test, EveryBlockTypeCanBeWrittenAndReadBack)
{
	const std::string file = path("types.vtrec3");

	RtosRecordingV3 writer;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(writer.create(file, 1700000000, problem));

	const RtosRecordingV3::BlockType types[] = {
	    RtosRecordingV3::BlockType::SessionManifest,     RtosRecordingV3::BlockType::RawTrace,
	    RtosRecordingV3::BlockType::NormalizedEvents,    RtosRecordingV3::BlockType::SnapshotRound,
	    RtosRecordingV3::BlockType::IdentityDictionary,  RtosRecordingV3::BlockType::SourceDiagnostics,
	    RtosRecordingV3::BlockType::RuleConfiguration,   RtosRecordingV3::BlockType::IndexCheckpoint,
	};

	for (const RtosRecordingV3::BlockType type : types)
	{
		const std::vector<uint8_t> payload(3, static_cast<uint8_t>(type));

		ASSERT_TRUE(writer.append(type, payload, 1, problem)) << RtosRecordingV3::describe(problem);
	}

	writer.close();

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;

	ASSERT_TRUE(reader.open(file, contents, problem)) << RtosRecordingV3::describe(problem);
	ASSERT_EQ(8u, contents.blocks.size());

	for (size_t index = 0; index < 8; index++)
		EXPECT_EQ(types[index], contents.blocks[index].type);
}

TEST_F(RtosRecordingV3Test, ACheckpointIsWrittenEverySixtyFourDataBlocks)
{
	const std::string file = path("checkpoint.vtrec3");

	RtosRecordingV3 writer;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(writer.create(file, 1700000000, problem));

	const std::vector<uint8_t> payload = {1, 2, 3, 4};

	for (int index = 0; index < 70; index++)
		ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::RawTrace, payload, 1, problem));

	/* 70 data blocks and one checkpoint, written right after the sixty fourth. */
	EXPECT_EQ(71u, writer.blocksWritten());
	EXPECT_EQ(70u, writer.index().size());

	writer.close();

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;

	ASSERT_TRUE(reader.open(file, contents, problem)) << RtosRecordingV3::describe(problem);
	ASSERT_EQ(71u, contents.blocks.size());

	ASSERT_EQ(RtosRecordingV3::BlockType::IndexCheckpoint, contents.blocks[64].type);

	/* Sixty four entries of twenty four bytes: every data block before it. */
	EXPECT_EQ(64u * 24u, contents.blocks[64].payload.size());

	/* And the entries point at the payloads that are actually there. */
	const auto firstOffset = static_cast<uint64_t>(contents.blocks[64].payload[4]) |
	                         (static_cast<uint64_t>(contents.blocks[64].payload[5]) << 8) |
	                         (static_cast<uint64_t>(contents.blocks[64].payload[6]) << 16) |
	                         (static_cast<uint64_t>(contents.blocks[64].payload[7]) << 24);

	EXPECT_EQ(contents.blocks[0].payloadOffset, firstOffset);
}
