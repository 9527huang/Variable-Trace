#include "ViewerDataHandler.hpp"

#include "GdbRtosLayout.hpp"
#include "RtosFactory.hpp"
#include "RtosProbeMemory.hpp"

#include <memory>

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
	/* A restart rebuilds the address table, and that is asked for when the
	   symbol file changed or the set of variables did. The task view prepared
	   against that same file, so its preparation is dropped here and the loop
	   makes a new one on its next pass. */
	forgetRtosPreparation();

	restartRequested.store(true);
	stateChangeCondition.notify_all();
}

bool ViewerDataHandler::prepareRtos()
{
	rtosSampler.reset();

	auto note = [this](const std::string& reason)
	{
		std::lock_guard<std::mutex> guard(rtosPrepareErrorMutex);
		rtosPrepareError = reason;
	};

	auto report = [this, &note](const std::string& reason)
	{
		note(reason);

		return false;
	};

	if (!rtosEnabled.load(std::memory_order_relaxed))
		return report("the task view is switched off");

	/* Every way out of here says what it was, here and in the log.
	   Preparation happens once, on a pass of the sampling loop, and the log
	   is only written when the program exits - so a reason that lives only
	   there is a reason nobody sees while the empty panel is on screen. */
	if (rtosSymbolProvider == nullptr)
	{
		logger->warn("The task view is switched on but no symbol provider was set, so it stays off for this run.");

		return report("no symbol provider was set");
	}

	IElfParser::SymbolMap symbols;
	std::string elfPath;

	/* The provider may run a symbol file parser here, which takes seconds on
	   a large file. That is why this is called before acquisition rather than
	   from inside the sampling loop. */
	if (!rtosSymbolProvider(symbols, elfPath))
	{
		/* Debug rather than a warning: the usual reason is that no project is
		   open yet, and the loop tries again in a few seconds. The reason is
		   recorded either way, so that a genuine failure - a symbol file with
		   no kernel in it - can still be read off the interface. */
		logger->debug("The task view could not read the symbol file yet; it will try again.");

		return report("the symbol file could not be read, or holds none of the kernel's symbols");
	}

	if (rtosModel == nullptr)
		rtosModel = std::make_shared<RtosModel>();

	const std::shared_ptr<RtosProfileBase> profile = RtosFactory::create(RtosProfileBase::Type::FreeRtos);

	auto sampler = std::make_shared<RtosSampler>(profile);

	/*
	 * The offsets belong to the build rather than to the kernel version:
	 * turning on the list integrity checks moves every offset behind the first
	 * item, and a table of written down numbers would then read the wrong
	 * bytes and report them as data. They come out of the same symbol file the
	 * addresses did, over a gdb session opened with the command this handler
	 * already keeps for the variable side.
	 *
	 * Whatever the debug information does not describe keeps the profile's own
	 * number, which the resolution marks as a fallback so that the interface
	 * can say which of the two answered.
	 */
	const RtosLayoutTable measured = GdbRtosLayout::read(getSettings().gdbCommand, elfPath, *profile, logger);

	logger->info("The kernel layout: {} structures and {} fields read from debug information.", measured.structCount(), measured.fieldCount());

	if (!sampler->prepare(symbols, measured, rtosSettings))
	{
		const std::string reason = sampler->lastError();

		logger->warn("The task view could not prepare against {} symbols: {}", symbols.size(), reason);

		return report("the sampler could not be prepared: " + reason);
	}

	logger->info("The task view is ready: {} symbols resolved, {} lists to walk.", symbols.size(), sampler->listSources().size());

	/*
	 * Path B, if this firmware has it.
	 *
	 * Optional on purpose: the task view works on samples alone, and a target
	 * that carries no ring is a target it still has something to say about. One
	 * symbol is all that is looked for - the ring is one object, so that the
	 * linker keeps the capacity that only the host reads.
	 */
	rtosEventRingAddress = 0;
	rtosEventsRead = 0;

	if (const auto ring = symbols.find("vtrace_events"); ring != symbols.end())
	{
		rtosEventRingAddress = ring->second.address;

		logger->info("Switch records are kept by the target, at 0x{:08X}.", rtosEventRingAddress);
	}

	/* Every snapshot carries the identity of the run it came from, so a saved
	   record can be told apart from the next one. The wall clock is the only
	   thing at hand that is unique to this run. */
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	rtosSessionId = "session-" + std::to_string(seconds);

	rtosModel->beginSession(rtosSessionId);

	/* A new session starts with no readings. A water mark taken against the
	   previous target would be a number about a stack that is not the one on
	   screen any more, and a difference between two counters would be taken
	   across two kernels. */
	rtosStackSweep.forget();
	rtosCpuLoad.forget();
	rtosLastRoundAt = -1.0;
	rtosSampler = std::move(sampler);

	/* Cleared, so that a reason left over from a failed attempt does not sit
	   beside a sampler that is working. */
	note("");

	return true;
}

void ViewerDataHandler::publishSyncDepths(const RtosSnapshot& snapshot)
{
	/*
	 * The name a depth is carried under.
	 *
	 * Written in the bracket form a variable uses for an array element, so
	 * that a depth can never collide with a structure member or with something
	 * imported: `q_sensor[depth]` is not a name a target could have, while
	 * `q_sensor.depth` is the shape of a member and could be.
	 */
	auto nameOf = [](const RtosSyncObject& object) { return object.name + "[depth]"; };

	std::vector<std::pair<std::shared_ptr<Variable>, uint32_t>> toWrite;
	std::vector<std::pair<std::string, uint32_t>> toCreate;

	/*
	 * Step one: decide what has to be written and what has to be made.
	 *
	 * No lock is needed for the snapshot itself, because a published snapshot
	 * never changes again. The model's lock is taken only to look things up,
	 * and it is released before anything is created: `addVariable` takes that
	 * same lock itself, and calling it while holding one is a deadlock rather
	 * than a slow path.
	 */
	{
		auto modelLock = variableHandler->lockModel();

		for (const RtosSyncObject& object : snapshot.syncObjects)
		{
			/* An object with no name cannot be told apart from the next
			   unnamed one, and one whose depth was not read has no value to
			   draw. Both are left alone rather than given a made up name. */
			if (object.name.empty() || !object.hasCounts)
				continue;

			const std::string name = nameOf(object);

			std::shared_ptr<Variable> variable;

			{
				std::lock_guard<std::mutex> guard(syncDepthVariablesMutex);

				const auto it = syncDepthVariables.find(name);

				if (it != syncDepthVariables.end())
					variable = it->second;
			}

			if (variable != nullptr)
			{
				toWrite.push_back({variable, object.items});

				continue;
			}

			/* A variable of that name already in the model did not come from
			   here, so it belongs to someone else and is left untouched. */
			if (variableHandler->contains(name))
				continue;

			toCreate.push_back({name, object.items});
		}
	}

	/* Step two: make the new ones. `addVariable` takes the model's lock on its
	   own, so nothing may be held while this runs. */
	for (const auto& [name, depth] : toCreate)
	{
		auto variable = std::make_shared<Variable>(name, Variable::Type::U32, 0.0);

		/* Marked before it goes into the model, so that every path which adds
		   a series over it - the interface, a project file, a tool call - will
		   see it for what it is and keep it out of the read plan. */
		variable->setDerived(true);

		/*
		 * And told not to take its address from a symbol file.
		 *
		 * That flag is what `getIsFound` answers with, and everything that
		 * asks - the variable table, the write path, the interface's Ctrl
		 * click - is really asking whether the variable can be used. A
		 * derived variable can be used; it simply has no address to look up.
		 * Leaving the flag on had the table report `NOT FOUND!` in red for a
		 * value that was there all along, and had Ctrl click refuse to select
		 * it, which is the one gesture that puts it on a plot.
		 */
		variable->setShouldUpdateFromElf(false);

		variableHandler->addVariable(variable);

		{
			std::lock_guard<std::mutex> guard(syncDepthVariablesMutex);
			syncDepthVariables[name] = variable;
		}

		/* Written on the same pass that made it, so that the first round a
		   depth exists it already shows the value it was read with rather than
		   a zero that means nothing. */
		toWrite.push_back({variable, depth});
	}

	if (toCreate.empty() && toWrite.empty())
		return;

	/* Step three: write, under the model's lock and touching nothing else. */
	auto modelLock = variableHandler->lockModel();

	for (const auto& [variable, value] : toWrite)
	{
		/*
		 * Both flags are set every round rather than only when the variable is
		 * made. One made before these rules existed would otherwise keep the
		 * old answers, and the table would go on colouring it as an address
		 * that was not found - which is what a reader sees first.
		 */
		variable->setDerived(true);
		variable->setShouldUpdateFromElf(false);

		/*
		 * Both of them, and the reason is worth writing down.
		 *
		 * A variable carries a raw value and a displayed one, and the sampling
		 * loop is what turns the first into the second for every variable it
		 * reads. A derived variable is not one of those, so nothing would ever
		 * make that turn for it: setting only the raw value would leave the
		 * number the interface shows - and the statistics measure, and the
		 * export writes - at its initial zero for good.
		 */
		variable->setRawValue(value);
		variable->setValue(static_cast<double>(value));
	}
}

bool ViewerDataHandler::setRtosEnabled(bool enabled)
{
	const bool wasEnabled = rtosEnabled.load(std::memory_order_relaxed);

	rtosEnabled.store(enabled, std::memory_order_relaxed);

	/*
	 * Setting it to what it already is does nothing.
	 *
	 * The interface calls this every time the start button is pressed, and
	 * the value almost never changes. Dropping what was prepared on every
	 * press would have the loop read the symbol file and open two gdb
	 * sessions again on each one - a fifth of a second of work, and a new
	 * sampler and session behind a view that is being looked at, for a
	 * setting that did not move.
	 */
	if (wasEnabled == enabled)
		return enabled;

	/* Whatever was prepared belongs to the previous switch setting. Clearing
	   this makes the loop prepare again on its next pass, which is the only
	   place it can be done: the loop is where the project is known to be
	   loaded. */
	rtosPrepared.store(false, std::memory_order_relaxed);

	if (!enabled)
	{
		/* Off means the sampler goes away, and with it every read the task
		   view would have made. The model stays, so the last snapshot is
		   still there to look at. */
		rtosSampler.reset();

		return false;
	}

	return true;
}

void ViewerDataHandler::forgetRtosPreparation()
{
	/* Called when the project changes underneath: what was prepared came from
	   another symbol file. The next pass of the loop prepares again. */
	rtosPrepared.store(false, std::memory_order_relaxed);
	rtosSampler.reset();
}

bool ViewerDataHandler::rtosRoundIsDue(double now, std::chrono::steady_clock::time_point nextDeadline, std::chrono::steady_clock::duration interval) const
{
	if (rtosSampler == nullptr || !rtosEnabled.load(std::memory_order_relaxed))
		return false;

	if (rtosLastRoundAt >= 0.0 && (now - rtosLastRoundAt) < rtosIntervalSeconds)
		return false;

	/* Not past the next sample as well, or the round gives way.

	   The loop wakes on the deadline, so by the time a sample has been read
	   and written the current one is already behind it. Comparing against the
	   deadline would refuse every round; comparing against the one after it
	   asks the question that matters, which is whether this pass still fits
	   inside the period it belongs to. */
	return std::chrono::steady_clock::now() < nextDeadline + interval;
}

/*
 * One batch of switch records.
 *
 * The ring is one object on the target: a monotonic head, its capacity, and the
 * records. The head is read first and the records after it, and a record that
 * the target overwrote while it was being read is a record the counter no
 * longer covers - which is why what is read is bounded by the head that was
 * read before it, and not by whatever the head says afterwards.
 *
 * What is read is the newest few rather than the oldest: a round has a budget,
 * and a reader that falls behind wants to be looking at now. Everything passed
 * over is counted, so the strip can say its picture has a hole instead of
 * drawing one task's run across another's turn.
 */
/*
 * The stream, drained on the sampling tick.
 *
 * RTT is not a ring: the target appends to a buffer and the probe empties it.
 * What matters is draining it at the rate it fills, which the task view's own
 * round - twice a second - is nowhere near. So this runs on the sampling tick,
 * where the variable side already has the probe open, and hands the records to
 * the same history the ring path fills.
 *
 * A record the target was in the middle of writing is carried to the next read
 * rather than dropped: the read boundary can fall anywhere, and eight bytes of
 * a record is not something to throw away for want of the other two.
 */
void ViewerDataHandler::pollEventStream(double nowSeconds)
{
	if (!rtosEnabled.load(std::memory_order_relaxed) || rtosModel == nullptr || debugProbe == nullptr)
		return;

	if (!rttActive)
	{
		if (!debugProbe->startRtt())
			return;

		rttActive = true;
	}

	std::array<uint8_t, 4096> buffer{};
	const uint32_t bytes = debugProbe->readRtt(buffer.data(), static_cast<uint32_t>(buffer.size()));

	if (bytes == 0 && rttCarry.empty())
		return;

	rttCarry.insert(rttCarry.end(), buffer.begin(), buffer.begin() + std::min<uint32_t>(bytes, static_cast<uint32_t>(buffer.size())));

	const size_t whole = rttCarry.size() / sizeof(RtosEventRing::Record);

	if (whole == 0)
		return;

	std::vector<RtosEventRing::Record> batch;
	batch.reserve(whole);

	for (size_t entry = 0; entry < whole; entry++)
	{
		RtosEventRing::Record record;

		std::memcpy(&record.tcbAddress, rttCarry.data() + entry * sizeof(RtosEventRing::Record), 4);
		std::memcpy(&record.timeUs, rttCarry.data() + entry * sizeof(RtosEventRing::Record) + 4, 4);

		batch.push_back(record);
	}

	rttCarry.erase(rttCarry.begin(), rttCarry.begin() + static_cast<long>(whole * sizeof(RtosEventRing::Record)));

	/* The same anchor rule the ring path uses, and for the same reason. */
	if (rttAnchorSeconds == 0.0)
	{
		rttAnchorUs = batch.front().timeUs;
		rttAnchorSeconds = nowSeconds - static_cast<double>(static_cast<uint32_t>(batch.back().timeUs - rttAnchorUs)) / 1000000.0;
	}
	else
	{
		const double targetElapsed = static_cast<double>(static_cast<uint32_t>(batch.back().timeUs - rttAnchorUs)) / 1000000.0;
		const double error = (nowSeconds - rttAnchorSeconds) - targetElapsed;

		if (std::abs(error) < 5.0)
			rttAnchorSeconds += error * 0.1;
		else
		{
			rttAnchorUs = batch.front().timeUs;
			rttAnchorSeconds = nowSeconds;
		}
	}

	rtosModel->appendEventIntervals(RtosEventRing::intervalsFrom(batch, rttAnchorUs, rttAnchorSeconds, nowSeconds), rtosEventsKeepSeconds);
}


void ViewerDataHandler::readEventRing(RtosProbeMemory& memory, double nowSeconds)
{
	if (rtosEventRingAddress == 0 || rtosModel == nullptr)
		return;

	/* Two words: the head, then the capacity. */
	std::array<uint8_t, 8> header{};

	if (!memory.read(rtosEventRingAddress, header.data(), static_cast<uint32_t>(header.size())))
	{
		/* Once, so that a ring that cannot be read is a line in the log rather
		   than a strip that quietly stays empty. */
		static bool logged = false;

		if (!logged)
		{
			logger->warn("The switch ring at 0x{:08X} could not be read: {}", rtosEventRingAddress, debugProbe->getLastErrorMsg());

			logged = true;
		}

		return;
	}

	uint32_t words[2] = {};
	std::memcpy(words, header.data(), sizeof(words));

	const uint32_t head = words[0];
	const uint32_t capacity = words[1];

	/* A ring is a power of two, and a sane one is a few thousand records. */
	if (capacity < 2 || capacity > 65536 || (capacity & (capacity - 1)) != 0)
		return;

	const RtosEventRing::Window window = RtosEventRing::windowFor(head, rtosEventsRead, capacity, rtosEventsPerRound);

	/* The first read is not a reader that fell behind: what was in the ring
	   before it started is history, not loss. */
	if (rtosEventsRead != 0)
		rtosModel->noteLostEvents(window.lost);

	rtosEventsRead = head;

	if (window.count == 0)
		return;

	std::vector<RtosEventRing::Record> records;
	records.reserve(window.count);

	uint32_t next = window.first;

	while (records.size() < window.count)
	{
		const uint32_t index = next & (capacity - 1u);
		const uint32_t run = std::min(window.count - static_cast<uint32_t>(records.size()), capacity - index);
		const uint32_t address = rtosEventRingAddress + 8u + index * static_cast<uint32_t>(sizeof(RtosEventRing::Record));

		std::vector<uint8_t> bytes(static_cast<size_t>(run) * sizeof(RtosEventRing::Record));

		if (!memory.read(address, bytes.data(), static_cast<uint32_t>(bytes.size())))
			return;

		for (uint32_t entry = 0; entry < run; entry++)
		{
			RtosEventRing::Record record;

			std::memcpy(&record.tcbAddress, bytes.data() + entry * 8, 4);
			std::memcpy(&record.timeUs, bytes.data() + entry * 8 + 4, 4);

			records.push_back(record);
		}

		next += run;
	}

	/*
	 * Where the target's clock meets the host's.
	 *
	 * Taken from the newest record and the host time it arrived at, then
	 * corrected by a tenth of the error each round: the two crystals drift
	 * slowly apart, and following every reading would put the read's own
	 * latency into the picture as jitter. A jump too large to be drift is a new
	 * session on the host, and the anchor is set again rather than nudged.
	 */
	if (rtosEventAnchorSeconds == 0.0)
	{
		/* The oldest record of the batch, not the newest. `intervalsFrom` starts
		   its running total from this one, and every record in the batch is at
		   or after it - whereas the newest is *older* than the ones before it in
		   the next batch, and an unsigned step backwards is seventy-one minutes
		   forwards. That is where the runs went: into the future, off the end of
		   the window. */
		rtosEventAnchorUs = records.front().timeUs;
		rtosEventAnchorSeconds = nowSeconds - static_cast<double>(static_cast<uint32_t>(records.back().timeUs - records.front().timeUs)) / 1000000.0;
	}
	else
	{
		const double targetElapsed = static_cast<double>(static_cast<uint32_t>(records.back().timeUs - rtosEventAnchorUs)) / 1000000.0;
		const double error = (nowSeconds - rtosEventAnchorSeconds) - targetElapsed;

		if (std::abs(error) < 5.0)
			rtosEventAnchorSeconds += error * 0.1;
		else
		{
			rtosEventAnchorUs = records.back().timeUs;
			rtosEventAnchorSeconds = nowSeconds;
		}
	}

	rtosModel->appendEventIntervals(RtosEventRing::intervalsFrom(records, rtosEventAnchorUs, rtosEventAnchorSeconds, nowSeconds), rtosEventsKeepSeconds);
}

void ViewerDataHandler::runRtosRound(std::chrono::steady_clock::time_point start)
{
	if (rtosSampler == nullptr || rtosModel == nullptr || debugProbe == nullptr)
		return;

	const auto secondsSinceStart = [&start]()
	{
		return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();
	};

	const double startedAt = secondsSinceStart();

	/* The probe lock is held by the sampling loop, so the memory reader is
	   built on a reference to the open probe rather than looking it up and
	   taking the lock a second time. */
	RtosProbeMemory memory(*debugProbe);

	/* Path B, read before the round's verdict: a round that did not hold
	   together says nothing about whether switches happened. */
	readEventRing(memory, startedAt);

	RtosSampler::Round round = rtosSampler->collect(memory, rtosSessionId, rtosSettings);

	if (!round.usable())
	{
		/* A round that did not hold together is not published. The previous
		   one stays, and the reason is kept so the interface can say a round
		   went missing instead of showing half of one. */
		rtosLastRoundAt = secondsSinceStart();

		std::string reason;

		for (const std::string& problem : round.problems)
			reason += (reason.empty() ? "" : "; ") + problem;

		rtosModel->recordRejectedRound(reason.empty() ? "the round did not hold together" : reason, startedAt);

		return;
	}

	/*
	 * The stack water marks, a few tasks of this round's set.
	 *
	 * It runs here rather than inside the sampler because it is the one part of
	 * the task view that is deliberately not a function of a single round: a
	 * stack costs hundreds of bytes to read, which is milliseconds, and the
	 * whole set of them would not fit in a sampling period. The sweep holds the
	 * turn and the readings, and it is handed the same memory reader the round
	 * used, so its reads are counted with the round's.
	 *
	 * Before the depths and before the publish: what this adds has to be part
	 * of the snapshot that is published, and nothing may modify a snapshot once
	 * the model has handed it out.
	 */
	rtosStackSweep.step(memory, round.snapshot->tasks, rtosStackSweepSettings, startedAt);
	rtosStackSweep.applyTo(*round.snapshot, startedAt);

	/* The share of the time each task used, from the counters the round read
	   with the control blocks. The window is several rounds long, so the first
	   round after the view starts leaves every row without a share, and the
	   window grows to its full length over the rounds that follow. */
	rtosCpuLoad.step(round.snapshot->tasks, round.snapshot->cpuLoad, rtosCpuLoadSettings, startedAt);

	/* The end of the round is only known now, which is why the sampler leaves
	   the capture times to this side. It is the end of everything the snapshot
	   holds, the stack reads included: stamping it before them would have the
	   snapshot claim its parts were read over a shorter span than they were. */
	rtosLastRoundAt = secondsSinceStart();
	round.snapshot->captureStartedAt = startedAt;
	round.snapshot->captureFinishedAt = rtosLastRoundAt;
	round.snapshot->captureSpanSeconds = round.snapshot->captureFinishedAt - round.snapshot->captureStartedAt;

	/* The depths go into their variables before the round is published, on this
	   thread and outside the sampling loop's own bookkeeping: the values come
	   from the snapshot, which cannot change, and they are written into the
	   variable model, which has its own lock. */
	publishSyncDepths(*round.snapshot);

	rtosModel->publish(std::move(round.snapshot));

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
		/* Preparing the task view reads a symbol file, which takes seconds on
		   a large one, so it happens here rather than at the moment the switch
		   went on: this is the first point at which the project is known to be
		   loaded and its file path means anything.

		   The first pass normally fails, because the loop starts before any
		   project is open, and the next one succeeds. A failed attempt is
		   therefore not reported as a fault: it is logged at debug level, and
		   the next try is put off so that a symbol file that is genuinely
		   missing cannot fill the log at the sampling rate. */
		if (rtosEnabled.load(std::memory_order_relaxed) && !rtosPrepared.load(std::memory_order_relaxed) && Clock::now() >= rtosNextPrepareAttempt)
		{
			const bool prepared = prepareRtos();

			rtosPrepared.store(prepared, std::memory_order_relaxed);

			if (!prepared)
				rtosNextPrepareAttempt = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(rtosPrepareRetrySeconds));
		}

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

						/* Path B's stream, drained here because this is the
						   tick that runs often enough for it. */
						pollEventStream(timestamp);
					}
					else
						metricsFailedSamples.fetch_add(1, std::memory_order_relaxed);

					/* The task view goes after the variables. Both of them
					   would rather lose a round than push the other out of the
					   way, and the snapshot loses one more cheaply. */
					if (rtosRoundIsDue(period, nextDeadline, sampleInterval))
						runRtosRound(start);

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

			/* A derived series holds a value worked out on this side, so its
			   variable has no address on the target. Adding it to the read
			   plan would have the loop read whatever lives at that address
			   and record it as the series' value, which is worse than leaving
			   the series out: a wrong number looks like a right one. */
			if (ser->derived)
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
