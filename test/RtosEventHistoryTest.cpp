#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "RtosEventHistory.hpp"

namespace
{
	RtosEventRing::Interval run(uint32_t tcb, double from, double to)
	{
		RtosEventRing::Interval interval;

		interval.tcbAddress = tcb;
		interval.fromSeconds = from;
		interval.toSeconds = to;

		return interval;
	}
}  // namespace

TEST(RtosEventHistory, KeepsTheRunsInOrderAndAnswersByRange)
{
	RtosEventHistory history;

	history.append({run(0x100, 1.0, 1.5), run(0x200, 1.5, 1.8)}, 600.0);

	EXPECT_EQ(history.size(), 2u);

	std::vector<RtosEventRing::Interval> copied;
	history.copyRange(1.4, 2.0, copied);

	/* The run that starts before the range is copied: the caller clips, because
	   it is the caller that knows what to draw. */
	ASSERT_EQ(copied.size(), 2u);
	EXPECT_EQ(copied[0].tcbAddress, 0x100u);
	EXPECT_EQ(copied[1].tcbAddress, 0x200u);

	history.copyRange(5.0, 6.0, copied);
	EXPECT_TRUE(copied.empty());

	/* The range the runs cover, which is how a mapping from the target's clock
	   that is off is seen as a number rather than as an empty strip. */
	EXPECT_DOUBLE_EQ(history.oldestSeconds(), 1.0);
	EXPECT_DOUBLE_EQ(history.newestSeconds(), 1.8);
}

TEST(RtosEventHistory, TheRunThatWasStillGoingIsNotDrawnTwice)
{
	RtosEventHistory history;

	/* The second batch starts with the run the first one ended on. */
	history.append({run(0x100, 1.0, 1.5), run(0x200, 1.5, 1.6)}, 600.0);
	history.append({run(0x200, 1.6, 1.9), run(0x100, 1.9, 2.0)}, 600.0);

	EXPECT_EQ(history.size(), 3u);

	std::vector<RtosEventRing::Interval> copied;
	history.copyRange(0.0, 10.0, copied);

	ASSERT_EQ(copied.size(), 3u);
	EXPECT_EQ(copied[1].tcbAddress, 0x200u);
	EXPECT_DOUBLE_EQ(copied[1].fromSeconds, 1.5);
	EXPECT_DOUBLE_EQ(copied[1].toSeconds, 1.9);
}

TEST(RtosEventHistory, ForgetsWhatIsOlderThanTheWindow)
{
	RtosEventHistory history;

	history.append({run(0x100, 0.0, 1.0), run(0x200, 1.0, 2.0)}, 600.0);

	/* A window of a second and a half: the first run ends before the newest
	   minus the window, and goes. */
	history.append({run(0x100, 2.0, 3.0)}, 1.5);

	EXPECT_EQ(history.size(), 2u);

	std::vector<RtosEventRing::Interval> copied;
	history.copyRange(0.0, 10.0, copied);

	ASSERT_EQ(copied.size(), 2u);
	EXPECT_DOUBLE_EQ(copied.front().fromSeconds, 1.0);
}

TEST(RtosEventHistory, CountsWhatNeverCrossed)
{
	RtosEventHistory history;

	EXPECT_EQ(history.lostRecords(), 0u);

	history.noteLostRecords(3);
	history.noteLostRecords(0);
	history.noteLostRecords(2);

	EXPECT_EQ(history.lostRecords(), 5u);

	history.clear();
	EXPECT_EQ(history.lostRecords(), 0u);
	EXPECT_EQ(history.size(), 0u);
}
