#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "RtosCpuLoad.hpp"

namespace
{
	RtosTask makeTask(uint32_t index, uint32_t counter, uint32_t kernelTaskNumber = 0)
	{
		RtosTask task;

		task.tcbAddress = 0x20010000u + index * 0x100u;
		task.hasRunTimeCounter = true;
		task.runTimeCounter = counter;
		task.hasKernelTaskNumber = kernelTaskNumber != 0;
		task.kernelTaskNumber = kernelTaskNumber;

		return task;
	}
}  // namespace

TEST(RtosCpuLoad, SharesAreThePartOfWhatTheTasksUsed)
{
	std::vector<RtosTask> tasks{makeTask(0, 100), makeTask(1, 300)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(tasks, summary, 0.0);

	/* Nothing to compare against yet: an absolute counter says nothing, so the
	   first round is spent remembering it, and no row gets a share. */
	EXPECT_EQ(summary.tasksMeasured, 0u);
	EXPECT_FALSE(summary.counting);
	EXPECT_FALSE(tasks[0].hasCpuShare);
	EXPECT_EQ(load.rememberedCount(), 2u);

	tasks[0].runTimeCounter = 200;
	tasks[1].runTimeCounter = 1000;

	load.step(tasks, summary, 0.5);

	/* 100 counts for the first task and 700 for the second, out of 800. */
	EXPECT_TRUE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 2u);
	EXPECT_DOUBLE_EQ(summary.windowSeconds, 0.5);

	ASSERT_TRUE(tasks[0].hasCpuShare);
	EXPECT_DOUBLE_EQ(tasks[0].cpuSharePercent, 12.5);

	ASSERT_TRUE(tasks[1].hasCpuShare);
	EXPECT_DOUBLE_EQ(tasks[1].cpuSharePercent, 87.5);
}

TEST(RtosCpuLoad, ACounterThatWrappedStillGivesTheDifference)
{
	std::vector<RtosTask> tasks{makeTask(0, 0xFFFFFF00u), makeTask(1, 0)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(tasks, summary, 0.0);

	tasks[0].runTimeCounter = 0x00000100u;
	tasks[1].runTimeCounter = 0x00000100u;

	load.step(tasks, summary, 0.5);

	/* The first task used 512 counts across the wrap, the second 256. */
	EXPECT_TRUE(summary.counting);

	ASSERT_TRUE(tasks[0].hasCpuShare);

	/* 512 out of 768: two thirds. An unsigned subtraction is what makes the
	   wrap come out right rather than as a negative difference. */
	EXPECT_NEAR(tasks[0].cpuSharePercent, 66.666, 0.001);
	EXPECT_NEAR(tasks[1].cpuSharePercent, 33.333, 0.001);
}

TEST(RtosCpuLoad, CountersThatDidNotMoveGiveNoShares)
{
	std::vector<RtosTask> tasks{makeTask(0, 500), makeTask(1, 900)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(tasks, summary, 0.0);
	load.step(tasks, summary, 0.5);

	/* The counters were read twice and nothing changed. A share of zero out of
	   zero is not a share, and a column of zeroes would read as a system with
	   nothing to do. */
	EXPECT_FALSE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 2u);
	EXPECT_FALSE(tasks[0].hasCpuShare);
	EXPECT_FALSE(tasks[1].hasCpuShare);
}

TEST(RtosCpuLoad, ATaskMissingFromARoundGoesWithoutAShareThatRound)
{
	std::vector<RtosTask> both{makeTask(0, 100), makeTask(1, 100)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(both, summary, 0.0);

	both[0].runTimeCounter = 200;
	both[1].runTimeCounter = 300;

	/* The second task was not in this round's table. Its counter is still
	   kept for next time, but a share needs the same two moments on both
	   sides and this round has only one. */
	std::vector<RtosTask> firstOnly{both[0]};

	load.step(firstOnly, summary, 0.5);

	EXPECT_TRUE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 1u);
	ASSERT_TRUE(firstOnly[0].hasCpuShare);
	EXPECT_DOUBLE_EQ(firstOnly[0].cpuSharePercent, 100.0);

	/* Back in the next round, which takes a reference for it rather than a
	   share: it needs both sides of the window to be its own, so coming back
	   costs it one round. */
	both[0].runTimeCounter = 300;
	both[1].runTimeCounter = 300;

	load.step(both, summary, 1.0);

	EXPECT_EQ(summary.tasksMeasured, 1u);
	EXPECT_TRUE(both[0].hasCpuShare);
	EXPECT_FALSE(both[1].hasCpuShare);

	/* And the round after that has both moments for both tasks again. */
	both[0].runTimeCounter = 400;
	both[1].runTimeCounter = 500;

	load.step(both, summary, 1.5);

	EXPECT_EQ(summary.tasksMeasured, 2u);
	ASSERT_TRUE(both[0].hasCpuShare);
	ASSERT_TRUE(both[1].hasCpuShare);
	EXPECT_NEAR(both[0].cpuSharePercent, 33.333, 0.001);
	EXPECT_NEAR(both[1].cpuSharePercent, 66.666, 0.001);
}

TEST(RtosCpuLoad, AControlBlockHandedOutAgainIsNotTheSameTask)
{
	std::vector<RtosTask> tasks{makeTask(0, 100, 11), makeTask(1, 100, 12)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(tasks, summary, 0.0);

	/* The kernel deleted the first task and gave its control block to
	   another, which starts its own counter. The address is the same, so the
	   numbers would look like a difference of a whole counter's worth of
	   time. */
	tasks[0].kernelTaskNumber = 13;
	tasks[0].runTimeCounter = 0;
	tasks[1].runTimeCounter = 200;

	load.step(tasks, summary, 0.5);

	EXPECT_EQ(summary.tasksMeasured, 1u);
	EXPECT_FALSE(tasks[0].hasCpuShare);
	ASSERT_TRUE(tasks[1].hasCpuShare);
	EXPECT_DOUBLE_EQ(tasks[1].cpuSharePercent, 100.0);
}

TEST(RtosCpuLoad, ATaskWithNoCounterIsLeftAlone)
{
	std::vector<RtosTask> tasks{makeTask(0, 100), makeTask(1, 100)};

	tasks[1].hasRunTimeCounter = false;

	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(tasks, summary, 0.0);

	EXPECT_EQ(load.rememberedCount(), 1u);

	tasks[0].runTimeCounter = 200;

	load.step(tasks, summary, 0.5);

	EXPECT_EQ(summary.tasksMeasured, 1u);
	ASSERT_TRUE(tasks[0].hasCpuShare);
	EXPECT_FALSE(tasks[1].hasCpuShare);
}

TEST(RtosCpuLoad, ForgettingDropsTheReferenceCounters)
{
	std::vector<RtosTask> tasks{makeTask(0, 100), makeTask(1, 100)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;

	load.step(tasks, summary, 0.0);

	/* A new session against a different target must not difference its
	   counters against the previous one's. */
	load.forget();

	EXPECT_EQ(load.rememberedCount(), 0u);

	tasks[0].runTimeCounter = 900;
	tasks[1].runTimeCounter = 900;

	load.step(tasks, summary, 0.5);

	EXPECT_FALSE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 0u);
}
