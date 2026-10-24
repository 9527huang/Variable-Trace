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
	EXPECT_NE(std::string::npos, hints[0].sentence.find("never read"));
	EXPECT_EQ(4200u, hints[0].count);
	EXPECT_NE(std::string::npos, hints[1].sentence.find("did not hold together"));
}

TEST(RtosGapExplainerTest, CausesWithNoCountAreNotMentioned)
{
	RtosGapEvidence evidence = gap(20.0);

	evidence.roundsRejected = 7;

	const std::vector<RtosGapHint> hints = explainGap(evidence);

	ASSERT_EQ(1u, hints.size());
	EXPECT_NE(std::string::npos, hints[0].sentence.find("did not hold together"));
}

TEST(RtosGapExplainerTest, AnUnexplainedGapSaysSo)
{
	/* Nothing counted explains it. Picking whichever counter happened to be
	   non-zero would be a guess dressed as a finding; this says the honest thing
	   and points at the one explanation left. */
	const std::vector<RtosGapHint> hints = explainGap(gap(30.0));

	ASSERT_EQ(1u, hints.size());
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
	EXPECT_EQ(std::string::npos, quiet[0].sentence.find("estimated"));

	RtosGapEvidence large = gap(10.0);

	large.tasksEstimated = 5;
	large.tasksKnown = 10;

	const std::vector<RtosGapHint> hints = explainGap(large);

	ASSERT_EQ(1u, hints.size());
	EXPECT_NE(std::string::npos, hints[0].sentence.find("estimated"));
}
