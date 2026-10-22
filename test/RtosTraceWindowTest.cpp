#include "RtosTraceWindow.hpp"

#include <gtest/gtest.h>

namespace
{
	RtosTrackEntry entry(RtosTrackEntry::Kind kind, double from, double to)
	{
		RtosTrackEntry made;

		made.kind = kind;
		made.hasSeconds = true;
		made.fromSeconds = from;
		made.toSeconds = to;

		return made;
	}

	RtosTraceWindow::Options window(double from, double to, int pixels)
	{
		RtosTraceWindow::Options options;

		options.fromSeconds = from;
		options.toSeconds = to;
		options.pixels = pixels;

		return options;
	}

	uint32_t totalCount(const RtosTraceWindow::Result& result)
	{
		uint32_t total = 0;

		for (const RtosTraceWindow::Bucket& bucket : result.buckets)
			total += bucket.count;

		return total;
	}
}  // namespace

TEST(RtosTraceWindowTest, OnlyWhatTouchesTheWindowIsCounted)
{
	const std::vector<RtosTrackEntry> entries = {
	    entry(RtosTrackEntry::Kind::Isr, 0.5, 0.6),
	    entry(RtosTrackEntry::Kind::Isr, 1.5, 1.6),
	    entry(RtosTrackEntry::Kind::Isr, 9.0, 9.1),
	};

	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 2.0, 100));

	EXPECT_EQ(1u, result.matched);
	EXPECT_EQ(1u, totalCount(result));
}

TEST(RtosTraceWindowTest, AnIntervalRunningIntoTheWindowIsInIt)
{
	/* It started before the window and ended inside: it was running here, and a
	   reader looking at this stretch wants to see it. */
	const std::vector<RtosTrackEntry> entries = {entry(RtosTrackEntry::Kind::Isr, 0.2, 1.4)};

	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 2.0, 100));

	EXPECT_EQ(1u, result.matched);
	EXPECT_EQ(1u, totalCount(result));
}

TEST(RtosTraceWindowTest, ManyEntriesInFewColumnsBecomeFewBucketsWithCounts)
{
	std::vector<RtosTrackEntry> entries;

	for (int index = 0; index < 100; index++)
	{
		const double from = 1.0 + 0.001 * index;

		entries.push_back(entry(RtosTrackEntry::Kind::Isr, from, from + 0.0005));
	}

	/* Two columns over the window: everything lands in one of them, and the
	   count is what a column that cannot be resolved still owes the reader. */
	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 1.1, 2));

	ASSERT_EQ(2u, result.buckets.size());
	EXPECT_EQ(100u, result.matched);
	EXPECT_EQ(100u, totalCount(result));

	for (const RtosTraceWindow::Bucket& bucket : result.buckets)
		EXPECT_TRUE(bucket.hasDuration);
}

TEST(RtosTraceWindowTest, TheLongestIntervalInAColumnIsKept)
{
	const std::vector<RtosTrackEntry> entries = {
	    entry(RtosTrackEntry::Kind::Isr, 1.00, 1.01),
	    entry(RtosTrackEntry::Kind::Isr, 1.02, 1.09),
	    entry(RtosTrackEntry::Kind::Isr, 1.03, 1.04),
	};

	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 1.1, 1));

	ASSERT_EQ(1u, result.buckets.size());
	EXPECT_EQ(3u, result.buckets[0].count);
	EXPECT_NEAR(0.07, result.buckets[0].longestSeconds, 1e-9);
}

TEST(RtosTraceWindowTest, AnEntryWithNoTargetTimeIsCountedAndNotPlaced)
{
	std::vector<RtosTrackEntry> entries = {entry(RtosTrackEntry::Kind::Isr, 1.2, 1.3)};

	RtosTrackEntry untimed;
	untimed.kind = RtosTrackEntry::Kind::Isr;
	untimed.hasSeconds = false;

	entries.push_back(untimed);

	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 2.0, 100));

	EXPECT_EQ(1u, result.matched);
	EXPECT_EQ(1u, result.untimed);

	/* The one without a time did not become a zero-length interval at the
	   start of the window. */
	EXPECT_EQ(1u, totalCount(result));
}

TEST(RtosTraceWindowTest, AFaultIsNeverAveragedAway)
{
	std::vector<RtosTrackEntry> entries;

	/* Fifty ordinary entries and one fault, all in the same column: an
	   aggregation that kept only "the longest" would drop the fault. */
	for (int index = 0; index < 50; index++)
		entries.push_back(entry(RtosTrackEntry::Kind::Isr, 1.00, 1.0001));

	RtosTrackEntry fault = entry(RtosTrackEntry::Kind::Fault, 1.0005, 1.0005);
	fault.faultClass = 2;
	entries.push_back(fault);

	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 1.01, 1));

	ASSERT_EQ(1u, result.buckets.size());
	EXPECT_TRUE(result.buckets[0].hasFault);
	EXPECT_EQ(2u, result.buckets[0].worstFaultClass);

	/* And the fault is still counted: it is one of the fifty one entries, not a
	   replacement for the column. */
	EXPECT_EQ(51u, result.buckets[0].count);
}

TEST(RtosTraceWindowTest, TheWorstFaultInAColumnIsTheOneReported)
{
	const std::vector<RtosTrackEntry> entries = {
	    entry(RtosTrackEntry::Kind::Fault, 1.001, 1.001),
	    entry(RtosTrackEntry::Kind::Fault, 1.002, 1.002),
	};

	RtosTrackEntry first = entries[0];
	first.faultClass = 1;

	RtosTrackEntry second = entries[1];
	second.faultClass = 3;

	std::vector<RtosTrackEntry> both = {first, second};

	const RtosTraceWindow::Result result = RtosTraceWindow::query(both, window(1.0, 1.01, 1));

	ASSERT_EQ(1u, result.buckets.size());
	EXPECT_EQ(3u, result.buckets[0].worstFaultClass);
}

TEST(RtosTraceWindowTest, AGapIsNeverAveragedAwayEither)
{
	std::vector<RtosTrackEntry> entries;

	for (int index = 0; index < 20; index++)
		entries.push_back(entry(RtosTrackEntry::Kind::Isr, 1.00, 1.0001));

	entries.push_back(entry(RtosTrackEntry::Kind::Gap, 1.0005, 1.0005));

	const RtosTraceWindow::Result result = RtosTraceWindow::query(entries, window(1.0, 1.01, 1));

	ASSERT_EQ(1u, result.buckets.size());
	EXPECT_TRUE(result.buckets[0].hasGap);
	EXPECT_EQ(21u, result.buckets[0].count);
}

TEST(RtosTraceWindowTest, OnlyFaultAndGapArePromisedNeverToBeAggregated)
{
	EXPECT_TRUE(RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind::Fault));
	EXPECT_TRUE(RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind::Gap));

	/* Everything else may be collapsed into a count: an ISR column with forty
	   entries in it is a busy column, and nobody reads forty bands there. */
	EXPECT_FALSE(RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind::Isr));
	EXPECT_FALSE(RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind::Sync));
	EXPECT_FALSE(RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind::Heap));
	EXPECT_FALSE(RtosTraceWindow::neverAggregated(RtosTrackEntry::Kind::Task));
}

TEST(RtosTraceWindowTest, AnEmptyWindowOrAnEmptyTrackIsAnEmptyAnswer)
{
	const std::vector<RtosTrackEntry> entries = {entry(RtosTrackEntry::Kind::Isr, 1.0, 1.1)};

	EXPECT_TRUE(RtosTraceWindow::query(entries, window(2.0, 2.0, 10)).buckets.empty());
	EXPECT_TRUE(RtosTraceWindow::query({}, window(1.0, 2.0, 10)).buckets.size() == 10);

	const RtosTraceWindow::Result backwards = RtosTraceWindow::query(entries, window(2.0, 1.0, 10));
	EXPECT_TRUE(backwards.buckets.empty());
}

TEST(RtosTraceWindowTest, AColumnIsAsWideAsTheAxisSaysItIs)
{
	const RtosTraceWindow::Result result = RtosTraceWindow::query({}, window(10.0, 20.0, 5));

	ASSERT_EQ(5u, result.buckets.size());
	EXPECT_DOUBLE_EQ(10.0, result.buckets[0].fromSeconds);
	EXPECT_DOUBLE_EQ(12.0, result.buckets[0].toSeconds);
	EXPECT_DOUBLE_EQ(20.0, result.buckets[4].toSeconds);
}
