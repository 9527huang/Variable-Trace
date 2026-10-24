#include <gtest/gtest.h>

#include "RtosTimeBudget.hpp"

namespace
{
	RtosTimeline::StateInterval band(double from, double to)
	{
		RtosTimeline::StateInterval interval;

		interval.tcbAddress = 0x1000;
		interval.state = RtosTaskState::Running;
		interval.from = from;
		interval.to = to;

		return interval;
	}

	RtosTrackEntry isr(double from, double to)
	{
		RtosTrackEntry entry;

		entry.kind = RtosTrackEntry::Kind::Isr;
		entry.hasSeconds = true;
		entry.fromSeconds = from;
		entry.toSeconds = to;

		return entry;
	}
}  // namespace

TEST(RtosTimeBudgetTest, TasksAndInterruptsCoverTheWholeWindow)
{
	/* Eight seconds of task time, two of interrupt, in a ten second window:
	   nothing is missing and the panel should say so. */
	const std::vector<RtosTimeline::StateInterval> bands = {band(0.0, 8.0)};
	const std::vector<RtosTrackEntry> interrupts = {isr(8.0, 10.0)};

	const RtosTimeBudget budget = computeTimeBudget(bands, interrupts, 0.0, 10.0);

	EXPECT_DOUBLE_EQ(10.0, budget.windowSeconds);
	EXPECT_DOUBLE_EQ(8.0, budget.taskSeconds);
	EXPECT_DOUBLE_EQ(2.0, budget.isrSeconds);
	EXPECT_DOUBLE_EQ(0.0, budget.unaccountedSeconds);
	EXPECT_DOUBLE_EQ(80.0, budget.taskPercent());
	EXPECT_DOUBLE_EQ(20.0, budget.isrPercent());
	EXPECT_FALSE(budget.overlaps());
}

TEST(RtosTimeBudgetTest, AStretchNobodyReadIsUnaccountedNotNormalisedAway)
{
	/* Three seconds of the ten are covered by neither: two rounds missed the
	   target, or events were lost. That is the finding - the parts must not be
	   rescaled to add up to a hundred. */
	const std::vector<RtosTimeline::StateInterval> bands = {band(0.0, 5.0)};
	const std::vector<RtosTrackEntry> interrupts = {isr(5.0, 7.0)};

	const RtosTimeBudget budget = computeTimeBudget(bands, interrupts, 0.0, 10.0);

	EXPECT_DOUBLE_EQ(3.0, budget.unaccountedSeconds);
	EXPECT_DOUBLE_EQ(30.0, budget.unaccountedPercent());
	EXPECT_DOUBLE_EQ(50.0, budget.taskPercent());
	EXPECT_DOUBLE_EQ(20.0, budget.isrPercent());
}

TEST(RtosTimeBudgetTest, OverlappingAccountsAreReportedRatherThanClamped)
{
	/* An interrupt cannot run inside a task. When the data says it did, one of
	   the two accounts is wrong - and "minus a bit" is a better answer than a
	   zero that hides it. */
	const std::vector<RtosTimeline::StateInterval> bands = {band(0.0, 10.0)};
	const std::vector<RtosTrackEntry> interrupts = {isr(2.0, 4.0)};

	const RtosTimeBudget budget = computeTimeBudget(bands, interrupts, 0.0, 10.0);

	EXPECT_DOUBLE_EQ(-2.0, budget.unaccountedSeconds);
	EXPECT_TRUE(budget.overlaps());
}

TEST(RtosTimeBudgetTest, OverlappingTaskSamplesCountOnce)
{
	/* Two tasks whose samples cover the same seconds - a task running on two
	   cores, or a round that read the same interval twice. The CPU has one
	   second of time there, not two. */
	const std::vector<RtosTimeline::StateInterval> bands = {band(0.0, 4.0), band(0.0, 4.0)};
	const std::vector<RtosTrackEntry> interrupts = {isr(4.0, 10.0)};

	const RtosTimeBudget budget = computeTimeBudget(bands, interrupts, 0.0, 10.0);

	EXPECT_DOUBLE_EQ(4.0, budget.taskSeconds);
	EXPECT_DOUBLE_EQ(6.0, budget.isrSeconds);
	EXPECT_DOUBLE_EQ(0.0, budget.unaccountedSeconds);
}

TEST(RtosTimeBudgetTest, SpansOutsideTheWindowContributeNothing)
{
	const std::vector<RtosTimeline::StateInterval> bands = {band(-5.0, 1.0), band(9.0, 15.0)};
	const std::vector<RtosTrackEntry> interrupts = {isr(20.0, 21.0)};

	const RtosTimeBudget budget = computeTimeBudget(bands, interrupts, 0.0, 10.0);

	/* -5..1 contributes 0..1, 9..15 contributes 9..10, and the interrupt is
	   entirely outside. Two seconds of task time, eight unaccounted. */
	EXPECT_DOUBLE_EQ(2.0, budget.taskSeconds);
	EXPECT_DOUBLE_EQ(0.0, budget.isrSeconds);
	EXPECT_DOUBLE_EQ(8.0, budget.unaccountedSeconds);
}

TEST(RtosTimeBudgetTest, NoWindowMeansNoBudget)
{
	const std::vector<RtosTimeline::StateInterval> bands = {band(0.0, 1.0)};

	const RtosTimeBudget budget = computeTimeBudget(bands, {}, 5.0, 5.0);

	EXPECT_DOUBLE_EQ(0.0, budget.windowSeconds);
	EXPECT_DOUBLE_EQ(0.0, budget.taskPercent());
}
