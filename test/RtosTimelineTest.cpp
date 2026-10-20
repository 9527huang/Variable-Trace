#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "RtosTimeline.hpp"

namespace
{
	RtosSnapshot snapshotAt(double atSeconds, uint32_t taskCount)
	{
		RtosSnapshot snapshot;
		snapshot.captureStartedAt = atSeconds;

		for (uint32_t index = 0; index < taskCount; index++)
		{
			RtosTask task;
			task.tcbAddress = 0x20008000u + index * 0x40u;
			task.state = index == 0 ? RtosTaskState::Running : RtosTaskState::Ready;
			task.hasCpuShare = index == 0;
			task.cpuSharePercent = index == 0 ? 12.5 : 0.0;

			snapshot.tasks.push_back(task);
		}

		return snapshot;
	}
}  // namespace

TEST(RtosTimeline, KeepsWhatEachRoundSaw)
{
	RtosTimeline timeline(4);

	EXPECT_EQ(timeline.size(), 0u);
	EXPECT_DOUBLE_EQ(timeline.oldestSeconds(), 0.0);
	EXPECT_DOUBLE_EQ(timeline.newestSeconds(), 0.0);

	timeline.add(snapshotAt(10.0, 2));
	timeline.add(snapshotAt(10.5, 2));

	EXPECT_EQ(timeline.size(), 2u);
	EXPECT_DOUBLE_EQ(timeline.oldestSeconds(), 10.0);
	EXPECT_DOUBLE_EQ(timeline.newestSeconds(), 10.5);

	std::vector<RtosTimeline::Sample> samples;
	timeline.copyRange(0.0, 100.0, samples);

	ASSERT_EQ(samples.size(), 2u);
	EXPECT_DOUBLE_EQ(samples[0].atSeconds, 10.0);
	EXPECT_DOUBLE_EQ(samples[1].atSeconds, 10.5);

	ASSERT_EQ(samples[0].entries.size(), 2u);
	EXPECT_EQ(samples[0].entries[0].tcbAddress, 0x20008000u);
	EXPECT_EQ(samples[0].entries[0].state, RtosTaskState::Running);
	EXPECT_TRUE(samples[0].entries[0].hasCpuShare);
	EXPECT_DOUBLE_EQ(samples[0].entries[0].cpuSharePercent, 12.5);

	/* The second task of the round has no share, which is a different thing
	   from a share of zero: the round did not measure it. */
	EXPECT_FALSE(samples[0].entries[1].hasCpuShare);
}

TEST(RtosTimeline, DropsTheOldestSamplesOnceTheRingIsFull)
{
	RtosTimeline timeline(3);

	for (uint32_t index = 0; index < 5; index++)
		timeline.add(snapshotAt(index * 0.5, 1));

	EXPECT_EQ(timeline.size(), 3u);
	EXPECT_DOUBLE_EQ(timeline.oldestSeconds(), 1.0);
	EXPECT_DOUBLE_EQ(timeline.newestSeconds(), 2.0);

	std::vector<RtosTimeline::Sample> samples;
	timeline.copyRange(0.0, 100.0, samples);

	/* Oldest first, and only what is still held. */
	ASSERT_EQ(samples.size(), 3u);
	EXPECT_DOUBLE_EQ(samples[0].atSeconds, 1.0);
	EXPECT_DOUBLE_EQ(samples[1].atSeconds, 1.5);
	EXPECT_DOUBLE_EQ(samples[2].atSeconds, 2.0);
}

TEST(RtosTimeline, CopiesOnlyTheRangeItWasAskedFor)
{
	RtosTimeline timeline(8);

	for (uint32_t index = 0; index < 5; index++)
		timeline.add(snapshotAt(static_cast<double>(index), 1));

	std::vector<RtosTimeline::Sample> samples;
	timeline.copyRange(1.0, 3.0, samples);

	/* Both ends are inside the range: the interface asks for what it is about
	   to draw, and a sample exactly on the edge is drawn. */
	ASSERT_EQ(samples.size(), 3u);
	EXPECT_DOUBLE_EQ(samples.front().atSeconds, 1.0);
	EXPECT_DOUBLE_EQ(samples.back().atSeconds, 3.0);

	timeline.copyRange(10.0, 20.0, samples);
	EXPECT_TRUE(samples.empty());
}

TEST(RtosTimeline, AnEmptyHistoryAnswersWithNothing)
{
	RtosTimeline timeline(4);

	EXPECT_EQ(timeline.size(), 0u);
	EXPECT_DOUBLE_EQ(timeline.oldestSeconds(), 0.0);
	EXPECT_DOUBLE_EQ(timeline.newestSeconds(), 0.0);

	std::vector<RtosTimeline::Sample> samples;
	timeline.copyRange(-100.0, 100.0, samples);

	EXPECT_TRUE(samples.empty());
}

TEST(RtosTimeline, ClearForgetsEverything)
{
	RtosTimeline timeline(4);

	timeline.add(snapshotAt(1.0, 3));
	timeline.add(snapshotAt(2.0, 3));

	ASSERT_EQ(timeline.size(), 2u);

	timeline.clear();

	/* Two runs on one axis would be a picture of neither, so a new session
	   starts from nothing. */
	EXPECT_EQ(timeline.size(), 0u);
	EXPECT_DOUBLE_EQ(timeline.oldestSeconds(), 0.0);

	std::vector<RtosTimeline::Sample> samples;
	timeline.copyRange(-100.0, 100.0, samples);
	EXPECT_TRUE(samples.empty());
}

TEST(RtosTimeline, AFullRingKeepsTheOrderAfterClearingAndRefilling)
{
	RtosTimeline timeline(3);

	for (uint32_t index = 0; index < 4; index++)
		timeline.add(snapshotAt(index * 1.0, 1));

	timeline.clear();

	for (uint32_t index = 0; index < 2; index++)
		timeline.add(snapshotAt(100.0 + index, 1));

	std::vector<RtosTimeline::Sample> samples;
	timeline.copyRange(0.0, 1000.0, samples);

	/* Clearing resets the ring, so the samples that follow are the only ones
	   there and they come back in the order they were added. */
	ASSERT_EQ(samples.size(), 2u);
	EXPECT_DOUBLE_EQ(samples[0].atSeconds, 100.0);
	EXPECT_DOUBLE_EQ(samples[1].atSeconds, 101.0);
}

TEST(RtosTimeline, TheNearestSampleIsTheOneACursorReads)
{
	std::vector<RtosTimeline::Sample> samples;

	for (uint32_t index = 0; index < 3; index++)
	{
		RtosTimeline::Sample sample;
		sample.atSeconds = static_cast<double>(index);

		samples.push_back(sample);
	}

	/* A cursor falls between two rounds, so reading one out means naming the
	   nearest and not the one before it. */
	EXPECT_EQ(RtosTimeline::nearestIndex(samples, 0.4), 0u);
	EXPECT_EQ(RtosTimeline::nearestIndex(samples, 0.6), 1u);
	EXPECT_EQ(RtosTimeline::nearestIndex(samples, 1.4), 1u);
	EXPECT_EQ(RtosTimeline::nearestIndex(samples, 1.6), 2u);

	/* Outside the window at either end, the end sample is the answer. */
	EXPECT_EQ(RtosTimeline::nearestIndex(samples, -5.0), 0u);
	EXPECT_EQ(RtosTimeline::nearestIndex(samples, 99.0), 2u);

	std::vector<RtosTimeline::Sample> none;
	EXPECT_EQ(RtosTimeline::nearestIndex(none, 1.0), 0u);
}

TEST(RtosTimeline, TheWindowFollowsThePlotCursorsWhenThereAreAny)
{
	/* No cursors, or following switched off: the last few seconds. */
	RtosTimeline::Span span = RtosTimeline::windowFrom({}, 10.0, true, 100.0);

	EXPECT_DOUBLE_EQ(span.from, 90.0);
	EXPECT_DOUBLE_EQ(span.to, 100.0);

	span = RtosTimeline::windowFrom({70.0, 72.0}, 10.0, false, 100.0);

	EXPECT_DOUBLE_EQ(span.from, 90.0);
	EXPECT_DOUBLE_EQ(span.to, 100.0);

	/* Two cursors make the window the measurement between them, with a margin,
	   because that is the span the reader drew and wants to look at. */
	span = RtosTimeline::windowFrom({70.0, 80.0}, 10.0, true, 100.0);

	EXPECT_NEAR(span.from, 70.0 - 0.8, 1e-9);
	EXPECT_NEAR(span.to, 80.0 + 0.8, 1e-9);

	/* One cursor centres a window on itself: a cursor is placed at the moment
	   being looked at, which is usually well behind the newest round. */
	span = RtosTimeline::windowFrom({40.0}, 10.0, true, 100.0);

	EXPECT_DOUBLE_EQ(span.from, 35.0);
	EXPECT_DOUBLE_EQ(span.to, 45.0);

	/* Two cursors still sitting where they started are not a span, and a
	   zero-width window would show nothing at all. */
	span = RtosTimeline::windowFrom({0.0, 0.05}, 10.0, true, 100.0);

	EXPECT_DOUBLE_EQ(span.from, -5.0);
	EXPECT_DOUBLE_EQ(span.to, 5.0);
}

/*
 * Samples into bands.
 *
 * The strip used to draw these as separate marks and leave the time between
 * them blank, which is what made a busy target look like a strip of noise.
 */
namespace
{
	RtosTimeline::Sample sampleAt(double at, std::vector<std::pair<uint32_t, RtosTaskState>> entries)
	{
		RtosTimeline::Sample sample;
		sample.atSeconds = at;

		for (const auto& [address, state] : entries)
			sample.entries.push_back({address, state, false, 0.0});

		return sample;
	}
}  // namespace

TEST(RtosTimeline, TheSameStateTwiceBecomesOneInterval)
{
	const std::vector<RtosTimeline::Sample> samples = {
		sampleAt(0.0, {{0x1000, RtosTaskState::Ready}}),
		sampleAt(1.0, {{0x1000, RtosTaskState::Ready}}),
		sampleAt(2.0, {{0x1000, RtosTaskState::Ready}}),
	};

	const std::vector<RtosTimeline::StateInterval> intervals = RtosTimeline::stateIntervals(samples, 3.0);

	ASSERT_EQ(intervals.size(), 1u);
	EXPECT_EQ(intervals[0].tcbAddress, 0x1000u);
	EXPECT_EQ(intervals[0].state, RtosTaskState::Ready);
	EXPECT_DOUBLE_EQ(intervals[0].from, 0.0);
	EXPECT_DOUBLE_EQ(intervals[0].to, 3.0);
}

TEST(RtosTimeline, AChangeOfStateBreaksTheInterval)
{
	const std::vector<RtosTimeline::Sample> samples = {
		sampleAt(0.0, {{0x1000, RtosTaskState::Ready}}),
		sampleAt(1.0, {{0x1000, RtosTaskState::Running}}),
		sampleAt(2.0, {{0x1000, RtosTaskState::Blocked}}),
	};

	const std::vector<RtosTimeline::StateInterval> intervals = RtosTimeline::stateIntervals(samples, 3.0);

	ASSERT_EQ(intervals.size(), 3u);
	EXPECT_DOUBLE_EQ(intervals[0].to, 1.0);
	EXPECT_DOUBLE_EQ(intervals[1].from, 1.0);
	EXPECT_DOUBLE_EQ(intervals[1].to, 2.0);
	EXPECT_DOUBLE_EQ(intervals[2].from, 2.0);
	EXPECT_DOUBLE_EQ(intervals[2].to, 3.0);
}

TEST(RtosTimeline, ARoundThatMissedTheTaskEndsItsIntervalThere)
{
	/* The middle round did not read the task. Missing is not a state, so the
	   band stops where the reading stopped instead of being stretched over a
	   round that never mentioned it. */
	const std::vector<RtosTimeline::Sample> samples = {
		sampleAt(0.0, {{0x1000, RtosTaskState::Ready}}),
		sampleAt(1.0, {}),
		sampleAt(2.0, {{0x1000, RtosTaskState::Ready}}),
	};

	const std::vector<RtosTimeline::StateInterval> intervals = RtosTimeline::stateIntervals(samples, 3.0);

	ASSERT_EQ(intervals.size(), 2u);
	EXPECT_DOUBLE_EQ(intervals[0].from, 0.0);
	EXPECT_DOUBLE_EQ(intervals[0].to, 1.0);
	EXPECT_DOUBLE_EQ(intervals[1].from, 2.0);
	EXPECT_DOUBLE_EQ(intervals[1].to, 3.0);
}

TEST(RtosTimeline, TwoTasksDoNotShareTheirBands)
{
	const std::vector<RtosTimeline::Sample> samples = {
		sampleAt(0.0, {{0x1000, RtosTaskState::Running}, {0x2000, RtosTaskState::Ready}}),
		sampleAt(1.0, {{0x1000, RtosTaskState::Ready}, {0x2000, RtosTaskState::Running}}),
	};

	const std::vector<RtosTimeline::StateInterval> intervals = RtosTimeline::stateIntervals(samples, 2.0);

	ASSERT_EQ(intervals.size(), 4u);
	EXPECT_NE(intervals[0].tcbAddress, intervals[1].tcbAddress);
}

TEST(RtosTimeline, NoSamplesMakeNoIntervals)
{
	const std::vector<RtosTimeline::StateInterval> intervals = RtosTimeline::stateIntervals({}, 5.0);

	EXPECT_TRUE(intervals.empty());
}
