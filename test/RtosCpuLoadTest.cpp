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

	RtosCpuLoad::Settings settingsOf(uint32_t windowRounds = 4)
	{
		RtosCpuLoad::Settings settings;
		settings.windowRounds = windowRounds;

		return settings;
	}
}  // namespace

TEST(RtosCpuLoad, SharesAreThePartOfWhatTheTasksUsed)
{
	std::vector<RtosTask> tasks{makeTask(0, 100), makeTask(1, 300)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf();

	load.step(tasks, summary, settings, 0.0);

	/* One round is a set of absolute counters, and an absolute counter says
	   nothing, so the first round is spent remembering it. */
	EXPECT_EQ(summary.tasksMeasured, 0u);
	EXPECT_FALSE(summary.counting);
	EXPECT_FALSE(tasks[0].hasCpuShare);
	EXPECT_EQ(load.rememberedRounds(), 1u);

	tasks[0].runTimeCounter = 200;
	tasks[1].runTimeCounter = 1000;

	load.step(tasks, summary, settings, 0.5);

	/* 100 counts for the first task and 700 for the second, out of 800. */
	EXPECT_TRUE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 2u);
	EXPECT_DOUBLE_EQ(summary.windowSeconds, 0.5);

	ASSERT_TRUE(tasks[0].hasCpuShare);
	EXPECT_DOUBLE_EQ(tasks[0].cpuSharePercent, 12.5);

	ASSERT_TRUE(tasks[1].hasCpuShare);
	EXPECT_DOUBLE_EQ(tasks[1].cpuSharePercent, 87.5);
}

TEST(RtosCpuLoad, TheWindowGrowsToItsLengthAndThenStops)
{
	std::vector<RtosTask> tasks{makeTask(0, 0), makeTask(1, 0)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf(4);

	/* A round every half second, every task using the same amount each time:
	   the window should reach two seconds and stay there. */
	for (uint32_t round = 0; round <= 8; round++)
	{
		tasks[0].runTimeCounter = round * 100u;
		tasks[1].runTimeCounter = round * 100u;

		load.step(tasks, summary, settings, round * 0.5);
	}

	EXPECT_DOUBLE_EQ(summary.windowSeconds, 2.0);
	EXPECT_EQ(load.rememberedRounds(), 5u);

	/* Equal work, equal share - measured across the window rather than over
	   the last round, which is what makes the number hold still. */
	EXPECT_DOUBLE_EQ(tasks[0].cpuSharePercent, 50.0);
	EXPECT_DOUBLE_EQ(tasks[1].cpuSharePercent, 50.0);
}

TEST(RtosCpuLoad, OneBadRoundBarelyMovesAShareMeasuredOverSeveral)
{
	std::vector<RtosTask> tasks{makeTask(0, 0), makeTask(1, 0)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf(4);

	/* Two tasks using the same amount per round, until the last one, where the
	   first is preempted throughout and gets almost nothing done. */
	const uint32_t firstCounts[] = {0, 100, 200, 300, 400, 420};
	const uint32_t secondCounts[] = {0, 100, 200, 300, 400, 500};

	for (uint32_t round = 0; round < 6; round++)
	{
		tasks[0].runTimeCounter = firstCounts[round];
		tasks[1].runTimeCounter = secondCounts[round];

		load.step(tasks, summary, settings, round * 0.5);
	}

	/* The last round on its own would have said 20 out of 120, or 17 per cent.
	   Across the window it is 320 out of 720. */
	ASSERT_TRUE(tasks[0].hasCpuShare);
	EXPECT_NEAR(tasks[0].cpuSharePercent, 44.444, 0.001);
	EXPECT_NEAR(tasks[1].cpuSharePercent, 55.555, 0.001);
}

TEST(RtosCpuLoad, ACounterThatWrappedStillGivesTheDifference)
{
	std::vector<RtosTask> tasks{makeTask(0, 0xFFFFFF00u), makeTask(1, 0)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf();

	load.step(tasks, summary, settings, 0.0);

	tasks[0].runTimeCounter = 0x00000100u;
	tasks[1].runTimeCounter = 0x00000100u;

	load.step(tasks, summary, settings, 0.5);

	/* The first task used 512 counts across the wrap, the second 256. An
	   unsigned subtraction is what makes the wrap come out right. */
	EXPECT_TRUE(summary.counting);

	ASSERT_TRUE(tasks[0].hasCpuShare);
	EXPECT_NEAR(tasks[0].cpuSharePercent, 66.666, 0.001);
	EXPECT_NEAR(tasks[1].cpuSharePercent, 33.333, 0.001);
}

TEST(RtosCpuLoad, CountersThatDidNotMoveGiveNoShares)
{
	std::vector<RtosTask> tasks{makeTask(0, 500), makeTask(1, 900)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf();

	load.step(tasks, summary, settings, 0.0);
	load.step(tasks, summary, settings, 0.5);

	/* The counters were read twice and nothing changed. A share of zero out of
	   zero is not a share, and a column of zeroes would read as a system with
	   nothing to do. */
	EXPECT_FALSE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 2u);
	EXPECT_FALSE(tasks[0].hasCpuShare);
	EXPECT_FALSE(tasks[1].hasCpuShare);
}

TEST(RtosCpuLoad, AWindowThatReachesPastAMissedRoundStillAnswers)
{
	std::vector<RtosTask> both{makeTask(0, 100), makeTask(1, 100)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf(4);

	load.step(both, summary, settings, 0.0);

	both[0].runTimeCounter = 200;

	/* The second task was not in this round's table - a walk of one of the
	   kernel's lists can leave things out. The window still reaches back to
	   the round where it was there, so the missing round costs it nothing. */
	std::vector<RtosTask> firstOnly{both[0]};

	load.step(firstOnly, summary, settings, 0.5);

	both[0].runTimeCounter = 300;
	both[1].runTimeCounter = 300;

	load.step(both, summary, settings, 1.0);

	EXPECT_TRUE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 2u);
	ASSERT_TRUE(both[0].hasCpuShare);
	ASSERT_TRUE(both[1].hasCpuShare);
	EXPECT_DOUBLE_EQ(both[0].cpuSharePercent, 50.0);
	EXPECT_DOUBLE_EQ(both[1].cpuSharePercent, 50.0);
}

TEST(RtosCpuLoad, ARoundMissingATaskDoesNotScaleEveryoneElseUp)
{
	std::vector<RtosTask> both{makeTask(0, 0), makeTask(1, 0)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf(2);

	/* The middle round is one the second task was missing from - a walk of one
	   of the kernel's lists failed, so everything that was on it is left out.
	   Measuring every task against the oldest round held would drop it from
	   the whole and scale the first task up to a hundred per cent, which is
	   the busy half of the target disappearing and everything else growing to
	   fill the gap. */
	load.step(both, summary, settings, 0.0);

	both[0].runTimeCounter = 100;

	std::vector<RtosTask> firstOnly{both[0]};

	load.step(firstOnly, summary, settings, 0.5);

	both[0].runTimeCounter = 200;
	both[1].runTimeCounter = 100;
	load.step(both, summary, settings, 1.0);

	/* Both are in the whole, each against the oldest round that has it. */
	EXPECT_EQ(summary.tasksMeasured, 2u);
	ASSERT_TRUE(both[0].hasCpuShare);
	ASSERT_TRUE(both[1].hasCpuShare);
	EXPECT_NEAR(both[0].cpuSharePercent, 66.666, 0.001);
	EXPECT_NEAR(both[1].cpuSharePercent, 33.333, 0.001);

	both[0].runTimeCounter = 300;
	both[1].runTimeCounter = 200;
	load.step(both, summary, settings, 1.5);

	/* Now the oldest round held is the one the second task was missing from.
	   It is measured against the round after that instead, over half the
	   span, and its difference is divided by that span before the two are
	   compared: both tasks used a hundred counts per half second, so the
	   answer is half each. Compared as raw differences it would have read
	   200 against 100, and the task that lost half its span would have looked
	   like a task that did half the work. */
	EXPECT_EQ(summary.tasksMeasured, 2u);
	ASSERT_TRUE(both[0].hasCpuShare);
	ASSERT_TRUE(both[1].hasCpuShare);
	EXPECT_NEAR(both[0].cpuSharePercent, 50.0, 0.001);
	EXPECT_NEAR(both[1].cpuSharePercent, 50.0, 0.001);

	/* And the window is reported as the shortest span any share was measured
	   over, so the summary never claims more smoothing than it has. */
	EXPECT_DOUBLE_EQ(summary.windowSeconds, 0.5);
}

TEST(RtosCpuLoad, AControlBlockHandedOutAgainIsNotTheSameTask)
{
	std::vector<RtosTask> tasks{makeTask(0, 100, 11), makeTask(1, 100, 12)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf();

	load.step(tasks, summary, settings, 0.0);

	/* The kernel deleted the first task and gave its control block to another,
	   which starts its own counter. The address is the same, so the numbers
	   would look like a difference of a whole counter's worth of time. */
	tasks[0].kernelTaskNumber = 13;
	tasks[0].runTimeCounter = 0;
	tasks[1].runTimeCounter = 200;

	load.step(tasks, summary, settings, 0.5);

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
	const RtosCpuLoad::Settings settings = settingsOf();

	load.step(tasks, summary, settings, 0.0);

	EXPECT_EQ(load.rememberedRounds(), 1u);

	tasks[0].runTimeCounter = 200;

	load.step(tasks, summary, settings, 0.5);

	EXPECT_EQ(summary.tasksMeasured, 1u);
	ASSERT_TRUE(tasks[0].hasCpuShare);
	EXPECT_FALSE(tasks[1].hasCpuShare);
}

TEST(RtosCpuLoad, ForgettingDropsTheReferenceCounters)
{
	std::vector<RtosTask> tasks{makeTask(0, 100), makeTask(1, 100)};
	RtosCpuLoadSummary summary;
	RtosCpuLoad load;
	const RtosCpuLoad::Settings settings = settingsOf();

	load.step(tasks, summary, settings, 0.0);

	/* A new session against a different target must not difference its
	   counters against the previous one's. */
	load.forget();

	EXPECT_EQ(load.rememberedRounds(), 0u);

	tasks[0].runTimeCounter = 900;
	tasks[1].runTimeCounter = 900;

	load.step(tasks, summary, settings, 0.5);

	EXPECT_FALSE(summary.counting);
	EXPECT_EQ(summary.tasksMeasured, 0u);
}
