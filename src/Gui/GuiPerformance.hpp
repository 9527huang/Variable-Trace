#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct GuiPerformanceMetrics
{
	uint64_t runId = 0;
	uint64_t measuredFrames = 0;
	uint64_t snapshotCopiedBytes = 0;
	uint64_t rawPointsSubmitted = 0;
	uint64_t displayPointsSubmitted = 0;
	uint64_t decimatedSeries = 0;
	uint64_t decimationCacheMisses = 0;
	uint64_t decimationNs = 0;
	uint64_t variablePlotNs = 0;
	double frameBuildP50Us = 0.0;
	double frameBuildP95Us = 0.0;
	double frameBuildP99Us = 0.0;
	double frameTotalP50Us = 0.0;
	double frameTotalP95Us = 0.0;
	double frameTotalP99Us = 0.0;
};

class GuiPerformanceTracker
{
   public:
	static constexpr std::size_t historyLimit = 2048;

	void beginFrame(uint64_t runId)
	{
		if (activeRunId != runId)
			reset(runId);

		current = {};
	}

	void recordSnapshotCopy(uint64_t bytes) { current.snapshotCopiedBytes += bytes; }

	void recordSeries(uint64_t rawPoints, uint64_t displayPoints, bool decimated)
	{
		current.rawPointsSubmitted += rawPoints;
		current.displayPointsSubmitted += displayPoints;
		if (decimated)
			current.decimatedSeries++;
	}

	void recordDecimation(uint64_t durationNs)
	{
		current.decimationNs += durationNs;
		current.decimationCacheMisses++;
	}

	void recordVariablePlot(uint64_t durationNs) { current.variablePlotNs += durationNs; }

	void endFrame(uint64_t frameBuildNs, uint64_t frameTotalNs)
	{
		current.frameBuildNs = frameBuildNs;
		current.frameTotalNs = frameTotalNs;

		if (history.size() < historyLimit)
			history.push_back(current);
		else
		{
			history[nextHistorySlot] = current;
			nextHistorySlot = (nextHistorySlot + 1) % historyLimit;
		}

		totals.snapshotCopiedBytes += current.snapshotCopiedBytes;
		totals.rawPointsSubmitted += current.rawPointsSubmitted;
		totals.displayPointsSubmitted += current.displayPointsSubmitted;
		totals.decimatedSeries += current.decimatedSeries;
		totals.decimationCacheMisses += current.decimationCacheMisses;
		totals.decimationNs += current.decimationNs;
		totals.variablePlotNs += current.variablePlotNs;
		totals.measuredFrames++;
	}

	GuiPerformanceMetrics getMetrics() const
	{
		GuiPerformanceMetrics result = totals;
		result.runId = activeRunId == std::numeric_limits<uint64_t>::max() ? 0 : activeRunId;

		std::vector<uint64_t> build;
		std::vector<uint64_t> total;
		build.reserve(history.size());
		total.reserve(history.size());
		for (const FrameSample& sample : history)
		{
			build.push_back(sample.frameBuildNs);
			total.push_back(sample.frameTotalNs);
		}

		std::sort(build.begin(), build.end());
		std::sort(total.begin(), total.end());
		result.frameBuildP50Us = percentileUs(build, 0.50);
		result.frameBuildP95Us = percentileUs(build, 0.95);
		result.frameBuildP99Us = percentileUs(build, 0.99);
		result.frameTotalP50Us = percentileUs(total, 0.50);
		result.frameTotalP95Us = percentileUs(total, 0.95);
		result.frameTotalP99Us = percentileUs(total, 0.99);
		return result;
	}

   private:
	struct FrameSample
	{
		uint64_t frameBuildNs = 0;
		uint64_t frameTotalNs = 0;
		uint64_t snapshotCopiedBytes = 0;
		uint64_t rawPointsSubmitted = 0;
		uint64_t displayPointsSubmitted = 0;
		uint64_t decimatedSeries = 0;
		uint64_t decimationCacheMisses = 0;
		uint64_t decimationNs = 0;
		uint64_t variablePlotNs = 0;
	};

	static double percentileUs(const std::vector<uint64_t>& sorted, double percentile)
	{
		if (sorted.empty())
			return 0.0;

		const std::size_t index = static_cast<std::size_t>(percentile * static_cast<double>(sorted.size() - 1));
		return static_cast<double>(sorted[index]) / 1000.0;
	}

	void reset(uint64_t runId)
	{
		activeRunId = runId;
		history.clear();
		nextHistorySlot = 0;
		totals = {};
		totals.runId = runId;
	}

	uint64_t activeRunId = std::numeric_limits<uint64_t>::max();
	std::vector<FrameSample> history;
	std::size_t nextHistorySlot = 0;
	FrameSample current;
	GuiPerformanceMetrics totals;
};
