#include "RtosEventQuery.hpp"

#include <gtest/gtest.h>

namespace
{
	using Interval = RtosEventRing::Interval;

	std::vector<Interval> makeRuns()
	{
		/* Four runs of task 1 interleaved with three of task 2, in time order:
		   1.00, 1.10, 1.20, 1.30, 1.40, 1.50, 1.60. */
		std::vector<Interval> runs;

		const uint32_t addresses[] = {1, 2, 1, 2, 1, 2, 1};

		for (size_t index = 0; index < 7; index++)
		{
			Interval run;
			run.tcbAddress = addresses[index];
			run.fromSeconds = 1.0 + 0.1 * static_cast<double>(index);
			run.toSeconds = run.fromSeconds + 0.05;
			runs.push_back(run);
		}

		return runs;
	}
}

TEST(RtosEventQueryTest, KeepsEveryRunOfEveryTaskWithoutAFilter)
{
	const RtosEventQuery::Selection selection = RtosEventQuery::select(makeRuns(), 0, 0);

	EXPECT_EQ(7u, selection.matched);
	ASSERT_EQ(7u, selection.rows.size());
	EXPECT_NEAR(1.0, selection.rows.front().fromSeconds, 1e-9);
	EXPECT_NEAR(1.6, selection.rows.back().fromSeconds, 1e-9);
}

TEST(RtosEventQueryTest, KeepsOnlyTheTaskAskedFor)
{
	const RtosEventQuery::Selection selection = RtosEventQuery::select(makeRuns(), 1, 0);

	EXPECT_EQ(4u, selection.matched);
	ASSERT_EQ(4u, selection.rows.size());

	for (const RtosEventQuery::Row& row : selection.rows)
		EXPECT_EQ(1u, row.tcbAddress);

	EXPECT_NEAR(1.0, selection.rows.front().fromSeconds, 1e-9);
	EXPECT_NEAR(1.6, selection.rows.back().fromSeconds, 1e-9);
}

TEST(RtosEventQueryTest, CountsEveryMatchAndKeepsTheNewestOfThem)
{
	/* The count is what survives the cut: "2 of 4" and not "2". */
	const RtosEventQuery::Selection selection = RtosEventQuery::select(makeRuns(), 1, 2);

	EXPECT_EQ(4u, selection.matched);
	ASSERT_EQ(2u, selection.rows.size());
	EXPECT_NEAR(1.4, selection.rows.front().fromSeconds, 1e-9);
	EXPECT_NEAR(1.6, selection.rows.back().fromSeconds, 1e-9);
}

TEST(RtosEventQueryTest, KeepsTheOldestFirstSoTheListReadsLeftToRight)
{
	const RtosEventQuery::Selection selection = RtosEventQuery::select(makeRuns(), 0, 3);

	ASSERT_EQ(3u, selection.rows.size());
	EXPECT_NEAR(1.4, selection.rows[0].fromSeconds, 1e-9);
	EXPECT_NEAR(1.5, selection.rows[1].fromSeconds, 1e-9);
	EXPECT_NEAR(1.6, selection.rows[2].fromSeconds, 1e-9);
}

TEST(RtosEventQueryTest, AnswersWithNothingWhenTheTaskNeverRan)
{
	const RtosEventQuery::Selection selection = RtosEventQuery::select(makeRuns(), 99, 600);

	EXPECT_EQ(0u, selection.matched);
	EXPECT_TRUE(selection.rows.empty());
}

TEST(RtosEventQueryTest, AnswersWithNothingWhenThereAreNoRuns)
{
	const RtosEventQuery::Selection selection = RtosEventQuery::select({}, 0, 600);

	EXPECT_EQ(0u, selection.matched);
	EXPECT_TRUE(selection.rows.empty());
}

TEST(RtosEventQueryTest, ReportsHowLongEachRunLasted)
{
	const RtosEventQuery::Selection selection = RtosEventQuery::select(makeRuns(), 0, 1);

	ASSERT_EQ(1u, selection.rows.size());
	EXPECT_NEAR(0.05, selection.rows.front().ranForSeconds(), 1e-9);
}
