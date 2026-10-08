#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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
 * Where a sample ends up on the time axis is decided by the loop inside that
 * thread and by the clock the loop keeps for itself, so there is no way to ask
 * the question these tests are about without letting the thread run.
 */

namespace
{
	/* Answers every read with a value of its own rather than talking to a
	   target, so that a run gets its samples with nothing attached. */
	class StubDebugProbe : public IDebugProbe
	{
	   public:
		bool startAcqusition(const DebugProbeSettings&, std::vector<std::pair<uint32_t, uint8_t>>&, uint32_t) override
		{
			isRunning = true;
			return true;
		}

		bool stopAcqusition() override
		{
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

		/* Only the high speed mode reads this, and no test here runs in it. */
		std::optional<varEntryType> readSingleEntry() override
		{
			return std::nullopt;
		}

		bool readMemory(uint32_t, uint8_t* buf, uint32_t size) override
		{
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
	};

	class ViewerDataHandlerTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("viewer-data-handler-test", std::make_shared<spdlog::sinks::null_sink_mt>());

			/* One variable, on one plot, in the group that is sampled. */
			plotGroupHandler.addGroup("group");
			plotGroupHandler.setActiveGroup("group");

			variableHandler.addNewVariable("adc");
			variableHandler.getVariable("adc")->setAddress(0x20000000);

			plot = plotHandler.addPlot("adc");
			plot->addSeries(variableHandler.getVariable("adc").get());
			plotGroupHandler.getActiveGroup()->addPlot(plot);

			viewerDataHandler = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &modelMutex, logger.get());
		}

		void TearDown() override
		{
			/* The sampling thread runs until this is set, and the handler joins
			   it on the way out. */
			applicationDone = true;
			viewerDataHandler.reset();
		}

		void startSampling(uint32_t frequencyHz)
		{
			ViewerDataHandler::Settings settings = viewerDataHandler->getSettings();
			settings.sampleFrequencyHz = frequencyHz;
			viewerDataHandler->setSettings(settings);

			viewerDataHandler->setDebugProbe(std::make_shared<StubDebugProbe>());
			viewerDataHandler->setState(DataHandlerBase::State::RUN);
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

		/* Waits for the plot to hold at least as many samples as asked for. The
		   run is on another thread, so looking until it has got there is the
		   only way to know. Gives up rather than hanging when it never does. */
		bool waitForSamples(uint32_t count)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

			while (std::chrono::steady_clock::now() < deadline)
			{
				if (sampleCount() >= count)
					return true;

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			return false;
		}

		std::shared_ptr<spdlog::logger> logger;

		std::atomic<bool> applicationDone{false};
		std::mutex modelMutex;

		PlotGroupHandler plotGroupHandler;
		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotHandler tracePlotHandler;
		std::shared_ptr<Plot> plot;

		/* Declared last, so that it is destroyed before everything it reaches
		   into on the way out. */
		std::unique_ptr<ViewerDataHandler> viewerDataHandler;
	};
}  // namespace

/* A restart changes what is read from the target, not the run.
 *
 * The samples already on the plot keep their place, and what arrives after the
 * restart is put after them. A clock that was put back to the start of the run
 * instead would send the new samples to the beginning of the time axis, on top
 * of the ones that are already there, and the curve would fold back over
 * itself. */
TEST_F(ViewerDataHandlerTest, ARestartLeavesTheSamplesWhereTheyAre)
{
	startSampling(200);
	ASSERT_TRUE(waitForSamples(5));

	/* Cleared the way the start button clears it, so that everything the plot
	   holds from here on belongs to one run. */
	{
		std::lock_guard<std::mutex> lock(modelMutex);
		plot->erase();
	}

	ASSERT_TRUE(waitForSamples(10));

	const uint32_t beforeRestart = sampleCount();
	viewerDataHandler->requestRestart();

	/* Waiting for more samples than the plot already holds, so that the restart
	   is well behind us by the time the axis is read. */
	ASSERT_TRUE(waitForSamples(beforeRestart + 20));

	const std::vector<double> times = sampleTimes();
	ASSERT_GE(times.size(), 30u);

	for (size_t index = 1; index < times.size(); index++)
		ASSERT_GE(times[index], times[index - 1])
			<< "sample " << index << " is earlier than the one before it, so the time axis runs backwards";

	/* The samples taken before the restart are still there, and the newest one
	   is later than they are. */
	EXPECT_GE(times.back(), times[beforeRestart - 1]);
}

/* The clock is moved forward by the time a restart takes, and the counter that
   holds the sampling rate down is left where it is along with it.
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
	startSampling(200);

	/* A second and a half at the rate asked for, so that the burst a cleared
	   counter would allow is several times what is counted below. */
	ASSERT_TRUE(waitForSamples(300));

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
