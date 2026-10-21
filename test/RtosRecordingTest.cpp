#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "RtosRecording.hpp"

namespace
{
	/* One directory for the whole file, emptied first: a test that leaves its
	   files behind makes the next run read the last one's work. */
	class RtosRecordingTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			directory = std::filesystem::temp_directory_path() / "variable-trace-recording-test";
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

		std::filesystem::path directory;
	};

	RtosSnapshot makeRound(double atSeconds, uint32_t taskCount)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = atSeconds;
		snapshot.captureFinishedAt = atSeconds + 0.05;
		snapshot.captureSpanSeconds = 0.05;
		snapshot.consistency = RtosConsistency::BestEffort;
		snapshot.listsWalked = 8;
		snapshot.listsFailed = 1;
		snapshot.queueRegistryAvailable = true;
		snapshot.sessionId = "session-1";

		snapshot.stackScan.enabled = true;
		snapshot.stackScan.fillSeen = true;
		snapshot.stackScan.tasksScanned = 2;
		snapshot.stackScan.oldestReadingAgeSeconds = 1.5;

		snapshot.cpuLoad.supported = true;
		snapshot.cpuLoad.counting = true;
		snapshot.cpuLoad.tasksEstimated = 1;
		snapshot.cpuLoad.tasksMeasured = taskCount;
		snapshot.cpuLoad.tasksKnown = taskCount;
		snapshot.cpuLoad.windowSeconds = 1.75;

		snapshot.problems.push_back("a list declared more items than it yielded");

		for (uint32_t index = 0; index < taskCount; index++)
		{
			RtosTask task;

			task.tcbAddress = 0x20001000u + index * 0x40u;
			task.name = "task" + std::to_string(index);
			task.state = index == 0 ? RtosTaskState::Running : RtosTaskState::Blocked;
			task.rawPriority = 3 - index;
			task.hasPriority = true;
			task.priorityRank = 3 - index;
			task.basePriority = 1;
			task.hasBasePriority = true;
			task.stackBase = 0x20002000u;
			task.stackTop = task.stackBase + 512;
			task.hasStack = true;
			task.hasStackWatermark = true;
			task.stackFreeBytes = 200 + index;
			task.stackWatermarkStackBytes = 512;
			task.stackWatermarkAgeSeconds = 0.5 * index;
			task.hasCpuShare = true;
			task.cpuSharePercent = 12.5 * index;
			task.waitingOn = index == 0 ? "" : "sem_task";
			task.sourceList = index == 0 ? "ready" : "suspended";
			task.eventList = index == 0 ? 0 : 0x20003000u;
			task.hasEventList = true;

			snapshot.tasks.push_back(task);
		}

		RtosSyncObject object;
		object.address = 0x20004000;
		object.name = "mtx";
		object.kind = RtosSyncKind::Mutex;
		object.capacity = 1;
		object.items = 0;
		object.hasCounts = true;
		object.hasWaiters = true;
		object.waitingToReceive = 2;
		object.holder = 0x20001000;
		object.hasHolder = true;

		snapshot.syncObjects.push_back(object);

		return snapshot;
	}

	void writeRounds(const std::string& file, const std::vector<RtosSnapshot>& rounds)
	{
		RtosRecording recording;
		RtosRecording::Problem problem = RtosRecording::Problem::None;

		ASSERT_TRUE(recording.begin(file, problem)) << RtosRecording::describe(problem);

		for (const RtosSnapshot& round : rounds)
			recording.write(round);

		recording.finish();
	}
}  // namespace

TEST_F(RtosRecordingTest, EveryFieldTheViewDrawsSurvivesTheFile)
{
	const std::string file = path("round.vtrec");

	RtosRecording writer;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(writer.begin(file, problem)) << RtosRecording::describe(problem);
	writer.write(makeRound(1.25, 3));
	writer.finish();

	RtosRecording reader;

	ASSERT_TRUE(reader.open(file, problem)) << RtosRecording::describe(problem);
	ASSERT_EQ(reader.roundCount(), 1u);

	const std::shared_ptr<RtosSnapshot> round = reader.roundAt(0);

	ASSERT_NE(round, nullptr);

	EXPECT_DOUBLE_EQ(round->captureStartedAt, 1.25);
	EXPECT_DOUBLE_EQ(round->captureSpanSeconds, 0.05);
	EXPECT_EQ(round->listsWalked, 8u);
	EXPECT_EQ(round->listsFailed, 1u);
	EXPECT_TRUE(round->queueRegistryAvailable);
	EXPECT_TRUE(round->stackScan.fillSeen);
	EXPECT_EQ(round->stackScan.tasksScanned, 2u);
	EXPECT_DOUBLE_EQ(round->stackScan.oldestReadingAgeSeconds, 1.5);
	EXPECT_TRUE(round->cpuLoad.counting);
	EXPECT_EQ(round->cpuLoad.tasksMeasured, 3u);
	EXPECT_DOUBLE_EQ(round->cpuLoad.windowSeconds, 1.75);
	EXPECT_EQ(round->sessionId, "session-1");
	ASSERT_EQ(round->problems.size(), 1u);
	EXPECT_EQ(round->problems.front(), "a list declared more items than it yielded");

	ASSERT_EQ(round->tasks.size(), 3u);
	EXPECT_EQ(round->tasks[1].name, "task1");
	EXPECT_EQ(round->tasks[1].state, RtosTaskState::Blocked);
	EXPECT_EQ(round->tasks[1].rawPriority, 2u);
	EXPECT_EQ(round->tasks[1].hasPriority, true);
	EXPECT_EQ(round->tasks[1].basePriority, 1u);
	EXPECT_EQ(round->tasks[1].stackFreeBytes, 201u);
	EXPECT_DOUBLE_EQ(round->tasks[1].stackWatermarkAgeSeconds, 0.5);
	EXPECT_DOUBLE_EQ(round->tasks[1].cpuSharePercent, 12.5);
	EXPECT_EQ(round->tasks[1].waitingOn, "sem_task");
	EXPECT_EQ(round->tasks[1].sourceList, "suspended");
	EXPECT_EQ(round->tasks[1].eventList, 0x20003000u);

	ASSERT_EQ(round->syncObjects.size(), 1u);
	EXPECT_EQ(round->syncObjects.front().name, "mtx");
	EXPECT_EQ(round->syncObjects.front().kind, RtosSyncKind::Mutex);
	EXPECT_EQ(round->syncObjects.front().holder, 0x20001000u);
	EXPECT_EQ(round->syncObjects.front().waitingToReceive, 2u);
}

TEST_F(RtosRecordingTest, TheIndexKnowsWhenEachRoundWasTaken)
{
	const std::string file = path("three.vtrec");

	writeRounds(file, {makeRound(1.0, 2), makeRound(1.5, 2), makeRound(2.0, 2)});

	RtosRecording reader;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(reader.open(file, problem)) << RtosRecording::describe(problem);
	ASSERT_EQ(reader.roundCount(), 3u);

	EXPECT_DOUBLE_EQ(reader.atSeconds(0), 1.0);
	EXPECT_DOUBLE_EQ(reader.atSeconds(2), 2.0);

	const RtosRecording::Info info = reader.info();

	EXPECT_EQ(info.rounds, 3u);
	EXPECT_DOUBLE_EQ(info.firstSecond, 1.0);
	EXPECT_DOUBLE_EQ(info.lastSecond, 2.0);
	EXPECT_DOUBLE_EQ(info.durationSeconds(), 1.0);
}

TEST_F(RtosRecordingTest, NearestRoundIsTheCloserOfTheTwo)
{
	const std::string file = path("nearest.vtrec");

	writeRounds(file, {makeRound(1.0, 2), makeRound(1.5, 2), makeRound(2.0, 2)});

	RtosRecording reader;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(reader.open(file, problem)) << RtosRecording::describe(problem);

	EXPECT_EQ(reader.nearestRound(1.24), 0u);
	EXPECT_EQ(reader.nearestRound(1.26), 1u);

	/* Outside the recording, a click lands on the end nearest to it rather than
	   on nothing: the strip can be dragged past either end. */
	EXPECT_EQ(reader.nearestRound(0.1), 0u);
	EXPECT_EQ(reader.nearestRound(9.0), 2u);
}

TEST_F(RtosRecordingTest, ARecordingWithNoRoundsIsRefused)
{
	const std::string file = path("empty.vtrec");

	RtosRecording writer;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(writer.begin(file, problem)) << RtosRecording::describe(problem);
	writer.finish();

	RtosRecording reader;

	EXPECT_FALSE(reader.open(file, problem));
	EXPECT_NE(problem, RtosRecording::Problem::None);
	EXPECT_EQ(reader.roundCount(), 0u);
}

TEST_F(RtosRecordingTest, AFileThatIsNotARecordingIsRefused)
{
	const std::string file = path("other.bin");

	std::ofstream out(file, std::ios::binary);
	out << "name,value\n1,2\n";
	out.close();

	RtosRecording reader;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	EXPECT_FALSE(reader.open(file, problem));
	EXPECT_NE(problem, RtosRecording::Problem::None);
}

TEST_F(RtosRecordingTest, ATruncatedTailKeepsTheRoundsThatAreWhole)
{
	const std::string file = path("cut.vtrec");

	writeRounds(file, {makeRound(1.0, 3), makeRound(2.0, 3), makeRound(3.0, 3)});

	/* A machine switched off mid-write leaves the last record short. The rounds
	   before it are still a reading of the target; the last one is not. */
	const auto whole = std::filesystem::file_size(file);
	std::filesystem::resize_file(file, whole - 40);

	RtosRecording reader;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(reader.open(file, problem)) << RtosRecording::describe(problem);
	EXPECT_EQ(reader.roundCount(), 2u);
	EXPECT_NE(reader.roundAt(0), nullptr);
	EXPECT_NE(reader.roundAt(1), nullptr);
	EXPECT_EQ(reader.roundAt(2), nullptr);
}

TEST_F(RtosRecordingTest, OneRoleAtATime)
{
	const std::string first = path("one.vtrec");
	const std::string second = path("two.vtrec");

	writeRounds(second, {makeRound(1.0, 1)});

	RtosRecording writer;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(writer.begin(first, problem)) << RtosRecording::describe(problem);

	/* Opening while writing would leave two readers on one handle. */
	EXPECT_FALSE(writer.open(second, problem));

	writer.finish();

	RtosRecording reader;

	ASSERT_TRUE(reader.open(second, problem)) << RtosRecording::describe(problem);

	EXPECT_FALSE(reader.begin(first, problem));
}

TEST_F(RtosRecordingTest, TheBytesOnDiskStayWithTheRound)
{
	const std::string file = path("size.vtrec");

	writeRounds(file, {makeRound(1.0, 5), makeRound(2.0, 5)});

	/* A round of five tasks with three names each is about a kilobyte; what
	   matters is that the count is reported and that it grows with the rounds
	   rather than with the number of reads. */
	RtosRecording reader;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(reader.open(file, problem)) << RtosRecording::describe(problem);

	const RtosRecording::Info info = reader.info();

	EXPECT_GT(info.bytes, 500u);
	EXPECT_LT(info.bytes, 8000u);
}
