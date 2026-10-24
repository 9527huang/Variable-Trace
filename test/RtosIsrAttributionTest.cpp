#include <gtest/gtest.h>

#include "RtosIsrAttribution.hpp"

namespace
{
	RtosTimeline::StateInterval run(uint32_t tcb, double from, double to)
	{
		RtosTimeline::StateInterval interval;

		interval.tcbAddress = tcb;
		interval.state = RtosTaskState::Running;
		interval.from = from;
		interval.to = to;

		return interval;
	}

	RtosTrackEntry interrupt(double from, double to, uint8_t depth = 1)
	{
		RtosTrackEntry entry;

		entry.kind = RtosTrackEntry::Kind::Isr;
		entry.hasSeconds = true;
		entry.fromSeconds = from;
		entry.toSeconds = to;
		entry.depth = depth;

		return entry;
	}
}  // namespace

TEST(RtosIsrAttributionTest, AnInterruptLandsOnTheTaskRunningAtThatInstant)
{
	const std::vector<RtosTimeline::StateInterval> running = {run(0x1000, 1.0, 2.0), run(0x1040, 2.0, 3.0)};
	const std::vector<RtosTrackEntry> isr = {interrupt(2.5, 2.501)};

	const RtosIsrAttribution attribution = attributeIsr(running, isr);

	ASSERT_EQ(1u, attribution.hits.size());
	EXPECT_EQ(0x1040u, attribution.hits[0].tcbAddress);
	EXPECT_DOUBLE_EQ(2.5, attribution.hits[0].seconds);
}

TEST(RtosIsrAttributionTest, ABoundaryBelongsToTheTaskThatStartsThere)
{
	/* Intervals are half open: the one that ended at 2.0 was not running at 2.0.
	   Attributing a hit on the boundary to both - or to the earlier one - puts a
	   mark on a lane the task had already left. */
	const std::vector<RtosTimeline::StateInterval> running = {run(0x1000, 1.0, 2.0), run(0x1040, 2.0, 3.0)};
	const std::vector<RtosTrackEntry> isr = {interrupt(2.0, 2.001)};

	const RtosIsrAttribution attribution = attributeIsr(running, isr);

	ASSERT_EQ(1u, attribution.hits.size());
	EXPECT_EQ(0x1040u, attribution.hits[0].tcbAddress);
}

TEST(RtosIsrAttributionTest, AnInterruptBetweenRoundsIsCountedNotPlaced)
{
	/* No task was recorded as running at 5.0 - the rounds missed it, or nobody
	   was. A mark on the wrong lane would be worse than no mark, so this is
	   counted and the caller can say how many. */
	const std::vector<RtosTimeline::StateInterval> running = {run(0x1000, 1.0, 2.0)};
	const std::vector<RtosTrackEntry> isr = {interrupt(5.0, 5.001)};

	const RtosIsrAttribution attribution = attributeIsr(running, isr);

	EXPECT_TRUE(attribution.hits.empty());
	EXPECT_EQ(1u, attribution.unattributed);
}

TEST(RtosIsrAttributionTest, AnUntimedIntervalIsSkippedRatherThanComplainedAbout)
{
	RtosTrackEntry untimed = interrupt(1.5, 1.501);

	untimed.hasSeconds = false;

	const std::vector<RtosTimeline::StateInterval> running = {run(0x1000, 1.0, 2.0)};

	const RtosIsrAttribution attribution = attributeIsr(running, {untimed});

	EXPECT_TRUE(attribution.hits.empty());
	EXPECT_EQ(0u, attribution.unattributed);
}

TEST(RtosIsrAttributionTest, DepthSurvivesTheAttribution)
{
	const std::vector<RtosTimeline::StateInterval> running = {run(0x1000, 1.0, 2.0)};
	const std::vector<RtosTrackEntry> isr = {interrupt(1.1, 1.101, 1), interrupt(1.2, 1.201, 2)};

	const RtosIsrAttribution attribution = attributeIsr(running, isr);

	ASSERT_EQ(2u, attribution.hits.size());
	EXPECT_EQ(1, attribution.hits[0].depth);
	EXPECT_EQ(2, attribution.hits[1].depth);
}
