#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "RtosEventRing.hpp"

namespace
{
	RtosEventRing::Record record(uint32_t tcb, uint32_t timeUs)
	{
		RtosEventRing::Record entry;

		entry.tcbAddress = tcb;
		entry.timeUs = timeUs;

		return entry;
	}
}  // namespace

TEST(RtosEventRing, ReadsWhatIsStillInTheRingAndCountsWhatFellOut)
{
	/* Ten written into a ring of eight: the first two are gone before anyone
	   looked, and a reader that is told so can say the picture has a hole. */
	const RtosEventRing::Window window = RtosEventRing::windowFor(10, 0, 8, 64);

	EXPECT_EQ(window.first, 2u);
	EXPECT_EQ(window.count, 8u);
	EXPECT_EQ(window.lost, 2u);
}

TEST(RtosEventRing, NothingToReadOnceTheReaderHasCaughtUp)
{
	const RtosEventRing::Window window = RtosEventRing::windowFor(10, 10, 8, 64);

	EXPECT_EQ(window.count, 0u);
	EXPECT_EQ(window.lost, 0u);
}

TEST(RtosEventRing, NothingIsLostBeforeTheRingHasWrapped)
{
	const RtosEventRing::Window window = RtosEventRing::windowFor(3, 0, 8, 64);

	EXPECT_EQ(window.first, 0u);
	EXPECT_EQ(window.count, 3u);
	EXPECT_EQ(window.lost, 0u);
}

TEST(RtosEventRing, ABudgetTakesTheNewestAndCountsTheRestAsLost)
{
	/* More waiting than the round has budget to read. The newest are taken -
	   the reader is watching what is happening now - and the rest are reported
	   as skipped rather than quietly dropped. */
	const RtosEventRing::Window window = RtosEventRing::windowFor(100, 90, 64, 4);

	EXPECT_EQ(window.first, 96u);
	EXPECT_EQ(window.count, 4u);
	EXPECT_EQ(window.lost, 6u);
}

TEST(RtosEventRing, TheRecordsBecomeTheRunsTheyDescribe)
{
	/* A switched in at 1 ms, B at 2, A again at 3, and the batch ends at 50. */
	const std::vector<RtosEventRing::Record> records{record(0x100, 1000), record(0x200, 2000), record(0x100, 3000)};

	const std::vector<RtosEventRing::Interval> intervals = RtosEventRing::intervalsFrom(records, 1000, 10.0, 10.050);

	ASSERT_EQ(intervals.size(), 3u);

	EXPECT_EQ(intervals[0].tcbAddress, 0x100u);
	EXPECT_DOUBLE_EQ(intervals[0].fromSeconds, 10.000);
	EXPECT_DOUBLE_EQ(intervals[0].toSeconds, 10.001);

	EXPECT_EQ(intervals[1].tcbAddress, 0x200u);
	EXPECT_DOUBLE_EQ(intervals[1].fromSeconds, 10.001);
	EXPECT_DOUBLE_EQ(intervals[1].toSeconds, 10.002);

	/* The one switched in last is still running as far as this batch knows. */
	EXPECT_EQ(intervals[2].tcbAddress, 0x100u);
	EXPECT_DOUBLE_EQ(intervals[2].fromSeconds, 10.002);
	EXPECT_DOUBLE_EQ(intervals[2].toSeconds, 10.050);
}

TEST(RtosEventRing, ASwitchToTheTaskAlreadyRunningIsNotANewRun)
{
	const std::vector<RtosEventRing::Record> records{record(0x100, 1000), record(0x100, 2000), record(0x200, 3000)};

	const std::vector<RtosEventRing::Interval> intervals = RtosEventRing::intervalsFrom(records, 1000, 0.0, 0.010);

	/* Two runs, not three: the first task's run began at its first record. */
	ASSERT_EQ(intervals.size(), 2u);
	EXPECT_EQ(intervals[0].tcbAddress, 0x100u);
	EXPECT_DOUBLE_EQ(intervals[0].fromSeconds, 0.0);
	EXPECT_DOUBLE_EQ(intervals[0].toSeconds, 0.002);
}

TEST(RtosEventRing, TheCounterWrappingIsASmallStepForward)
{
	/* The target's microsecond counter clears at about seventy-one minutes. An
	   unsigned difference comes out as the small step forward it is. */
	const std::vector<RtosEventRing::Record> records{record(0x100, 0xFFFFFF00u), record(0x200, 0x00000100u)};

	const std::vector<RtosEventRing::Interval> intervals = RtosEventRing::intervalsFrom(records, 0xFFFFFF00u, 0.0, 0.001);

	ASSERT_EQ(intervals.size(), 2u);
	EXPECT_NEAR(intervals[0].toSeconds, 512.0 / 1000000.0, 1e-12);
}

TEST(RtosEventRing, ABatchThatEndsBeforeItStartedLeavesTheLastRunOpen)
{
	const std::vector<RtosEventRing::Record> records{record(0x100, 5000)};

	/* The caller's clock went backwards - a new session on the host while the
	   target kept counting. A run of negative length would be worse than none. */
	const std::vector<RtosEventRing::Interval> intervals = RtosEventRing::intervalsFrom(records, 5000, 10.0, 9.0);

	EXPECT_TRUE(intervals.empty());
}

TEST(RtosEventRing, AReaderAheadOfTheWriterHasMetARestartedTarget)
{
	/* The reader had got to 5000; the target's counter is at 100. That cannot
	   happen unless the target began again, and a reader that kept its place
	   would wait for ever for a head it had already passed. */
	const RtosEventRing::Window window = RtosEventRing::windowFor(100, 5000, 512, 192);

	EXPECT_TRUE(window.restarted);
	EXPECT_EQ(window.first, 0u);
	EXPECT_EQ(window.count, 100u);
}

TEST(RtosEventRing, ACaughtUpReaderIsNotARestart)
{
	/* Equal is the ordinary "nothing new" case, not a restart. */
	const RtosEventRing::Window window = RtosEventRing::windowFor(100, 100, 512, 192);

	EXPECT_FALSE(window.restarted);
	EXPECT_EQ(window.count, 0u);
}
