#include "ViewerDataHandler.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "JlinkDebugProbe.hpp"
#include "StlinkDebugProbe.hpp"

#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace
{
	uint64_t nanosecondsBetween(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end)
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
	}

	void keepMaximum(std::atomic<uint64_t>& target, uint64_t candidate)
	{
		uint64_t previous = target.load(std::memory_order_relaxed);

		while (previous < candidate && !target.compare_exchange_weak(previous, candidate, std::memory_order_relaxed))
		{
		}
	}

	std::chrono::steady_clock::duration samplingInterval(uint32_t frequencyHz)
	{
		return std::max(std::chrono::steady_clock::duration{1},
						std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / frequencyHz)));
	}
}

ViewerDataHandler::ViewerDataHandler(PlotGroupHandler* plotGroupHandler, VariableHandler* variableHandler, PlotHandler* plotHandler, PlotHandler* tracePlotHandler, std::atomic<bool>& done, std::mutex* mtx, spdlog::logger* logger) : DataHandlerBase(plotGroupHandler, variableHandler, plotHandler, tracePlotHandler, done, mtx, logger, "viewer")
{
	dataHandle = std::thread(&ViewerDataHandler::dataHandler, this);
}
ViewerDataHandler::~ViewerDataHandler()
{
	stateChangeCondition.notify_all();
	if (dataHandle.joinable())
		dataHandle.join();
}

bool ViewerDataHandler::writeVariable(Variable& var, double value)
{
	lastWriteError.clear();

	/* The guard runs before the lock, because it reads nothing the sampling
	   thread writes. A variable without limits reports as allowed, which is the
	   behaviour the application had before limits existed. */
	if (!var.isWriteAllowed(value))
	{
		lastWriteError = "Value " + std::to_string(value) + " is out of write limits [" +
						 std::to_string(var.getWriteMin()) + ", " + std::to_string(var.getWriteMax()) + "]";
		return false;
	}

	std::lock_guard<std::mutex> lock(*mtx);
	uint32_t rawValue = var.getRawFromDouble(value);
	return debugProbe->writeMemory(var.getAddress(), (uint8_t*)&rawValue, var.getSize());
}

std::string ViewerDataHandler::getLastWriteError() const
{
	return lastWriteError;
}

std::string ViewerDataHandler::getLastReaderError() const
{
	return debugProbe->getLastErrorMsg();
}

bool ViewerDataHandler::isProbeHeld() const
{
	return debugProbe != nullptr && debugProbe->isValid();
}

void ViewerDataHandler::setDebugProbe(std::shared_ptr<IDebugProbe> probe)
{
	debugProbe = probe;
}

IDebugProbe::DebugProbeSettings ViewerDataHandler::getProbeSettings() const
{
	return probeSettings;
}

void ViewerDataHandler::setProbeSettings(const IDebugProbe::DebugProbeSettings& settings)
{
	probeSettings = settings;
}

ViewerDataHandler::Settings ViewerDataHandler::getSettings() const
{
	return settings;
}

void ViewerDataHandler::setSettings(const Settings& newSettings)
{
	settings = newSettings;
	mergeContiguousReadsEnabled.store(settings.mergeContiguousReads, std::memory_order_relaxed);

	/* What is kept here is what the buffers can hold. A capacity that does not
	   fit the memory the buffers are allowed is refused by the plot handler,
	   so both are cut down to it here rather than in each caller - a project
	   file written on a machine with more memory comes in through one of them.
	   The viewport is a length of time and needs only its own bounds: how much
	   of it can actually be shown is decided where the window is worked out,
	   from the samples that are still in hand. */
	settings.maxPoints = std::clamp(settings.maxPoints, static_cast<uint32_t>(1), plotHandler->maxPointsForMemoryBudget());
	settings.viewportWidthMs = std::clamp(settings.viewportWidthMs, minViewportWidthMs, maxViewportWidthMs);

	plotHandler->setMaxPoints(settings.maxPoints);
}

ViewerDataHandler::PerformanceMetrics ViewerDataHandler::getPerformanceMetrics() const
{
	PerformanceMetrics snapshot;
	snapshot.runId = metricsRunId.load(std::memory_order_relaxed);
	snapshot.requestedSamples = metricsRequestedSamples.load(std::memory_order_relaxed);
	snapshot.completedSamples = metricsCompletedSamples.load(std::memory_order_relaxed);
	snapshot.failedSamples = metricsFailedSamples.load(std::memory_order_relaxed);
	snapshot.deadlineMisses = metricsDeadlineMisses.load(std::memory_order_relaxed);
	snapshot.skippedSamplePeriods = metricsSkippedSamplePeriods.load(std::memory_order_relaxed);
	snapshot.maxDeadlineLatenessNs = metricsMaxDeadlineLatenessNs.load(std::memory_order_relaxed);
	snapshot.schedulerSleepNs = metricsSchedulerSleepNs.load(std::memory_order_relaxed);
	snapshot.probeCalls = metricsProbeCalls.load(std::memory_order_relaxed);
	snapshot.successfulProbeCalls = metricsSuccessfulProbeCalls.load(std::memory_order_relaxed);
	snapshot.bytesRead = metricsBytesRead.load(std::memory_order_relaxed);
	snapshot.probeLockWaitNs = metricsProbeLockWaitNs.load(std::memory_order_relaxed);
	snapshot.probeReadNs = metricsProbeReadNs.load(std::memory_order_relaxed);
	snapshot.modelLockWaitNs = metricsModelLockWaitNs.load(std::memory_order_relaxed);
	snapshot.sharedBufferLockWaitNs = metricsSharedBufferLockWaitNs.load(std::memory_order_relaxed);
	snapshot.modelUpdateNs = metricsModelUpdateNs.load(std::memory_order_relaxed);
	const CSVStreamer::Metrics csvMetrics = csvStreamer->getMetrics();
	snapshot.csvSubmitNs = metricsCsvSubmitNs.load(std::memory_order_relaxed);
	snapshot.csvFormatNs = csvMetrics.formatNs;
	snapshot.csvWriteNs = csvMetrics.writeNs;
	snapshot.csvMaxWriterLagNs = csvMetrics.maxWriterLagNs;
	snapshot.csvEnqueuedSamples = csvMetrics.enqueuedSamples;
	snapshot.csvWrittenSamples = csvMetrics.writtenSamples;
	snapshot.csvRejectedSamples = csvMetrics.rejectedSamples;
	snapshot.csvFailedSamples = csvMetrics.failedSamples;
	snapshot.csvIoErrors = csvMetrics.ioErrors;
	snapshot.csvQueueDepth = csvMetrics.queueDepth;
	snapshot.csvQueueHighWaterMark = csvMetrics.queueHighWaterMark;
	snapshot.csvQueueCapacity = csvMetrics.queueCapacity;
	snapshot.csvFailureAtUnixMs = csvMetrics.failureAtUnixMs;
	snapshot.csvFailureReason = CSVStreamer::failureReasonName(csvMetrics.failureReason);
	snapshot.csvErrorMessage = csvMetrics.errorMessage;
	snapshot.csvFilePath = csvMetrics.filePath;
	snapshot.csvActive = csvMetrics.active;
	snapshot.csvOverloaded = csvMetrics.overloaded;
	snapshot.csvAsynchronous = csvMetrics.mode == CSVStreamer::WriteMode::Asynchronous;
	snapshot.elapsedNs = metricsElapsedNs.load(std::memory_order_relaxed);
	snapshot.sampledVariables = metricsSampledVariables.load(std::memory_order_relaxed);
	snapshot.plannedProbeCallsPerSample = metricsPlannedProbeCallsPerSample.load(std::memory_order_relaxed);
	snapshot.mergedReadSpans = metricsMergedReadSpans.load(std::memory_order_relaxed);
	snapshot.mergedReadsEnabled = mergeContiguousReadsEnabled.load(std::memory_order_relaxed);
	return snapshot;
}

PerformanceDiagnosis ViewerDataHandler::getPerformanceDiagnosis(const PerformanceGuiMetrics& gui) const
{
	const PerformanceMetrics metrics = getPerformanceMetrics();
	PerformanceDiagnosisInput input;
	input.requestedFrequencyHz = settings.sampleFrequencyHz;
	input.sampledVariables = metrics.sampledVariables;
	input.plannedProbeCallsPerSample = metrics.plannedProbeCallsPerSample;
	input.completedSamples = metrics.completedSamples;
	input.deadlineMisses = metrics.deadlineMisses;
	input.skippedSamplePeriods = metrics.skippedSamplePeriods;
	input.elapsedNs = metrics.elapsedNs;
	input.probeReadNs = metrics.probeReadNs;
	input.sharedBufferLockWaitNs = metrics.sharedBufferLockWaitNs;
	input.csvQueueDepth = metrics.csvQueueDepth;
	input.csvQueueCapacity = metrics.csvQueueCapacity;
	input.csvActive = metrics.csvActive;
	input.csvFailed = metrics.csvFailureReason != "none";
	input.gui = gui;
	return diagnosePerformance(input);
}

void ViewerDataHandler::resetPerformanceMetrics()
{
	metricsRunId.fetch_add(1, std::memory_order_relaxed);
	metricsRequestedSamples.store(0, std::memory_order_relaxed);
	metricsCompletedSamples.store(0, std::memory_order_relaxed);
	metricsFailedSamples.store(0, std::memory_order_relaxed);
	metricsDeadlineMisses.store(0, std::memory_order_relaxed);
	metricsSkippedSamplePeriods.store(0, std::memory_order_relaxed);
	metricsMaxDeadlineLatenessNs.store(0, std::memory_order_relaxed);
	metricsSchedulerSleepNs.store(0, std::memory_order_relaxed);
	metricsProbeCalls.store(0, std::memory_order_relaxed);
	metricsSuccessfulProbeCalls.store(0, std::memory_order_relaxed);
	metricsBytesRead.store(0, std::memory_order_relaxed);
	metricsProbeLockWaitNs.store(0, std::memory_order_relaxed);
	metricsProbeReadNs.store(0, std::memory_order_relaxed);
	metricsModelLockWaitNs.store(0, std::memory_order_relaxed);
	metricsSharedBufferLockWaitNs.store(0, std::memory_order_relaxed);
	metricsModelUpdateNs.store(0, std::memory_order_relaxed);
	metricsCsvSubmitNs.store(0, std::memory_order_relaxed);
	metricsElapsedNs.store(0, std::memory_order_relaxed);
	metricsSampledVariables.store(static_cast<uint32_t>(sampleList.size()), std::memory_order_relaxed);
	const bool mergedReadsActive = mergeContiguousReadsEnabled.load(std::memory_order_relaxed) && probeSettings.mode == IDebugProbe::Mode::NORMAL && probeSettings.debugProbe != IDebugProbe::Probe::Serial;
	metricsPlannedProbeCallsPerSample.store(probeSettings.mode == IDebugProbe::Mode::HSS ? 1 : static_cast<uint32_t>(mergedReadsActive ? readPlan.size() : sampleList.size()), std::memory_order_relaxed);
	metricsMergedReadSpans.store(mergedReadsActive ? static_cast<uint32_t>(std::count_if(readPlan.begin(), readPlan.end(), [](const ReadSpan& span) { return span.merged; })) : 0, std::memory_order_relaxed);
}

bool ViewerDataHandler::readProbe(uint32_t address, uint8_t* buffer, uint32_t size, bool block)
{
	metricsProbeCalls.fetch_add(1, std::memory_order_relaxed);
	const bool succeeded = block ? debugProbe->readBlock(address, buffer, size) : debugProbe->readMemory(address, buffer, size);

	if (succeeded)
	{
		metricsSuccessfulProbeCalls.fetch_add(1, std::memory_order_relaxed);
		metricsBytesRead.fetch_add(size, std::memory_order_relaxed);
	}

	return succeeded;
}

bool ViewerDataHandler::readExactSamples(std::vector<double>& rawValues, std::vector<uint8_t>& valid)
{
	bool sampleSucceeded = true;
	std::fill(valid.begin(), valid.end(), 0);

	for (const auto& [address, size] : sampleList)
	{
		uint32_t value = 0;

		if (readProbe(address, reinterpret_cast<uint8_t*>(&value), size, false))
		{
			const auto index = sampleIndexByAddress.find(address);
			if (index != sampleIndexByAddress.end())
			{
				rawValues[index->second] = value;
				valid[index->second] = 1;
			}
		}
		else
		{
			sampleSucceeded = false;
			setState(State::STOP);
		}
	}

	return sampleSucceeded;
}

bool ViewerDataHandler::readNormalSample(std::vector<double>& rawValues, std::vector<uint8_t>& valid)
{
	/* Serial already batches its configured sweep in readMemory(). Keep that
	   protocol path unchanged rather than replacing its checked sweep response
	   with buffer-read frames. */
	if (!mergeContiguousReadsEnabled.load(std::memory_order_relaxed) || probeSettings.debugProbe == IDebugProbe::Probe::Serial)
		return readExactSamples(rawValues, valid);

	bool sampleSucceeded = true;
	std::fill(valid.begin(), valid.end(), 0);

	for (ReadSpan& span : readPlan)
	{
		if (!readProbe(span.address, span.buffer.data(), span.size, span.merged))
		{
			sampleSucceeded = false;
			setState(State::STOP);
			continue;
		}

		for (const auto& [address, size] : span.samples)
		{
			uint32_t value = 0;
			std::memcpy(&value, span.buffer.data() + (address - span.address), size);
			const auto index = sampleIndexByAddress.find(address);
			if (index != sampleIndexByAddress.end())
			{
				rawValues[index->second] = value;
				valid[index->second] = 1;
			}
		}
	}

	return sampleSucceeded;
}

void ViewerDataHandler::requestRestart()
{
	restartRequested.store(true);
	stateChangeCondition.notify_all();
}

void ViewerDataHandler::updateVariables(double timestamp, const std::unordered_map<uint32_t, double>& values)
{
	/* The variables and the plots drawn from them are read as one model. The
	   reader can add, delete and rename while this is running, and each of
	   those is two changes to it - a variable and the series that name it -
	   which are made under the same lock this holds. */
	const auto modelLockBegan = std::chrono::steady_clock::now();
	{
		auto modelLock = variableHandler->lockModel();
		metricsModelLockWaitNs.fetch_add(nanosecondsBetween(modelLockBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);

		for (const auto& sampled : sampledVariables)
		{
			const auto value = values.find(sampled.variable->getAddress());
			if (value != values.end())
				sampled.variable->setRawValue(value->second);
		}

		/* Plot and MCP consumers read Variable::value. Convert every bound value
		   after the complete raw sample is present, including when CSV is off. */
		for (const auto& sampled : sampledVariables)
			sampled.variable->transformToDouble();

		for (const auto& activePlot : activePlots)
		{
			const auto sharedLockBegan = std::chrono::steady_clock::now();
			std::lock_guard<std::mutex> lock(*mtx);
			metricsSharedBufferLockWaitNs.fetch_add(nanosecondsBetween(sharedLockBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
			activePlot.plot->updateSeries(activePlot.visibleSeries, activePlot.updateXAxis);
			activePlot.plot->addTimePoint(timestamp);
		}

		if (settings.shouldLog)
			for (size_t index = 0; index < csvColumns.size(); ++index)
				csvValues[index] = csvColumns[index].variable->getValue();
	}

	if (settings.shouldLog)
	{
		const auto csvBegan = std::chrono::steady_clock::now();
		csvStreamer->writeLine(timestamp, csvValues);
		metricsCsvSubmitNs.fetch_add(nanosecondsBetween(csvBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
	}
}

void ViewerDataHandler::updateVariables(double timestamp, const std::vector<double>& values, const std::vector<uint8_t>& valid)
{
	const auto modelLockBegan = std::chrono::steady_clock::now();
	{
		auto modelLock = variableHandler->lockModel();
		metricsModelLockWaitNs.fetch_add(nanosecondsBetween(modelLockBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);

		for (const auto& sampled : sampledVariables)
		{
			if (sampled.sampleIndex < valid.size() && valid[sampled.sampleIndex])
				sampled.variable->setRawValue(static_cast<uint32_t>(values[sampled.sampleIndex]));
		}

		for (const auto& sampled : sampledVariables)
			sampled.variable->transformToDouble();

		for (const auto& activePlot : activePlots)
		{
			const auto sharedLockBegan = std::chrono::steady_clock::now();
			std::lock_guard<std::mutex> lock(*mtx);
			metricsSharedBufferLockWaitNs.fetch_add(nanosecondsBetween(sharedLockBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
			activePlot.plot->updateSeries(activePlot.visibleSeries, activePlot.updateXAxis);
			activePlot.plot->addTimePoint(timestamp);
		}

		if (settings.shouldLog)
			for (size_t index = 0; index < csvColumns.size(); ++index)
				csvValues[index] = csvColumns[index].variable->getValue();
	}

	if (settings.shouldLog)
	{
		const auto csvBegan = std::chrono::steady_clock::now();
		csvStreamer->writeLine(timestamp, csvValues);
		metricsCsvSubmitNs.fetch_add(nanosecondsBetween(csvBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
	}
}

void ViewerDataHandler::dataHandler()
{
	using Clock = std::chrono::steady_clock;
	constexpr auto maximumWaitSlice = std::chrono::milliseconds(50);
	Clock::time_point start;
	Clock::time_point nextDeadline;
	Clock::duration sampleInterval{};
	uint32_t scheduledFrequencyHz = settings.sampleFrequencyHz;
	double lastT = 0.0;

#ifdef _WIN32
	HANDLE schedulerTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (schedulerTimer == nullptr)
		schedulerTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
#endif

	auto waitUntil = [this
#ifdef _WIN32
					  , schedulerTimer
#endif
	](Clock::time_point deadline)
	{
#ifdef _WIN32
		if (schedulerTimer != nullptr)
		{
			const auto remaining = deadline - Clock::now();
			if (remaining <= Clock::duration::zero())
				return;

			using HundredNanoseconds = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
			LARGE_INTEGER dueTime;
			dueTime.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<HundredNanoseconds>(remaining).count());
			if (SetWaitableTimer(schedulerTimer, &dueTime, 0, nullptr, nullptr, FALSE) != 0)
			{
				WaitForSingleObject(schedulerTimer, INFINITE);
				return;
			}
		}
#endif
		std::unique_lock<std::mutex> waitLock(stateChangeMutex);
		stateChangeCondition.wait_until(waitLock, deadline, [this]
											   { return done.load() || viewerState != State::RUN || stateChangeOrdered.load() || restartRequested.load(); });
	};

	while (!done)
	{
		/* setState publishes the requested state before this thread has opened the
		   probe and initialized the schedule. Do not sample during that transition. */
		if (viewerState == State::RUN && !stateChangeOrdered.load())
		{
			if (probeSettings.mode == IDebugProbe::Mode::HSS)
			{
				const double period = std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - start).count();
				std::optional<IDebugProbe::varEntryType> maybeEntry;
				const auto probeLockBegan = Clock::now();

				/* Held only around the probe call, so a caller that is waiting
				   for the connection gets it as soon as this read is done. */
				{
					std::unique_lock<std::mutex> probeLock = lockProbe();
					metricsProbeLockWaitNs.fetch_add(nanosecondsBetween(probeLockBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);

					if (!debugProbe->isValid())
						setState(State::STOP);

					const auto probeReadBegan = std::chrono::steady_clock::now();
					metricsProbeCalls.fetch_add(1, std::memory_order_relaxed);
					maybeEntry = debugProbe->readSingleEntry();
					metricsProbeReadNs.fetch_add(nanosecondsBetween(probeReadBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
				}

				if (!maybeEntry.has_value())
					continue;

				auto [timestamp, rawValues] = maybeEntry.value();

				metricsRequestedSamples.fetch_add(1, std::memory_order_relaxed);
				metricsSuccessfulProbeCalls.fetch_add(1, std::memory_order_relaxed);
				for (const auto& [address, size] : sampleList)
				{
					(void)address;
					metricsBytesRead.fetch_add(size, std::memory_order_relaxed);
				}
				const auto modelUpdateBegan = std::chrono::steady_clock::now();
				updateVariables(timestamp, rawValues);
				metricsModelUpdateNs.fetch_add(nanosecondsBetween(modelUpdateBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
				metricsCompletedSamples.fetch_add(1, std::memory_order_relaxed);
				metricsElapsedNs.store(nanosecondsBetween(start, Clock::now()), std::memory_order_relaxed);

				/* filter sampling frequency */
				averageSamplingPeriod = samplingPeriodFilter.filter((period - lastT));
				lastT = period;
			}

			else
			{
				if (scheduledFrequencyHz != settings.sampleFrequencyHz)
				{
					scheduledFrequencyHz = settings.sampleFrequencyHz;
					sampleInterval = samplingInterval(scheduledFrequencyHz);
					nextDeadline = Clock::now();
				}

				const auto now = Clock::now();
				if (now < nextDeadline)
				{
					const auto sleepBegan = now;
					waitUntil(std::min(nextDeadline, now + maximumWaitSlice));
					metricsSchedulerSleepNs.fetch_add(nanosecondsBetween(sleepBegan, Clock::now()), std::memory_order_relaxed);
				}
				else
				{
					const auto lateness = now - nextDeadline;
					const uint64_t skippedPeriods = static_cast<uint64_t>(lateness / sampleInterval);

					if (skippedPeriods > 0)
					{
						metricsDeadlineMisses.fetch_add(skippedPeriods, std::memory_order_relaxed);
						metricsSkippedSamplePeriods.fetch_add(skippedPeriods, std::memory_order_relaxed);
						keepMaximum(metricsMaxDeadlineLatenessNs, nanosecondsBetween(nextDeadline, now));
						nextDeadline += sampleInterval * static_cast<Clock::duration::rep>(skippedPeriods);
					}

					const double period = std::chrono::duration_cast<std::chrono::duration<double>>(now - start).count();

					metricsRequestedSamples.fetch_add(1, std::memory_order_relaxed);

					/* One sample is a set of reads, and a caller that wants the
					   connection gets it between two samples rather than in the
					   middle of one. */
					bool sampleSucceeded = true;
					const auto probeLockBegan = std::chrono::steady_clock::now();
					{
						std::unique_lock<std::mutex> probeLock = lockProbe();
						metricsProbeLockWaitNs.fetch_add(nanosecondsBetween(probeLockBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
						const auto probeReadBegan = std::chrono::steady_clock::now();

						sampleSucceeded = readNormalSample(normalRawValues, normalRawValid);

						metricsProbeReadNs.fetch_add(nanosecondsBetween(probeReadBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
					}

					if (sampleSucceeded)
					{
						double timestamp = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();
						const auto modelUpdateBegan = std::chrono::steady_clock::now();
						updateVariables(timestamp, normalRawValues, normalRawValid);
						metricsModelUpdateNs.fetch_add(nanosecondsBetween(modelUpdateBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
						metricsCompletedSamples.fetch_add(1, std::memory_order_relaxed);
					}
					else
						metricsFailedSamples.fetch_add(1, std::memory_order_relaxed);

					metricsElapsedNs.store(nanosecondsBetween(start, Clock::now()), std::memory_order_relaxed);

					/* filter sampling frequency */
					averageSamplingPeriod = samplingPeriodFilter.filter((period - lastT));
					lastT = period;
					nextDeadline += sampleInterval;
				}
			}
		}
		else
			std::this_thread::sleep_for(std::chrono::milliseconds(20));

		if (stateChangeOrdered)
		{
			if (viewerState == State::RUN)
			{
				createSampleList();
				prepareCSVFile();

				std::unique_lock<std::mutex> probeLock = lockProbe();

				if (debugProbe->startAcqusition(probeSettings, sampleList, settings.sampleFrequencyHz))
				{
					lastT = 0.0;
					start = Clock::now();
					scheduledFrequencyHz = settings.sampleFrequencyHz;
					sampleInterval = samplingInterval(scheduledFrequencyHz);
					nextDeadline = start;
					resetPerformanceMetrics();
				}
				else
					viewerState = State::STOP;
			}
			else
			{
				std::unique_lock<std::mutex> probeLock = lockProbe();
				debugProbe->stopAcqusition();
				if (settings.shouldLog)
					csvStreamer->finishLogging();
			}
			stateChangeOrdered = false;
		}

		/* A restart builds the list of addresses read from the target again, which
		   is what makes a variable that was just added to a plot, or a newly
		   activated group, take effect.

		   It changes what is read, and the run carries on: the samples already on
		   the plots stay there, and what arrives now joins them at the end of the
		   time axis. Throwing them away is the start button's job, not this
		   one's. */
		if (restartRequested.exchange(false) && viewerState == State::RUN)
		{
			/* Built before anything else is decided, because how it came out is
			   what says whether there is anything to do at all. Importing a
			   variable that no plot draws leaves this list exactly as it was,
			   and it is the common case - a reader picks a symbol and puts it
			   in the table before deciding where to show it. */
			if (!createSampleList())
				continue;

			/* Names are logged by the plot they are drawn on, so a series that
			   has appeared or gone takes the header of the log with it. */
			prepareCSVFile();

			/* The addresses are configured on the probe's side in two cases
			   only: the streaming mode, where the probe builds the sample
			   stream out of the list it was handed, and the serial driver,
			   which sweeps a list this process sends it down the port. Those
			   have to be told again, which means putting them down and picking
			   them up.

			   Every other probe hands back a word at a time on request, and the
			   list those requests are made from is right here, so the next
			   sample reads the new one. Putting such a probe down would cost
			   every sample taken while it was down - a hole in each curve that
			   nothing asked for, and one the reader has no way to tell from a
			   target that stopped answering. */
			const bool theProbeReadsTheListItself = probeSettings.mode == IDebugProbe::Mode::HSS || probeSettings.debugProbe == IDebugProbe::Probe::Serial;

			if (!theProbeReadsTheListItself)
				continue;

			/* Taken before the probe is touched, so that the clock below is
			   moved along by the whole of the pause, the part spent starting it
			   again included. */
			const auto restartBegan = std::chrono::steady_clock::now();

			std::unique_lock<std::mutex> probeLock = lockProbe();
			debugProbe->stopAcqusition();

			const bool started = debugProbe->startAcqusition(probeSettings, sampleList, settings.sampleFrequencyHz);

			/* The clock keeps its place in the run. It is moved forward by the
			   time the restart takes, so that the stretch the target was not
			   read for does not show up in the history as a gap.

			   Putting it back to the beginning of the run instead would append
			   everything read from here on at the start of the time axis, and
			   since the samples of the run are still there, the curve would
			   fold back over the part already drawn.

			   The sample counter and the previous period are left where they
			   are along with it. Both of them measure the clock, so both still
			   agree with it; setting them back while the clock reads minutes
			   would let the rate limit pass every sample until they caught up
			   again, and the target would be read as fast as the probe allows
			   for that stretch. */
			start += std::chrono::steady_clock::now() - restartBegan;

			/* Same handling as a failing start from the state transition above. */
			if (!started)
				viewerState = State::STOP;
		}
	}

#ifdef _WIN32
	if (schedulerTimer != nullptr)
		CloseHandle(schedulerTimer);
#endif
}

bool ViewerDataHandler::createSampleList()
{
	/* Walks the plots and the variables, so it takes the lock the interface
	   holds while it changes either of them. A variable added to a plot while
	   this is running is reached through requestRestart(), which runs this
	   again on this same thread. */
	auto modelLock = variableHandler->lockModel();

	/* Built beside the list in hand rather than over it, so that the two can be
	   compared before the one replaces the other. The answer is what the caller
	   decides on, and it has to be about the contents: the list carries the
	   addresses and the sizes of them, so a variable that keeps its address and
	   comes back a wider type counts as a change, as it should. */
	SampleListType wanted;

	auto checkIfElementExists = [&](std::pair<uint32_t, uint8_t> newElement)
	{
		return std::find_if(wanted.begin(), wanted.end(), [&newElement](const std::pair<uint32_t, uint8_t>& element)
							{ return element == newElement; }) != wanted.end();
	};

	for (auto& [name, plotElem] : *plotGroupHandler->getActiveGroup())
	{
		auto plot = plotElem.plot;

		if (!plotElem.visibility)
			continue;

		for (auto& [name, ser] : plot->getSeriesMap())
		{
			if (!ser->visible)
				continue;

			std::pair<uint32_t, uint8_t> newElement = {ser->var->getAddress(), ser->var->getSize()};

			if (!checkIfElementExists(newElement))
				wanted.push_back(newElement);

			Variable* maybeXAxisVariable = plot->getXAxisVariable();
			if (plot->getType() == Plot::Type::XY && maybeXAxisVariable != nullptr)
			{
				newElement = {maybeXAxisVariable->getAddress(), maybeXAxisVariable->getSize()};
				if (!checkIfElementExists(newElement))
					wanted.push_back(newElement);
			}
		}
	}

	/* additionally scan for eventual bases of fractional variables that should be sampled */
	for (auto variable : *variableHandler)
	{
		if (variable->getFractional().baseVariable != nullptr)
		{
			auto var = variable->getFractional().baseVariable;
			std::pair<uint32_t, uint8_t> newElement = {var->getAddress(), var->getSize()};

			if (!checkIfElementExists(newElement))
				wanted.push_back(newElement);
		}
	}

	const bool changed = wanted != sampleList;

	sampleList = std::move(wanted);
	sampleIndexByAddress.clear();
	sampleIndexByAddress.reserve(sampleList.size());
	for (size_t index = 0; index < sampleList.size(); ++index)
		sampleIndexByAddress.emplace(sampleList[index].first, index);
	normalRawValues.resize(sampleList.size());
	normalRawValid.resize(sampleList.size());

	/* Bind the model work to the plan while the model lock is held. The shared
	   owners keep the objects alive until the next rebuild, and updateVariables
	   still takes this lock before using them so a concurrent edit cannot race
	   with the bound view. */
	sampledVariables.clear();
	for (const auto& variable : *variableHandler)
	{
		const auto index = sampleIndexByAddress.find(variable->getAddress());
		if (index != sampleIndexByAddress.end())
			sampledVariables.push_back({variable, index->second});
	}

	activePlots.clear();
	csvColumns.clear();
	for (const auto& [name, plotElem] : *plotGroupHandler->getActiveGroup())
	{
		if (!plotElem.visibility || plotElem.plot == nullptr)
			continue;

		ActivePlot activePlot;
		activePlot.plot = plotElem.plot;
		activePlot.updateXAxis = plotElem.plot->getType() == Plot::Type::XY && plotElem.plot->getXAxisVariable() != nullptr;
		for (const auto& [seriesName, series] : plotElem.plot->getSeriesMap())
		{
			if (!series->visible || series->var == nullptr)
				continue;

			activePlot.visibleSeries.push_back(series);
			const auto sampled = std::find_if(sampledVariables.begin(), sampledVariables.end(), [&](const auto& bound)
												 { return bound.variable.get() == series->var; });
			if (sampled != sampledVariables.end())
				csvColumns.push_back({seriesName, sampled->variable});
		}
		activePlots.push_back(std::move(activePlot));
	}
	csvValues.resize(csvColumns.size());

	buildReadPlan();
	metricsSampledVariables.store(static_cast<uint32_t>(sampleList.size()), std::memory_order_relaxed);

	/* mark actively sampled variables */
	for (auto variable : *variableHandler)
	{
		variable->setIsCurrentlySampled(false);
		if (std::find(sampleList.begin(), sampleList.end(), std::pair<uint32_t, uint8_t>(variable->getAddress(), variable->getSize())) != sampleList.end())
			variable->setIsCurrentlySampled(true);
	}

	return changed;
}

void ViewerDataHandler::buildReadPlan()
{
	readPlan.clear();

	SampleListType knownRam;
	SampleListType exactReads;

	for (const auto& sample : sampleList)
	{
		const uint64_t end = static_cast<uint64_t>(sample.first) + sample.second;
		/* ARM's 0x2/0x3 system regions are SRAM. Without a chip memory map this
		   is the only range known strongly enough to merge; peripheral and other
		   architecture addresses retain exact reads. */
		if (sample.first >= 0x20000000u && end <= 0x40000000ull)
			knownRam.push_back(sample);
		else
			exactReads.push_back(sample);
	}

	std::stable_sort(knownRam.begin(), knownRam.end(), [](const auto& left, const auto& right) { return left.first < right.first; });

	for (const auto& sample : knownRam)
	{
		const uint32_t sampleEnd = sample.first + sample.second;

		if (readPlan.empty() || sample.first > readPlan.back().address + readPlan.back().size)
		{
			ReadSpan span;
			span.address = sample.first;
			span.size = sample.second;
			span.samples.push_back(sample);
			readPlan.push_back(std::move(span));
			continue;
		}

		ReadSpan& span = readPlan.back();
		span.size = std::max(span.address + span.size, sampleEnd) - span.address;
		span.samples.push_back(sample);
	}

	for (const auto& sample : exactReads)
	{
		ReadSpan span;
		span.address = sample.first;
		span.size = sample.second;
		span.samples.push_back(sample);
		readPlan.push_back(std::move(span));
	}

	uint32_t mergedSpans = 0;
	/* ST-Link's mem32 block transport requires aligned addresses and lengths.
	   An unaligned span keeps the original reads rather than changing their
	   byte selection or reading beyond the declared variables. */
	if (probeSettings.debugProbe == IDebugProbe::Probe::Stlink)
	{
		ReadPlanType compatiblePlan;
		for (ReadSpan& span : readPlan)
		{
			if (span.samples.size() > 1 && (span.address % 4 != 0 || span.size % 4 != 0))
			{
				for (const auto& sample : span.samples)
				{
					ReadSpan exact;
					exact.address = sample.first;
					exact.size = sample.second;
					exact.samples.push_back(sample);
					compatiblePlan.push_back(std::move(exact));
				}
			}
			else
				compatiblePlan.push_back(std::move(span));
		}
		readPlan = std::move(compatiblePlan);
	}

	for (ReadSpan& span : readPlan)
	{
		span.merged = span.samples.size() > 1;
		/* Some legacy readMemory implementations write through a uint32_t even
		   for one- and two-byte values, so exact-read storage keeps four bytes. */
		span.buffer.resize(std::max<uint32_t>(span.size, sizeof(uint32_t)));
		if (span.merged)
			mergedSpans++;
	}

	const bool enabled = mergeContiguousReadsEnabled.load(std::memory_order_relaxed) && probeSettings.mode == IDebugProbe::Mode::NORMAL && probeSettings.debugProbe != IDebugProbe::Probe::Serial;
	metricsPlannedProbeCallsPerSample.store(probeSettings.mode == IDebugProbe::Mode::HSS ? 1 : static_cast<uint32_t>(enabled ? readPlan.size() : sampleList.size()), std::memory_order_relaxed);
	metricsMergedReadSpans.store(enabled ? mergedSpans : 0, std::memory_order_relaxed);
}

void ViewerDataHandler::prepareCSVFile()
{
	if (!settings.shouldLog)
		return;

	std::vector<std::string> headerNames;
	headerNames.reserve(csvColumns.size());
	for (const auto& column : csvColumns)
		headerNames.push_back(column.name);
	csvStreamer->prepareFile(settings.logFilePath);
	csvStreamer->createHeader(headerNames);
}
