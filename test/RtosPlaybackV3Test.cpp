#include "RtosPlaybackV3.hpp"

#include <gtest/gtest.h>

#include <filesystem>

/*
 * A v3 recording read as rounds.
 *
 * The file is produced the way a conversion would produce it - v2 rounds,
 * re-encoded into a v3 block - because nothing writes v3 in production yet. That
 * keeps this test honest about what it proves: the reader works on bytes the
 * writer already produces, in a container the container tests already trust.
 */

namespace
{
	RtosSnapshot snapshotAt(double startedAt, const std::string& name)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = startedAt;
		snapshot.captureFinishedAt = startedAt + 0.4;
		snapshot.captureSpanSeconds = 0.35;
		snapshot.sessionId = "s";

		RtosTask task;
		task.tcbAddress = 0x20001000u;
		task.name = name;
		task.state = RtosTaskState::Ready;
		task.hasPriority = true;
		task.rawPriority = 2;
		task.priorityRank = 2;

		snapshot.tasks.push_back(task);

		return snapshot;
	}

	std::vector<RtosEventRing::Interval> oneRun(double from, double to)
	{
		RtosEventRing::Interval run;
		run.tcbAddress = 0x20001000u;
		run.fromSeconds = from;
		run.toSeconds = to;

		return {run};
	}

	/* Writes a v3 file holding `count` rounds, split over blocks of `perBlock`. */
	std::string writeV3(const std::filesystem::path& path, int count, int perBlock)
	{
		RtosRecordingV3 writer;
		RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

		EXPECT_TRUE(writer.create(path.string(), 1700000000, problem)) << RtosRecordingV3::describe(problem);

		std::vector<std::vector<uint8_t>> block;

		for (int index = 0; index < count; index++)
		{
			std::vector<uint8_t> encoded;
			RtosRecording::encodeRound(snapshotAt(10.0 + index, "t" + std::to_string(index)), oneRun(0.1, 0.2), encoded);
			block.push_back(encoded);

			if (static_cast<int>(block.size()) == perBlock || index + 1 == count)
			{
				EXPECT_TRUE(writer.append(RtosRecordingV3::BlockType::SnapshotRound, RtosRecordingV3::packRounds(block), 1, problem));
				block.clear();
			}
		}

		writer.close();

		return path.string();
	}
}  // namespace

TEST(RtosPlaybackV3Test, TheRoundsAreCountedInOrderAcrossBlocks)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "playback-v3-1.vtrec3";
	const std::string path = writeV3(file, 7, 3);  /* three blocks: 3 + 3 + 1 */

	RtosPlaybackV3 playback;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(playback.open(path, problem)) << RtosRecording::describe(problem);

	EXPECT_EQ(7u, playback.roundCount());
	EXPECT_TRUE(playback.isOpen());
	EXPECT_TRUE(playback.hasEventRecords());

	/* Times come from the first eight bytes of each round, so they are right
	   without decoding anything. */
	for (size_t index = 0; index < 7; index++)
		EXPECT_DOUBLE_EQ(10.0 + static_cast<double>(index), playback.atSeconds(index));

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}

TEST(RtosPlaybackV3Test, ARoundDecodesWithItsTasksAndItsSwitchRecords)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "playback-v3-2.vtrec3";
	const std::string path = writeV3(file, 3, 3);

	RtosPlaybackV3 playback;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(playback.open(path, problem)) << RtosRecording::describe(problem);

	const auto snapshot = playback.roundAt(1);
	ASSERT_NE(nullptr, snapshot);
	EXPECT_EQ(std::string("t1"), snapshot->tasks[0].name);
	EXPECT_DOUBLE_EQ(11.0, snapshot->captureStartedAt);

	const RtosPlaybackV3::Round round = playback.roundWithEvents(2);
	ASSERT_NE(nullptr, round.snapshot);
	EXPECT_EQ(std::string("t2"), round.snapshot->tasks[0].name);
	ASSERT_EQ(1u, round.events.size());
	EXPECT_DOUBLE_EQ(0.2, round.events[0].toSeconds);

	/* Out of range is nothing, not a crash and not the last round. */
	EXPECT_EQ(nullptr, playback.roundAt(99));
	EXPECT_EQ(nullptr, playback.roundWithEvents(99).snapshot);

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}

TEST(RtosPlaybackV3Test, AFileWithNoRoundBlockIsNotPlayable)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "playback-v3-empty.vtrec3";

	RtosRecordingV3 writer;
	RtosRecordingV3::Problem writeProblem = RtosRecordingV3::Problem::None;

	ASSERT_TRUE(writer.create(file.string(), 1700000000, writeProblem));
	ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::SourceDiagnostics, {1, 2, 3}, 1, writeProblem));
	writer.close();

	RtosPlaybackV3 playback;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	/* A container with only diagnostics in it is not something the interface can
	   show, and saying so is better than opening to an empty timeline. */
	EXPECT_FALSE(playback.open(file.string(), problem));
	EXPECT_EQ(RtosRecording::Problem::NoRounds, problem);
	EXPECT_FALSE(playback.isOpen());

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}

TEST(RtosPlaybackV3Test, AV2FileIsRefusedWithTheRightReason)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "playback-v2.vtrec";

	{
		RtosRecording writer;
		RtosRecording::Problem problem = RtosRecording::Problem::None;

		ASSERT_TRUE(writer.begin(file.string(), problem));
		writer.write(snapshotAt(1.0, "a"), {});
		writer.finish();
	}

	RtosPlaybackV3 playback;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	EXPECT_FALSE(playback.open(file.string(), problem));

	/* "Not a recording" rather than "cannot open": the caller uses this to pick
	   the other reader, and a wrong reason sends it to the wrong one. */
	EXPECT_EQ(RtosRecording::Problem::NotARecording, problem);

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}

TEST(RtosPlaybackV3Test, OtherBlocksInTheFileAreListedButDoNotBecomeRounds)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "playback-v3-mixed.vtrec3";

	{
		RtosRecordingV3 writer;
		RtosRecordingV3::Problem problem = RtosRecordingV3::Problem::None;

		ASSERT_TRUE(writer.create(file.string(), 1700000000, problem));
		ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::SessionManifest, {1}, 1, problem));

		std::vector<uint8_t> encoded;
		RtosRecording::encodeRound(snapshotAt(5.0, "only"), {}, encoded);
		ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::SnapshotRound, RtosRecordingV3::packRounds({encoded}), 1, problem));
		ASSERT_TRUE(writer.append(RtosRecordingV3::BlockType::RuleConfiguration, {2}, 1, problem));

		writer.close();
	}

	RtosPlaybackV3 playback;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(playback.open(file.string(), problem)) << RtosRecording::describe(problem);

	EXPECT_EQ(1u, playback.roundCount());
	EXPECT_EQ(3u, playback.blockTypes().size());

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}

/*
 * And through the class everything else already uses.
 *
 * The panel, the export and the tools all go through `RtosRecording`. If it did
 * not open v3 files, the reader next door would be a library nobody calls.
 */
TEST(RtosPlaybackV3Test, RtosRecordingOpensAV3FileAsIfItWereAnOrdinaryOne)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "playback-v3-through-v2.vtrec3";
	const std::string path = writeV3(file, 4, 2);

	RtosRecording recording;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(recording.open(path, problem)) << RtosRecording::describe(problem);

	EXPECT_TRUE(recording.isOpen());
	EXPECT_EQ(4u, recording.roundCount());
	EXPECT_TRUE(recording.hasEventRecords());

	/* The index the interface scrolls by comes from the first eight bytes of
	   each round, so it is right before anything is decoded. */
	EXPECT_DOUBLE_EQ(11.0, recording.atSeconds(1));
	EXPECT_EQ(2u, recording.nearestRound(12.0));

	const RtosRecording::Info info = recording.info();
	EXPECT_EQ(4u, info.rounds);
	EXPECT_DOUBLE_EQ(10.0, info.firstSecond);
	EXPECT_DOUBLE_EQ(13.0, info.lastSecond);

	const RtosRecording::Round round = recording.roundWithEvents(3);
	ASSERT_NE(nullptr, round.snapshot);
	EXPECT_EQ(std::string("t3"), round.snapshot->tasks[0].name);
	EXPECT_EQ(1u, round.events.size());

	recording.close();
	EXPECT_FALSE(recording.isOpen());

	std::error_code ignored;
	std::filesystem::remove(file, ignored);
}
