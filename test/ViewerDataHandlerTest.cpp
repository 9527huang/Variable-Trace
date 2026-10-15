#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "IDebugProbe.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The acquisition thread, running against a probe that answers straight away.
 *
 * Where a sample ends up on the time axis, and whether the run is interrupted
 * at all, is decided by the loop inside that thread, so there is no way to ask
 * the questions these tests are about without letting the thread run.
 */

namespace
{
	/* Answers every read with a value of its own rather than talking to a
	   target, so that a run gets its samples with nothing attached. It also
	   counts how often it was put down and picked up again, because that is
	   what says whether a run was interrupted.
	 *
	 * The two delays stand for the time a real probe takes: a J-Link closes its
	 * connection on the way down and opens and connects again on the way up,
	 * and the samples of an interrupted run are what that time costs. */
	class StubDebugProbe : public IDebugProbe
	{
	   public:
		bool startAcqusition(const DebugProbeSettings&, std::vector<std::pair<uint32_t, uint8_t>>&, uint32_t) override
		{
			std::this_thread::sleep_for(startDelay);

			attachments++;
			isRunning = true;
			return true;
		}

		bool stopAcqusition() override
		{
			std::this_thread::sleep_for(stopDelay);

			detachments++;
			isRunning = false;
			return true;
		}

		bool isValid() const override
		{
			return true;
		}

		std::string getTargetName() override
		{
			return "stub";
		}

		/* Only the streaming mode reads this, and no test here runs in it. */
		std::optional<varEntryType> readSingleEntry() override
		{
			return std::nullopt;
		}

		bool readMemory(uint32_t, uint8_t* buf, uint32_t size) override
		{
			if (failReads.load())
				return false;

			/* The caller hands over the address of a four byte value, so a
			   wider read than that is one it could not hold. */
			for (uint32_t index = 0; index < size && index < sizeof(uint32_t); index++)
				buf[index] = static_cast<uint8_t>(index);

			return true;
		}

		bool writeMemory(uint32_t, uint8_t*, uint32_t) override
		{
			return true;
		}

		std::string getLastErrorMsg() const override
		{
			return "";
		}

		std::vector<std::string> getConnectedDevices() override
		{
			return {};
		}

		std::chrono::milliseconds stopDelay{};
		std::chrono::milliseconds startDelay{};
		std::atomic<uint32_t> attachments{0};
		std::atomic<uint32_t> detachments{0};
		std::atomic<bool> failReads{false};
	};

	class ViewerDataHandlerTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("viewer-data-handler-test", std::make_shared<spdlog::sinks::null_sink_mt>());
		}

		void TearDown() override
		{
			/* The sampling thread runs until this is set, and the handler joins
			   it on the way out. */
			applicationDone = true;
			viewerDataHandler.reset();
		}

		/* One variable on one plot in the active group, and a run going over
		   them.
		 *
		 * The probe type is what decides whether the probe has to hear about a
		 * change to the list of addresses: the serial driver sweeps a list this
		 * process sends it down the port, while the debug probes are read a
		 * word at a time on request, so for those the list never leaves this
		 * process. */
		void startSampling(uint32_t frequencyHz, uint32_t probeType = IDebugProbe::Probe::Stlink, std::chrono::milliseconds stopDelay = {}, std::chrono::milliseconds startDelay = {})
		{
			plotGroupHandler.addGroup("group");
			plotGroupHandler.setActiveGroup("group");

			variableHandler.addNewVariable("adc");

			/* Away from the address a variable is given when it is made, so
			   that a variable added later is not read by accident: a fresh one
			   starts at that same default address, and a plot drawing the
			   default address would make it look sampled. */
			variableHandler.getVariable("adc")->setAddress(0x20001000);

			plot = plotHandler.addPlot("adc");
			plot->addSeries(variableHandler.getVariable("adc").get());
			plotGroupHandler.getActiveGroup()->addPlot(plot);

			viewerDataHandler = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &modelMutex, logger.get());

			ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();
			settings.sampleFrequencyHz = frequencyHz;
			viewerDataHandler->setSettings(settings);

			IDebugProbe::DebugProbeSettings probeSettings = viewerDataHandler->getProbeSettings();
			probeSettings.debugProbe = probeType;
			viewerDataHandler->setProbeSettings(probeSettings);

			stub = std::make_shared<StubDebugProbe>();
			stub->stopDelay = stopDelay;
			stub->startDelay = startDelay;

			viewerDataHandler->setDebugProbe(stub);
			viewerDataHandler->setState(DataHandlerBase::State::RUN);
		}

		/* A second variable, put on the plot the way dragging one there does. */
		void attachAnotherVariable(const std::string& name, uint32_t address)
		{
			variableHandler.addNewVariable(name);
			variableHandler.getVariable(name)->setAddress(address);

			auto modelLock = variableHandler.lockModel();
			plot->addSeries(variableHandler.getVariable(name).get());
		}

		/* How many samples the plot holds. Read under the same lock the
		   acquisition thread takes around the two calls that put one there. */
		uint32_t sampleCount()
		{
			std::lock_guard<std::mutex> lock(modelMutex);
			return plot->getXAxisSeries()->getSize();
		}

		/* The samples of the plot, oldest first, as they sit on the time axis.
		   The plot is a curve, so the buffer the drawing reads for the
		   horizontal axis is the one that holds the times. */
		std::vector<double> sampleTimes()
		{
			std::lock_guard<std::mutex> lock(modelMutex);

			ScrollingBuffer<double>* times = plot->getXAxisSeries();
			times->copyData();

			const double* first = times->getFirstElementCopy();
			return std::vector<double>(first, first + times->getSize());
		}

		bool waitUntil(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

			while (std::chrono::steady_clock::now() < deadline)
			{
				if (condition())
					return true;

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			return false;
		}

		/* Waits for the plot to hold at least as many samples as asked for. The
		   run is on another thread, so looking until it has got there is the
		   only way to know. Gives up rather than hanging when it never does. */
		bool waitForSamples(uint32_t count)
		{
			return waitUntil([this, count] { return sampleCount() >= count; });
		}

		std::shared_ptr<spdlog::logger> logger;

		std::atomic<bool> applicationDone{false};
		std::mutex modelMutex;

		PlotGroupHandler plotGroupHandler;
		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotHandler tracePlotHandler;
		std::shared_ptr<Plot> plot;
		std::shared_ptr<StubDebugProbe> stub;

		/* Declared last, so that it is destroyed before everything it reaches
		   into on the way out. */
		std::unique_ptr<ViewerDataHandler> viewerDataHandler;
	};

	/*
	 * What the panel keeps is what the buffers can hold, and the two settings
	 * are different kinds of thing: how many samples a series keeps is a
	 * capacity, and how much time the viewport shows is a length. A settings set
	 * that comes in from somewhere other than the panel is cut down at this end
	 * rather than trusted, so that what the reader sees in the field is what was
	 * stored.
	 */

	TEST_F(ViewerDataHandlerTest, aViewportWidthIsKeptWithinItsBounds)
	{
		plotGroupHandler.addGroup("group");
		plotGroupHandler.setActiveGroup("group");
		variableHandler.addNewVariable("adc");

		std::shared_ptr<Plot> target = plotHandler.addPlot("adc");
		target->addSeries(variableHandler.getVariable("adc").get());

		viewerDataHandler = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &modelMutex, logger.get());

		ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();

		/* A window no run could be shorter than, and one far longer than any
		   this is meant to show: both are brought back to what the setting can
		   express. */
		settings.viewportWidthMs = 0;
		viewerDataHandler->setSettings(settings);

		EXPECT_EQ(viewerDataHandler->getSettings().viewportWidthMs, ViewerDataHandler::minViewportWidthMs);

		settings.viewportWidthMs = 100000000;
		viewerDataHandler->setSettings(settings);

		EXPECT_EQ(viewerDataHandler->getSettings().viewportWidthMs, ViewerDataHandler::maxViewportWidthMs);
	}

	TEST_F(ViewerDataHandlerTest, aCapacityBeyondTheMemoryBudgetIsCutToIt)
	{
		plotGroupHandler.addGroup("group");
		plotGroupHandler.setActiveGroup("group");
		variableHandler.addNewVariable("adc");

		std::shared_ptr<Plot> target = plotHandler.addPlot("adc");
		target->addSeries(variableHandler.getVariable("adc").get());

		viewerDataHandler = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &modelMutex, logger.get());

		const uint32_t ceiling = plotHandler.maxPointsForMemoryBudget();

		ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();
		settings.maxPoints = ceiling + 1000000;
		viewerDataHandler->setSettings(settings);

		const ViewerDataHandler::Settings applied = viewerDataHandler->getSettings();

		EXPECT_EQ(applied.maxPoints, ceiling);
		EXPECT_EQ(target->getXAxisSeries()->getMaxSize(), ceiling);
	}
}  // namespace

/* Importing a variable that no plot draws changes nothing about what is read
   from the target, and the run goes on undisturbed.
 *
 * This is the case a reader meets most often: a symbol is picked out of the
 * symbol file and put in the table, and only afterwards is it dragged onto a
 * plot. Putting the probe down over that breaks every curve on the screen for
 * as long as the probe takes to come back, and the part of the signal that
 * falls in that stretch is simply not there to be drawn. */
TEST_F(ViewerDataHandlerTest, AVariableNoPlotDrawsLeavesTheRunAlone)
{
	startSampling(200);
	ASSERT_TRUE(waitForSamples(10));

	const uint32_t samplesBefore = sampleCount();

	variableHandler.addNewVariable("spare");
	viewerDataHandler->requestRestart();

	ASSERT_TRUE(waitForSamples(samplesBefore + 20));

	/* The variable joined the table and is not read from the target, which is
	   what the list built from the plots says about it. */
	EXPECT_FALSE(variableHandler.getVariable("spare")->getIsCurrentlySampled());

	/* Nothing was said to the probe, and it was never put down. */
	EXPECT_EQ(stub->attachments.load(), 1u);
	EXPECT_EQ(stub->detachments.load(), 0u);
}

/* Adding a series does change what is read, and for a probe that this process
   reads through the change takes effect on the next sample: the address joins
   the list and is read from then on, with the connection left alone. */
TEST_F(ViewerDataHandlerTest, WhatIsReadChangesWithoutTheProbeBeingPutDown)
{
	startSampling(200);
	ASSERT_TRUE(waitForSamples(10));

	attachAnotherVariable("second", 0x20000004);
	viewerDataHandler->requestRestart();

	ASSERT_TRUE(waitForSamples(sampleCount() + 20));

	EXPECT_TRUE(variableHandler.getVariable("second")->getIsCurrentlySampled());
	EXPECT_EQ(stub->attachments.load(), 1u);
	EXPECT_EQ(stub->detachments.load(), 0u);
}

TEST_F(ViewerDataHandlerTest, ATargetRawValueIsConvertedWhenCsvIsOff)
{
	startSampling(200);
	ASSERT_TRUE(waitForSamples(5));

	{
		auto modelLock = variableHandler.lockModel();
		variableHandler.getVariable("adc")->setType(Variable::Type::U32);
	}

	viewerDataHandler->requestRestart();
	const uint32_t before = sampleCount();
	ASSERT_TRUE(waitForSamples(before + 5));

	/* StubDebugProbe returns 00 01 02 03. The displayed value must be updated
	   even though logging is disabled; CSV must not be the conversion trigger. */
	EXPECT_EQ(variableHandler.getVariable("adc")->getValue(), 0x03020100u);
}

TEST_F(ViewerDataHandlerTest, AFailedTargetReadStopsWithoutAppendingAnInvalidSample)
{
	startSampling(200);
	ASSERT_TRUE(waitForSamples(5));

	stub->failReads.store(true);
	ASSERT_TRUE(waitUntil([this] { return viewerDataHandler->getStateImmediate() == DataHandlerBase::State::STOP; }));

	const ViewerDataHandler::PerformanceMetrics metrics = viewerDataHandler->getPerformanceMetrics();
	EXPECT_EQ(metrics.failedSamples, 1u);
	EXPECT_EQ(sampleCount(), metrics.completedSamples);
}

TEST_F(ViewerDataHandlerTest, RepeatedBindingRebuildsKeepSamplingWhileSeriesAreRemoved)
{
	startSampling(1000);
	ASSERT_TRUE(waitForSamples(20));
	const uint32_t before = sampleCount();

	for (uint32_t index = 0; index < 50; ++index)
	{
		const std::string name = "temporary" + std::to_string(index);
		attachAnotherVariable(name, 0x20002000 + index * sizeof(uint32_t));
		viewerDataHandler->requestRestart();

		{
			auto modelLock = variableHandler.lockModel();
			plot->removeSeries(name);
		}
		variableHandler.erase(name);
		viewerDataHandler->requestRestart();
	}

	ASSERT_TRUE(waitForSamples(before + 50));
	const auto metrics = viewerDataHandler->getPerformanceMetrics();
	EXPECT_EQ(metrics.failedSamples, 0u);
	EXPECT_EQ(stub->attachments.load(), 1u);
}

/* The serial driver sweeps a list this process sends it, so a change to that
   list has to reach it, and reaching it means putting it down and picking it
   up again. */
TEST_F(ViewerDataHandlerTest, AProbeThatSweepsTheListIsToldAgain)
{
	startSampling(200, IDebugProbe::Probe::Serial);
	ASSERT_TRUE(waitForSamples(5));

	attachAnotherVariable("second", 0x20000004);
	viewerDataHandler->requestRestart();

	ASSERT_TRUE(waitUntil([this] { return stub->attachments.load() == 2u; }));

	EXPECT_EQ(stub->detachments.load(), 1u);
	EXPECT_TRUE(variableHandler.getVariable("second")->getIsCurrentlySampled());
}

/* A restart that does put a probe down keeps the run going.
 *
 * The samples already on the plot keep their place, and what arrives after the
 * restart goes after them. A clock that was put back to the start of the run
 * instead would send the new samples to the beginning of the time axis, on top
 * of the ones already there, and the curve would fold back over itself. */
TEST_F(ViewerDataHandlerTest, ARestartLeavesTheSamplesWhereTheyAre)
{
	/* The delays stand for a probe that takes a moment to come back. */
	startSampling(200, IDebugProbe::Probe::Serial, std::chrono::milliseconds(20), std::chrono::milliseconds(60));
	ASSERT_TRUE(waitForSamples(5));

	/* Cleared the way the start button clears it, so that everything the plot
	   holds from here on belongs to one run. */
	{
		std::lock_guard<std::mutex> lock(modelMutex);
		plot->erase();
	}

	ASSERT_TRUE(waitForSamples(10));

	attachAnotherVariable("second", 0x20000004);

	const uint32_t beforeRestart = sampleCount();
	viewerDataHandler->requestRestart();

	/* Waiting for more samples than the plot already holds, so that the restart
	   is well behind us by the time the axis is read. */
	ASSERT_TRUE(waitForSamples(beforeRestart + 20));

	const std::vector<double> times = sampleTimes();
	ASSERT_GE(times.size(), 30u);

	double largestGap = 0.0;

	for (size_t index = 1; index < times.size(); index++)
	{
		ASSERT_GE(times[index], times[index - 1])
			<< "sample " << index << " is earlier than the one before it, so the time axis runs backwards";

		largestGap = std::max(largestGap, times[index] - times[index - 1]);
	}

	/* The pause is not left as a stretch with no samples on it either: the
	   clock is moved along by the whole of it, the part spent picking the probe
	   up again included. Moving it by only the part spent putting it down would
	   leave the rest, which is the longer part here, as a gap. Four sample
	   periods at the rate asked for, against a pause of eighty milliseconds. */
	EXPECT_LT(largestGap, 0.020);
}

/* The clock is moved by the time a restart takes, and the counter that holds
   the sampling rate down is left where it is along with it.
 *
 * That counter measures the clock: it counts the samples of the run, and the
 * clock counts the time of the run, so the two agree with each other. Clearing
 * it while the clock still reads the run so far would let through as many
 * samples as the run has taken up to that point, all at once, as fast as the
 * probe answers - a run that has lasted an hour would read for hours to catch
 * up. The run here is long enough for that burst to stand out against the
 * handful of samples this window is supposed to hold. */
TEST_F(ViewerDataHandlerTest, ARestartDoesNotLiftTheSamplingRateLimit)
{
	startSampling(200, IDebugProbe::Probe::Serial, std::chrono::milliseconds(20), std::chrono::milliseconds(60));

	/* A second and a half at the rate asked for, so that the burst a cleared
	   counter would let through is several times what is counted below. */
	ASSERT_TRUE(waitForSamples(300));

	attachAnotherVariable("second", 0x20000004);

	/* Counted from the moment the restart is asked for. The burst a cleared
	   counter would let through is over in the time one read of the probe
	   takes, so a window that opened after it would never see it. */
	const uint32_t before = sampleCount();
	viewerDataHandler->requestRestart();
	const auto began = std::chrono::steady_clock::now();

	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	const uint32_t after = sampleCount();
	const auto ended = std::chrono::steady_clock::now();

	const double seconds = std::chrono::duration<double>(ended - began).count();
	const double rate = (static_cast<double>(after) - static_cast<double>(before)) / seconds;

	/* A quarter of the rate that was asked for below it, and twice that rate
	   above it. A rate limit that is still in force cannot read faster than
	   the rate it was set to; the burst a cleared counter would let through
	   is seven times it here. */
	EXPECT_GT(rate, 50.0);
	EXPECT_LT(rate, 400.0);
}
