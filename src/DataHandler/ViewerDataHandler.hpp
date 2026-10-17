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

#include <chrono>
#include <functional>

#include "RtosModel.hpp"
#include "RtosCpuLoad.hpp"
#include "RtosSampler.hpp"
#include "RtosStackSweep.hpp"
#include "MovingAverage.hpp"
#include "PerformanceDiagnosis.hpp"
#include "VariableHandler.hpp"

class RtosProbeMemory;

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

	/*
	 * The task view.
	 *
	 * The symbol table and the symbol file's path come from the interface
	 * side. Reading an ELF file is that side's job, and this handler has no
	 * business owning a parser of its own just to reach the kernel's globals.
	 *
	 * The path is an output because the offsets need it as well: they are read
	 * from the same debug information the symbols are, over a gdb session this
	 * side opens with the command from its own settings.
	 */
	using RtosSymbolProvider = std::function<bool(IElfParser::SymbolMap& symbols, std::string& elfPath)>;

	void setRtosSymbolProvider(RtosSymbolProvider provider)
	{
		rtosSymbolProvider = std::move(provider);
	}

	/*
	 * Resolves the kernel's symbols and its field offsets.
	 *
	 * Called before acquisition starts, because the first call may run a
	 * symbol file parser and that is not something to do inside the sampling
	 * loop. Answers false when there is no provider, no profile, or the symbol
	 * file does not carry what the profile needs; a false answer costs the
	 * task view and nothing else, since the loop does no work without a
	 * prepared sampler.
	 */
	bool prepareRtos();

	/* The published snapshot and what it took to get there. The interface and
	   the API read this; neither reaches into the sampler. */
	std::shared_ptr<RtosModel> getRtosModel() const
	{
		return rtosModel;
	}

	/*
	 * The switch. Off means no sampler is built and the sampling loop does
	 * nothing beyond one atomic read per round.
	 *
	 * Turning it on does not prepare anything by itself. Preparation reads a
	 * symbol file, which can take seconds, and it needs the project to be
	 * loaded for the file path to mean anything; so the loop does it once, on
	 * its first pass after the switch went on. Answers whether the task view
	 * is switched on after the call.
	 */
	/* Switches the task view on or off. Returns whether it is on after the
	   call. */
	bool setRtosEnabled(bool enabled);

	/* Drops what the task view prepared, so that the loop prepares again on
	   its next pass. Called when what it prepared against has changed - the
	   project, or the symbol file behind it - and not when the switch is set
	   to the value it already has. */
	void forgetRtosPreparation();

	/* Whether the task view is switched on, and why it has nothing to show.
	 *
	 * The reason is kept rather than only logged. A task view that is enabled
	 * and silent looks the same from the outside whether the symbol file has
	 * no kernel in it, the probe is not open, or the application was built
	 * without the offsets this kernel needs - and the person looking at an
	 * empty panel cannot tell which one to go and fix. */
	bool isRtosEnabled() const
	{
		return rtosEnabled.load(std::memory_order_relaxed);
	}

	bool isRtosPrepared() const
	{
		return rtosPrepared.load(std::memory_order_relaxed);
	}

	std::string getRtosPrepareError() const
	{
		std::lock_guard<std::mutex> guard(rtosPrepareErrorMutex);

		return rtosPrepareError;
	}

	std::string getRtosLastRejection() const
	{
		std::lock_guard<std::mutex> guard(rtosPrepareErrorMutex);

		return rtosLastRejection;
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

	/* True when a round of the task view is due and there is room for it
	   before the next variable sample falls due.

	   The ordering rule comes from what a missed round costs. A variable
	   sample that is missed leaves a gap in a curve that the next round fills
	   in; a task snapshot that is missed shows a state a little later than it
	   could have. Neither is worth pushing the other out of the way, so the
	   snapshot goes last and gives way. */
	bool rtosRoundIsDue(double now, std::chrono::steady_clock::time_point nextDeadline, std::chrono::steady_clock::duration interval) const;

	/* Runs one round and hands the result to the model. The probe lock is held
	   by the caller, which is why this takes the time base rather than
	   reaching for the probe itself. */
	void runRtosRound(std::chrono::steady_clock::time_point start);

	/* One batch of switch records, when the target keeps them. */
	void readEventRing(RtosProbeMemory& memory, double nowSeconds);

	/* The stream the probe pushes, when the target keeps an RTT buffer. Read on
	   the sampling tick rather than on the task view's own: a stream has to be
	   drained at the rate it is written, and the task view's round is far too
	   slow for that. */
	void pollEventStream(double nowSeconds);

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

	/* The task view. Both objects are built by prepareRtos() and torn down by
	   setRtosEnabled(false), so the sampling loop only has to test the sampler
	   for emptiness. */
	std::shared_ptr<RtosModel> rtosModel;
	std::shared_ptr<RtosSampler> rtosSampler;
	RtosSymbolProvider rtosSymbolProvider;
	RtosSampler::Settings rtosSettings{};

	/* The stack water marks are not part of a round: a round is a function of
	   the target and the settings, while these are read a few tasks at a time
	   across several rounds. The sweep owns the turn and the readings. */
	RtosStackSweep rtosStackSweep;
	RtosStackSweep::Settings rtosStackSweepSettings{};

	/* The shares need two rounds of counters, so they are worked out here
	   rather than in a single round. Nothing extra is read for them: the
	   counter comes with the control block. */
	RtosCpuLoad rtosCpuLoad;
	RtosCpuLoad::Settings rtosCpuLoadSettings{};

	/* Path B: where the target keeps its switch records, zero when it does not.
	   Optional, because a firmware without the ring is a firmware the task view
	   still works against. */
	uint32_t rtosEventRingAddress = 0;

	/* How far the reader has got, in the ring's own monotonic count. */
	uint32_t rtosEventsRead = 0;

	/* The same moment said twice: the newest record's target microseconds and
	   the host's seconds when it was read. Drift between the two crystals is
	   corrected slowly, not jumped at. */
	uint32_t rtosEventAnchorUs = 0;
	double rtosEventAnchorSeconds = 0.0;

	/*
	 * How many records a round reads, and how much history the strip keeps.
	 *
	 * The budget is a bandwidth decision, not a storage one. At the default
	 * round interval this many records is about 1.5 KiB, which the probe moves
	 * in a couple of milliseconds and which leaves the strip dense enough to
	 * see a pattern; a hundred and ninety-two switches in half a second is what
	 * a busy target makes, so the count of records passed over stays small
	 * enough to be worth printing rather than a number that stops meaning
	 * anything.
	 */
	/* Set once the probe has found the target's RTT buffer. The ring is not
	   read while this is true: the same records would arrive twice. */
	bool rttActive = false;

	/* Bytes of a record the target was still writing when the read came back. */
	std::vector<uint8_t> rttCarry;

	/* The same anchor the ring path keeps, for the same reason. */
	uint32_t rttAnchorUs = 0;
	double rttAnchorSeconds = 0.0;

	static constexpr uint32_t rtosEventsPerRound = 192;
	static constexpr double rtosEventsKeepSeconds = 600.0;
	std::atomic<bool> rtosEnabled{false};

	/* Set by the sampling loop on the pass where it prepared the sampler.
	   Cleared whenever the switch changes, so a project opened afterwards gets
	   its own symbol file read rather than the previous one reused. */
	std::atomic<bool> rtosPrepared{false};

	/* When to try preparing again after a failed attempt.
	 *
	 * The first pass usually fails: the loop starts before a project is open,
	 * so there is no file path yet, and it succeeds a moment later once one
	 * is. Retrying on every pass would be fine for that and ruinous for a
	 * genuine failure - a symbol file that is not there would be retried at
	 * the sampling rate, and the log would fill with the same line. A pause
	 * between attempts costs nothing when the next one succeeds and keeps the
	 * log readable when none of them do. */
	std::chrono::steady_clock::time_point rtosNextPrepareAttempt{};

	/*
	 * The variables that carry synchronisation depths into the plot area.
	 *
	 * A derived series needs somewhere for its value to live that cursor
	 * readings, statistics and export can all read, and a variable is that
	 * place - which is why they are variables at all rather than a second kind
	 * of series. They are not addresses on the target: they are created here,
	 * written here, and the sampling loop is told to leave them out of its
	 * read plan.
	 *
	 * Kept under a mutex of this object's own rather than the model's. This is
	 * the handler's bookkeeping about which variables it made; the model's
	 * lock is for the variables themselves. Holding one while taking the other
	 * is how two locks end up nested, and the plan forbids that outright.
	 */
	std::map<std::string, std::shared_ptr<Variable>> syncDepthVariables;
	std::mutex syncDepthVariablesMutex;

	/* The last reason preparation did not produce a sampler, and the last
	   reason a round was refused. Empty means there is nothing to report. */
	std::string rtosPrepareError;
	std::string rtosLastRejection;
	mutable std::mutex rtosPrepareErrorMutex;

	/* Writes the depths of the round's synchronisation objects into those
	   variables, making the ones that are new. Called after a round is
	   published, on the sampling thread. */
	void publishSyncDepths(const RtosSnapshot& snapshot);

	/* Seconds on the sampling loop's own clock, which is what the snapshot's
	   capture times are measured against. */
	double rtosLastRoundAt = -1.0;
	std::string rtosSessionId;

	/*
	 * How much wall clock to leave between two rounds.
	 *
	 * Half a second is the plan's default for a scan that reads whole control
	 * blocks. It lives here rather than in the sampler's settings because it
	 * is not a property of the kernel: the sampler is asked for one round and
	 * does not know how often it gets asked.
	 */
	double rtosIntervalSeconds = 0.5;

	/* How long to wait after a failed preparation before trying again. */
	static constexpr double rtosPrepareRetrySeconds = 5.0;

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
