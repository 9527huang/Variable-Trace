#include <gtest/gtest.h>

#include "GuiPerformance.hpp"

TEST(GuiPerformanceTrackerTest, ReportsFramePercentilesAndWorkCounters)
{
	GuiPerformanceTracker tracker;
	for (uint64_t frame = 1; frame <= 4; ++frame)
	{
		tracker.beginFrame(7);
		tracker.recordSnapshotCopy(frame * 10);
		tracker.recordSeries(100, 20, true);
		tracker.recordDecimation(frame * 100);
		tracker.recordVariablePlot(frame * 200);
		tracker.endFrame(frame * 1000, frame * 2000);
	}

	const GuiPerformanceMetrics metrics = tracker.getMetrics();
	EXPECT_EQ(metrics.runId, 7u);
	EXPECT_EQ(metrics.measuredFrames, 4u);
	EXPECT_EQ(metrics.snapshotCopiedBytes, 100u);
	EXPECT_EQ(metrics.rawPointsSubmitted, 400u);
	EXPECT_EQ(metrics.displayPointsSubmitted, 80u);
	EXPECT_EQ(metrics.decimatedSeries, 4u);
	EXPECT_EQ(metrics.decimationCacheMisses, 4u);
	EXPECT_EQ(metrics.decimationNs, 1000u);
	EXPECT_EQ(metrics.variablePlotNs, 2000u);
	EXPECT_DOUBLE_EQ(metrics.frameBuildP50Us, 2.0);
	EXPECT_DOUBLE_EQ(metrics.frameBuildP95Us, 3.0);
	EXPECT_DOUBLE_EQ(metrics.frameBuildP99Us, 3.0);
	EXPECT_DOUBLE_EQ(metrics.frameTotalP50Us, 4.0);
}

TEST(GuiPerformanceTrackerTest, StartsAFreshWindowForANewAcquisitionRun)
{
	GuiPerformanceTracker tracker;
	tracker.beginFrame(10);
	tracker.recordSnapshotCopy(1000);
	tracker.endFrame(1000, 2000);

	tracker.beginFrame(11);
	tracker.endFrame(3000, 4000);

	const GuiPerformanceMetrics metrics = tracker.getMetrics();
	EXPECT_EQ(metrics.runId, 11u);
	EXPECT_EQ(metrics.measuredFrames, 1u);
	EXPECT_EQ(metrics.snapshotCopiedBytes, 0u);
	EXPECT_DOUBLE_EQ(metrics.frameBuildP50Us, 3.0);
}
