#include <gtest/gtest.h>

#include "RtosGapExplainer.hpp"

namespace
{
	RtosGapEvidence gap(double percent)
	{
		RtosGapEvidence evidence;

		evidence.unaccountedPercent = percent;

		return evidence;
	}
}  // namespace

TEST(RtosGapExplainerTest, ASmallGapIsNotExplained)
{
	/* Four percent is the sampling resolution, not a fault. A list of causes
	   under it would train the reader to ignore the list. */
	EXPECT_TRUE(explainGap(gap(4.0)).empty());
}

TEST(RtosGapExplainerTest, OverwrittenRecordsAreNamedFirst)
{
	/* An interrupt interval needs both of its events: a lost record leaves a
	   pair that never closes, and the time between them is exactly what goes
	   missing. This is the cause that explains a gap most completely. */
	RtosGapEvidence evidence = gap(14.0);

	evidence.eventsMissed = 4200;
	evidence.roundsRejected = 12;

	const std::vector<RtosGapHint> hints = explainGap(evidence);

	ASSERT_EQ(2u, hints.size());
	EXPECT_EQ(RtosGapCause::RecordsMissed, hints[0].cause);
	EXPECT_NE(std::string::npos, hints[0].sentence.find("never read"));
	EXPECT_EQ(4200u, hints[0].count);
	EXPECT_EQ(RtosGapCause::RoundsRejected, hints[1].cause);
	EXPECT_NE(std::string::npos, hints[1].sentence.find("did not hold together"));
}

TEST(RtosGapExplainerTest, EveryCauseIsNamedRatherThanMatchedByText)
{
	/* The panel prints these by cause, because the number sits inside the
	   sentence and a finished English sentence cannot be translated by lookup.
	   A cause that came back as the fallback would be drawn as "nothing
	   explains it" while the counts said otherwise, so each one is checked
	   here rather than left to the screenshot. */
	RtosGapEvidence evidence = gap(20.0);

	evidence.eventsMissed = 9;
	evidence.roundsRejected = 6;
	evidence.tornBatches = 3;
	evidence.tasksEstimated = 5;
	evidence.tasksKnown = 10;

	const std::vector<RtosGapHint> hints = explainGap(evidence);

	ASSERT_EQ(4u, hints.size());

	/* Ordered by count, so the order is the answer to "which explains most". */
	EXPECT_EQ(RtosGapCause::RecordsMissed, hints[0].cause);
	EXPECT_EQ(RtosGapCause::RoundsRejected, hints[1].cause);
	EXPECT_EQ(RtosGapCause::TasksEstimated, hints[2].cause);
	EXPECT_EQ(RtosGapCause::TornBatches, hints[3].cause);
}

TEST(RtosGapExplainerTest, TheEstimationHintCarriesBothNumbers)
{
	/* "5 of 10" needs both halves: the count alone reads as five gaps, which is
	   the opposite of what the hint says. */
	RtosGapEvidence evidence = gap(10.0);

	evidence.tasksEstimated = 5;
	evidence.tasksKnown = 10;

	const std::vector<RtosGapHint> hints = explainGap(evidence);

	ASSERT_EQ(1u, hints.size());
	EXPECT_EQ(RtosGapCause::TasksEstimated, hints[0].cause);
	EXPECT_EQ(5u, hints[0].count);
	EXPECT_EQ(10u, hints[0].total);
}

TEST(RtosGapExplainerTest, CausesWithNoCountAreNotMentioned)
{
	RtosGapEvidence evidence = gap(20.0);

	evidence.roundsRejected = 7;

	const std::vector<RtosGapHint> hints = explainGap(evidence);

	ASSERT_EQ(1u, hints.size());
	EXPECT_EQ(RtosGapCause::RoundsRejected, hints[0].cause);
	EXPECT_NE(std::string::npos, hints[0].sentence.find("did not hold together"));
}

TEST(RtosGapExplainerTest, AnUnexplainedGapSaysSo)
{
	/* Nothing counted explains it. Picking whichever counter happened to be
	   non-zero would be a guess dressed as a finding; this says the honest thing
	   and points at the one explanation left. */
	const std::vector<RtosGapHint> hints = explainGap(gap(30.0));

	ASSERT_EQ(1u, hints.size());
	EXPECT_EQ(RtosGapCause::Unexplained, hints[0].cause);
	EXPECT_NE(std::string::npos, hints[0].sentence.find("nothing in this session's counters"));
	EXPECT_EQ(0u, hints[0].count);
}

TEST(RtosGapExplainerTest, EstimationOnlyMattersWhenItIsALargeShare)
{
	/* One estimated task out of fourteen is noise; five out of ten means the
	   shares are mostly guesses. */
	RtosGapEvidence small = gap(10.0);

	small.tasksEstimated = 1;
	small.tasksKnown = 14;

	/* Not named as a cause - and because nothing else explains the gap either,
	   the list falls back to saying that rather than to naming the estimate. */
	const std::vector<RtosGapHint> quiet = explainGap(small);

	ASSERT_EQ(1u, quiet.size());
	EXPECT_EQ(RtosGapCause::Unexplained, quiet[0].cause);
	EXPECT_EQ(std::string::npos, quiet[0].sentence.find("estimated"));

	RtosGapEvidence large = gap(10.0);

	large.tasksEstimated = 5;
	large.tasksKnown = 10;

	const std::vector<RtosGapHint> hints = explainGap(large);

	ASSERT_EQ(1u, hints.size());
	EXPECT_EQ(RtosGapCause::TasksEstimated, hints[0].cause);
	EXPECT_NE(std::string::npos, hints[0].sentence.find("estimated"));
}
