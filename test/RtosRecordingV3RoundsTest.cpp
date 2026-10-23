#include "RtosRecording.hpp"
#include "RtosRecordingV3.hpp"

#include <gtest/gtest.h>

#include <filesystem>

/*
 * Rounds in the v3 container.
 *
 * The point is not that the container works - that is tested next door - but
 * that a round written by the old path and a round read back out of a v3 block
 * are the *same round*. One encoder, one decoder, two containers.
 */

namespace
{
	RtosSnapshot makeSnapshot(double startedAt, const std::string& taskName)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = startedAt;
		snapshot.captureFinishedAt = startedAt + 0.5;
		snapshot.captureSpanSeconds = 0.4;
		snapshot.consistency = RtosConsistency::Consistent;
		snapshot.listsWalked = 11;
		snapshot.listsFailed = 0;
		snapshot.sessionId = "session-1";

		RtosTask task;
		task.tcbAddress = 0x20001000u;
		task.name = taskName;
		task.state = RtosTaskState::Ready;
		task.hasPriority = true;
		task.rawPriority = 3;
		task.priorityRank = 3;
		task.hasStackWatermark = true;
		task.stackFreeBytes = 512;
		task.stackWatermarkStackBytes = 1024;

		snapshot.tasks.push_back(task);

		RtosSyncObject object;
		object.address = 0x20002000u;
		object.name = "mtx_a";
		object.kind = RtosSyncKind::Mutex;
		object.capacity = 1;

		snapshot.syncObjects.push_back(object);

		return snapshot;
	}

	std::vector<RtosEventRing::Interval> makeRuns()
	{
		RtosEventRing::Interval run;
		run.tcbAddress = 0x20001000u;
		run.fromSeconds = 0.1;
		run.toSeconds = 0.2;

		return {run};
	}
}  // namespace

TEST(RtosRecordingV3RoundsTest, ARoundSurvivesTheContainer)
{
	const RtosSnapshot snapshot = makeSnapshot(12.5, "led");
	const std::vector<RtosEventRing::Interval> runs = makeRuns();

	std::vector<uint8_t> encoded;
	RtosRecording::encodeRound(snapshot, runs, encoded);
	ASSERT_FALSE(encoded.empty());

	const std::filesystem::path file = std::filesystem::temp_directory_path() / "variable-trace-v3-rounds.vtrec3";

	{
		RtosRecordingV3 writer;
		RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

		ASSERT_TRUE(writer.create(file.string(), 1700000000, problem)) << RtosRecordingV3::describe(problem);
		ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::SnapshotRound, RtosRecordingV3::packRounds({encoded}), 1, problem))
		    << RtosRecordingV3::describe(problem);

		writer.close();
	}

	RtosRecordingV3 reader;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(reader.open(file.string(), contents, problem)) << RtosRecordingV3::describe(problem);
	ASSERT_EQ(1u, contents.blocks.size());
	EXPECT_EQ(RtosRecordingV3::BlockType::SnapshotRound, contents.blocks[0].type);

	std::vector<std::vector<uint8_t>> rounds;
	ASSERT_TRUE(RtosRecordingV3::unpackRounds(contents.blocks[0].payload, rounds));
	ASSERT_EQ(1u, rounds.size());

	RtosRecording::Round decoded;
	ASSERT_TRUE(RtosRecording::decodeRound(rounds[0].data(), rounds[0].size(), true, decoded));

	ASSERT_NE(nullptr, decoded.snapshot);
	EXPECT_DOUBLE_EQ(12.5, decoded.snapshot->captureStartedAt);
	EXPECT_EQ(std::string("session-1"), decoded.snapshot->sessionId);
	ASSERT_EQ(1u, decoded.snapshot->tasks.size());
	EXPECT_EQ(std::string("led"), decoded.snapshot->tasks[0].name);
	EXPECT_EQ(0x20001000u, decoded.snapshot->tasks[0].tcbAddress);
	EXPECT_EQ(512u, decoded.snapshot->tasks[0].stackFreeBytes);
	EXPECT_TRUE(decoded.snapshot->tasks[0].hasStackWatermark);

	/* The objects came back too: a round that lost its objects would draw a
	   task waiting for something that is not there. */
	ASSERT_EQ(1u, decoded.snapshot->syncObjects.size());
	EXPECT_EQ(std::string("mtx_a"), decoded.snapshot->syncObjects[0].name);

	/* And the switch records that travelled with the round. */
	ASSERT_EQ(1u, decoded.events.size());
	EXPECT_DOUBLE_EQ(0.2, decoded.events[0].toSeconds);

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}

TEST(RtosRecordingV3RoundsTest, SeveralRoundsFitInOneBlockAndKeepTheirOrder)
{
	std::vector<std::vector<uint8_t>> encoded;

	for (int index = 0; index < 5; index++)
	{
		std::vector<uint8_t> round;
		RtosRecording::encodeRound(makeSnapshot(100.0 + index, "task" + std::to_string(index)), {}, round);
		encoded.push_back(round);
	}

	const std::vector<uint8_t> payload = RtosRecordingV3::packRounds(encoded);

	std::vector<std::vector<uint8_t>> unpacked;
	ASSERT_TRUE(RtosRecordingV3::unpackRounds(payload, unpacked));
	ASSERT_EQ(5u, unpacked.size());

	for (int index = 0; index < 5; index++)
	{
		RtosRecording::Round decoded;
		ASSERT_TRUE(RtosRecording::decodeRound(unpacked[static_cast<size_t>(index)].data(),
		                                      unpacked[static_cast<size_t>(index)].size(), true, decoded));

		EXPECT_EQ(std::string("task" + std::to_string(index)), decoded.snapshot->tasks[0].name);
	}
}

TEST(RtosRecordingV3RoundsTest, ABlockThatPromisesMoreThanItHoldsIsRefused)
{
	std::vector<uint8_t> payload;

	const uint8_t count[4] = {2, 0, 0, 0};   /* two rounds claimed */
	const uint8_t length[4] = {64, 0, 0, 0}; /* the first claims 64 bytes */
	payload.insert(payload.end(), count, count + 4);
	payload.insert(payload.end(), length, length + 4);
	payload.push_back(0xAB);                 /* and one byte follows */

	std::vector<std::vector<uint8_t>> rounds;

	/* Refused rather than returning one truncated round: half a snapshot decoded
	   as a whole one is worse than no snapshot. */
	EXPECT_FALSE(RtosRecordingV3::unpackRounds(payload, rounds));
}

TEST(RtosRecordingV3RoundsTest, AnEmptyBlockIsZeroRounds)
{
	std::vector<std::vector<uint8_t>> rounds;

	ASSERT_TRUE(RtosRecordingV3::unpackRounds(RtosRecordingV3::packRounds({}), rounds));
	EXPECT_TRUE(rounds.empty());
}

TEST(RtosRecordingV3RoundsTest, AV2FileConvertsToV3RoundForRound)
{
	const std::filesystem::path v2Path = std::filesystem::temp_directory_path() / "variable-trace-convert.vtrec";
	const std::filesystem::path v3Path = std::filesystem::temp_directory_path() / "variable-trace-convert.vtrec3";

	/* Three rounds in the old container. */
	{
		RtosRecording writer;
		RtosRecording::Problem problem = RtosRecording::Problem::None;

		ASSERT_TRUE(writer.begin(v2Path.string(), problem)) << RtosRecording::describe(problem);

		for (int index = 0; index < 3; index++)
			writer.write(makeSnapshot(200.0 + index, "task" + std::to_string(index)), makeRuns());

		writer.finish();
	}

	/* Read them, re-encode, put them in the new container: this is what a
	   conversion would do, and it is a test rather than a tool because nothing
	   writes v3 yet. */
	{
		RtosRecording reader;
		RtosRecording::Problem problem = RtosRecording::Problem::None;

		ASSERT_TRUE(reader.open(v2Path.string(), problem)) << RtosRecording::describe(problem);
		ASSERT_EQ(3u, reader.roundCount());

		std::vector<std::vector<uint8_t>> encoded;

		for (size_t round = 0; round < reader.roundCount(); round++)
		{
			const RtosRecording::Round decoded = reader.roundWithEvents(round);
			ASSERT_NE(nullptr, decoded.snapshot);

			std::vector<uint8_t> bytes;
			RtosRecording::encodeRound(*decoded.snapshot, decoded.events, bytes);
			encoded.push_back(bytes);
		}

		RtosRecordingV3 writer;
		RtosRecordingV3::Problem v3Problem = RtosRecordingV3::Problem::None;

		ASSERT_TRUE(writer.create(v3Path.string(), 1700000000, v3Problem)) << RtosRecordingV3::describe(v3Problem);
		ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::SnapshotRound, RtosRecordingV3::packRounds(encoded), 1, v3Problem));
		writer.close();

		reader.close();
	}

	/* And the new file decodes to the same three rounds, in the same order. */
	RtosRecordingV3 container;
	RtosRecordingV3::Contents contents;
	RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(container.open(v3Path.string(), contents, problem)) << RtosRecordingV3::describe(problem);
	ASSERT_EQ(1u, contents.blocks.size());

	std::vector<std::vector<uint8_t>> rounds;
	ASSERT_TRUE(RtosRecordingV3::unpackRounds(contents.blocks[0].payload, rounds));
	ASSERT_EQ(3u, rounds.size());

	for (size_t index = 0; index < rounds.size(); index++)
	{
		RtosRecording::Round decoded;
		ASSERT_TRUE(RtosRecording::decodeRound(rounds[index].data(), rounds[index].size(), true, decoded));

		EXPECT_DOUBLE_EQ(200.0 + static_cast<double>(index), decoded.snapshot->captureStartedAt);
		EXPECT_EQ(std::string("task" + std::to_string(index)), decoded.snapshot->tasks[0].name);
		EXPECT_EQ(1u, decoded.events.size());
	}

	std::error_code ignored;
	std::filesystem::remove(v2Path, ignored);
	std::filesystem::remove(v3Path, ignored);
}
