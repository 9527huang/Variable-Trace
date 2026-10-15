#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DataHandlerBase.hpp"
#include "IDebugProbe.hpp"
#include "MovingAverage.hpp"
#include "PerformanceDiagnosis.hpp"
#include "VariableHandler.hpp"

class ViewerDataHandler : public DataHandlerBase
{
   public:
		/* A lock-free snapshot of the work done by the current acquisition run.
		   Durations are cumulative nanoseconds so callers can calculate their own
		   windows without putting formatting or logging on the sampling thread. */
		struct PerformanceMetrics
		{
			uint64_t runId = 0;
			uint64_t requestedSamples = 0;
			uint64_t completedSamples = 0;
			uint64_t failedSamples = 0;
			uint64_t deadlineMisses = 0;
			uint64_t skippedSamplePeriods = 0;
			uint64_t maxDeadlineLatenessNs = 0;
			uint64_t schedulerSleepNs = 0;
			uint64_t probeCalls = 0;
			uint64_t successfulProbeCalls = 0;
			uint64_t bytesRead = 0;
			uint64_t probeLockWaitNs = 0;
			uint64_t probeReadNs = 0;
			uint64_t modelLockWaitNs = 0;
			uint64_t sharedBufferLockWaitNs = 0;
			uint64_t modelUpdateNs = 0;
			uint64_t csvSubmitNs = 0;
			uint64_t csvFormatNs = 0;
			uint64_t csvWriteNs = 0;
			uint64_t csvMaxWriterLagNs = 0;
			uint64_t csvEnqueuedSamples = 0;
			uint64_t csvWrittenSamples = 0;
			uint64_t csvRejectedSamples = 0;
			uint64_t csvFailedSamples = 0;
			uint64_t csvIoErrors = 0;
			uint64_t csvQueueDepth = 0;
			uint64_t csvQueueHighWaterMark = 0;
			uint64_t csvQueueCapacity = 0;
			uint64_t csvFailureAtUnixMs = 0;
			std::string csvFailureReason;
			std::string csvErrorMessage;
			std::string csvFilePath;
			uint64_t elapsedNs = 0;
			uint32_t sampledVariables = 0;
			uint32_t plannedProbeCallsPerSample = 0;
			uint32_t mergedReadSpans = 0;
			bool mergedReadsEnabled = true;
			bool csvActive = false;
			bool csvOverloaded = false;
			bool csvAsynchronous = true;
		};

	static constexpr uint32_t minSamplinFrequencyHz = 1;
	static constexpr uint32_t maxSamplinFrequencyHz = 1000000;

	/* The viewport is a length of time, so its bounds are times: a millisecond
	   is shorter than the interval between two samples at any rate this reads
	   at, and an hour is longer than any run it is meant to show. */
	static constexpr uint32_t minViewportWidthMs = 1;
	static constexpr uint32_t maxViewportWidthMs = 3600000;

	typedef struct Settings
	{
		uint32_t sampleFrequencyHz = 100;
		uint32_t maxPoints = 10000;

		/* How much time the viewport shows, in milliseconds.

		   A length of time rather than a number of samples: the same window
		   then means the same thing whatever rate the run happens to reach,
		   and changing the sampling frequency does not silently change how
		   much of the signal is on screen. What the window can show is still
		   bounded by the samples that are kept - see Plot::liveTimeWindow. */
		uint32_t viewportWidthMs = 50000;
		bool refreshAddressesOnElfChange = false;
		bool stopAcqusitionOnElfChange = false;
		bool shouldLog = false;
		std::string logFilePath = "";
		std::string gdbCommand = "gdb";

		/* Which program reads the symbol file. A name rather than an object,
		   because the setting is stored in the project file and a project is
		   opened before the parser exists. The names are the ones the parser
		   interface answers to. */
		std::string elfParser = "gdb";
		std::string ofd2000Command = "ofd2000";
		/* P1 safety switch. Disabled uses the original one-read-per-variable
		   path, which also provides an A/B baseline and an emergency fallback. */
		bool mergeContiguousReads = true;
	} Settings;

	ViewerDataHandler(PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, PlotHandler* plotHandler, PlotHandler* tracePlotHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger);
	virtual ~ViewerDataHandler();

	std::string getLastReaderError() const;

	/* The link is open while a run is going, and stays open afterwards when the
	   settings ask for it. */
	bool isProbeHeld() const override;

	/* Writes a value to the target. The per variable range guard lives here
	   rather than in the caller, so that the plot table and the API are checked
	   the same way. Set the limits with Variable::setWriteLimits. */
	bool writeVariable(Variable& var, double value);

	/* Why the last writeVariable() call returned false. Empty when it did not,
	   and when the failure was the probe rather than a limit. */
	std::string getLastWriteError() const;

	/* Asks for the set of addresses read from the target to be built again from
	   the plots of the active group. Changing the group, or what the plots of it
	   draw, while running therefore has to ask for this; the acquisition thread
	   performs the rebuild, which is what keeps the walk over the model and the
	   change to it apart. Ignored while stopped.

	   The rebuild is all that happens for most probes. Only one that is the
	   reader itself - the serial driver, and the debug probes in the streaming
	   mode - is stopped and started again around the new list, because only then
	   does it have to be told. See the note in dataHandler(). */
	void requestRestart();

	IDebugProbe::DebugProbeSettings getProbeSettings() const;
	void setProbeSettings(const IDebugProbe::DebugProbeSettings& settings);

	void setDebugProbe(std::shared_ptr<IDebugProbe> probe);

	/* The open probe, so that a part of the application that moves memory outside
	   the sampling loop - the recorder - can reach the target through the same
	   connection instead of opening a second one. */
	std::shared_ptr<IDebugProbe> getDebugProbe() const
	{
		return debugProbe;
	}

	/* Guards the probe object. The sampling loop holds it around every call into
	   the probe, and a caller that moves memory outside the loop holds it for the
	   duration of its own transactions. Without it two threads would interleave
	   their frames on one connection and neither would receive the answer it
	   asked for. */
	std::unique_lock<std::mutex> lockProbe()
	{
		return std::unique_lock<std::mutex>(probeMutex);
	}

	Settings getSettings() const;
	void setSettings(const Settings& newSettings);

	double getAverageSamplingFrequency() const
	{
		if (averageSamplingPeriod > 0.0)
			return 1.0 / averageSamplingPeriod;
		return 0.0;
	}

		PerformanceMetrics getPerformanceMetrics() const;
		PerformanceDiagnosis getPerformanceDiagnosis(const PerformanceGuiMetrics& gui = {}) const;
		uint64_t getPerformanceRunId() const { return metricsRunId.load(std::memory_order_relaxed); }

   private:
	using SampleListType = std::vector<std::pair<uint32_t, uint8_t>>;
	struct ReadSpan
	{
		uint32_t address = 0;
		uint32_t size = 0;
		bool merged = false;
		SampleListType samples;
		std::vector<uint8_t> buffer;
	};
	using ReadPlanType = std::vector<ReadSpan>;
	struct SampledVariable
	{
		std::shared_ptr<Variable> variable;
		size_t sampleIndex = 0;
	};
	using SampledVariableList = std::vector<SampledVariable>;
	struct ActivePlot
	{
		std::shared_ptr<Plot> plot;
		std::vector<std::shared_ptr<Plot::Series>> visibleSeries;
		bool updateXAxis = false;
	};
	using ActivePlotList = std::vector<ActivePlot>;
	struct CsvColumn
	{
		std::string name;
		std::shared_ptr<Variable> variable;
	};

	void updateVariables(double timestamp, const std::unordered_map<uint32_t, double>& values);
	void updateVariables(double timestamp, const std::vector<double>& values, const std::vector<uint8_t>& valid);
	void dataHandler();
	void prepareCSVFile();
	void resetPerformanceMetrics();
	void buildReadPlan();
	bool readNormalSample(std::vector<double>& rawValues, std::vector<uint8_t>& valid);
	bool readExactSamples(std::vector<double>& rawValues, std::vector<uint8_t>& valid);
	bool readProbe(uint32_t address, uint8_t* buffer, uint32_t size, bool block);

	/* Builds the list of addresses read from the target out of the plots of the
	   active group, and reports whether it came out different from the one in
	   hand. The answer is what says whether anything has to happen at all: a
	   change to the model that leaves this list alone - a variable that no plot
	   draws - costs nothing. */
	bool createSampleList();

   private:
	static constexpr size_t maxVariablesOnSinglePlot = 100;
	std::shared_ptr<IDebugProbe> debugProbe;
	IDebugProbe::DebugProbeSettings probeSettings{};
	std::atomic<bool> restartRequested{false};
	MovingAverage samplingPeriodFilter{1000};
	double averageSamplingPeriod = 0.0;
	Settings settings{};
	std::string lastWriteError;

	/* See lockProbe(). Guards the probe and everything reached through it. */
	std::mutex probeMutex;

	SampleListType sampleList;
	std::unordered_map<uint32_t, size_t> sampleIndexByAddress;
	ReadPlanType readPlan;
	std::vector<double> normalRawValues;
	std::vector<uint8_t> normalRawValid;
	SampledVariableList sampledVariables;
	ActivePlotList activePlots;
	std::vector<CsvColumn> csvColumns;
	std::vector<double> csvValues;
	std::atomic<bool> mergeContiguousReadsEnabled{true};

		std::atomic<uint64_t> metricsRunId{0};
		std::atomic<uint64_t> metricsRequestedSamples{0};
		std::atomic<uint64_t> metricsCompletedSamples{0};
		std::atomic<uint64_t> metricsFailedSamples{0};
		std::atomic<uint64_t> metricsDeadlineMisses{0};
		std::atomic<uint64_t> metricsSkippedSamplePeriods{0};
		std::atomic<uint64_t> metricsMaxDeadlineLatenessNs{0};
		std::atomic<uint64_t> metricsSchedulerSleepNs{0};
		std::atomic<uint64_t> metricsProbeCalls{0};
		std::atomic<uint64_t> metricsSuccessfulProbeCalls{0};
		std::atomic<uint64_t> metricsBytesRead{0};
		std::atomic<uint64_t> metricsProbeLockWaitNs{0};
		std::atomic<uint64_t> metricsProbeReadNs{0};
		std::atomic<uint64_t> metricsModelLockWaitNs{0};
		std::atomic<uint64_t> metricsSharedBufferLockWaitNs{0};
		std::atomic<uint64_t> metricsModelUpdateNs{0};
		std::atomic<uint64_t> metricsCsvSubmitNs{0};
		std::atomic<uint64_t> metricsElapsedNs{0};
		std::atomic<uint32_t> metricsSampledVariables{0};
		std::atomic<uint32_t> metricsPlannedProbeCallsPerSample{0};
		std::atomic<uint32_t> metricsMergedReadSpans{0};
};
