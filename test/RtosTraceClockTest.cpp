#include "RtosTraceClock.hpp"

#include <gtest/gtest.h>

TEST(RtosTraceClockTest, TheFirstReadingDefinesTheBase)
{
	RtosTraceClock clock;

	EXPECT_FALSE(clock.hasBase());
	EXPECT_EQ(0x1234u, clock.extend(0x1234));
	EXPECT_TRUE(clock.hasBase());
	EXPECT_EQ(0u, clock.wraps());
}

TEST(RtosTraceClockTest, ACounterThatWrapsIsExtendedNotSubtracted)
{
	RtosTraceClock clock;

	clock.extend(0xFFFFFF00u);
	const uint64_t after = clock.extend(0x00000100u);

	/* One whole range plus the step over the wrap. */
	EXPECT_EQ(0x100000100ull, after);
	EXPECT_EQ(1u, clock.wraps());
}

TEST(RtosTraceClockTest, ASmallStepBackwardsIsNotAWrap)
{
	RtosTraceClock clock;

	clock.extend(0x00001000u);
	const uint64_t after = clock.extend(0x00000F00u);

	/* An out of order reading, not a wrap: counting it as one would add
	   sixty five seconds to a timeline that moved a microsecond. */
	EXPECT_EQ(0x00000F00ull, after);
	EXPECT_EQ(0u, clock.wraps());
}

TEST(RtosTraceClockTest, TwoWrapsCountTwice)
{
	RtosTraceClock clock;

	clock.extend(0xFFFFFFF0u);
	clock.extend(0x00000010u);
	clock.extend(0xFFFFFFF0u);
	const uint64_t after = clock.extend(0x00000010u);

	EXPECT_EQ(2u, clock.wraps());
	EXPECT_EQ(0x200000010ull, after);
}

TEST(RtosTraceClockTest, ADurationInsideOneFrequencyIsPlainArithmetic)
{
	RtosTraceClock clock;

	clock.extend(1000);
	clock.extend(1168);
	clock.noteFrequency(0, 168000000u);

	const std::optional<double> seconds = clock.secondsBetween(1000, 1168);

	ASSERT_TRUE(seconds.has_value());
	EXPECT_DOUBLE_EQ(1e-6, *seconds);
}

TEST(RtosTraceClockTest, ADurationAcrossAFrequencyChangeIsNotComputable)
{
	RtosTraceClock clock;

	clock.extend(1000);
	clock.noteFrequency(0, 168000000u);
	clock.noteFrequency(5000, 80000000u);

	const std::optional<double> across = clock.secondsBetween(1000, 6000);
	EXPECT_FALSE(across.has_value());

	/* Both ends inside the second interval is answerable again. */
	const std::optional<double> inside = clock.secondsBetween(5000, 5080);
	ASSERT_TRUE(inside.has_value());
	EXPECT_DOUBLE_EQ(1e-6, *inside);
}

TEST(RtosTraceClockTest, WithoutAFrequencyNothingIsComputable)
{
	RtosTraceClock clock;

	clock.extend(1000);
	clock.extend(2000);

	EXPECT_FALSE(clock.secondsBetween(1000, 2000).has_value());
	EXPECT_EQ(0u, clock.frequencyAt(1500));

	clock.noteFrequency(0, 0);
	EXPECT_FALSE(clock.secondsBetween(1000, 2000).has_value());
}

TEST(RtosTraceClockTest, BackwardsIntervalsAreNotADurationEither)
{
	RtosTraceClock clock;
	clock.noteFrequency(0, 1000000u);

	EXPECT_FALSE(clock.secondsBetween(2000, 1000).has_value());
}

TEST(RtosTraceClockTest, TheSameFrequencyDoesNotStartANewInterval)
{
	RtosTraceClock clock;

	clock.noteFrequency(0, 168000000u);
	clock.noteFrequency(900, 168000000u);

	/* A duration spanning the second call is still plain arithmetic. */
	const std::optional<double> seconds = clock.secondsBetween(0, 168);

	ASSERT_TRUE(seconds.has_value());
	EXPECT_DOUBLE_EQ(1e-6, *seconds);
	EXPECT_EQ(168000000u, clock.frequencyAt(1000));
}

TEST(RtosTraceClockTest, ResetForgetsEverything)
{
	RtosTraceClock clock;

	clock.extend(0xFFFFFFF0u);
	clock.extend(0x00000010u);
	clock.noteFrequency(0, 1000u);
	clock.reset();

	EXPECT_FALSE(clock.hasBase());
	EXPECT_EQ(0u, clock.wraps());
	EXPECT_FALSE(clock.secondsBetween(0, 1).has_value());
}
