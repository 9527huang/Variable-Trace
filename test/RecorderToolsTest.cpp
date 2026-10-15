#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "BorrowedTransport.hpp"
#include "McpContext.hpp"
#include "McpToolRegistry.hpp"
#include "McpTools.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "RecorderHandler.hpp"
#include "SerialDebugProbe.hpp"
#include "SerialTargetSimulator.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

namespace
{
	using mcp::Json;

	constexpr uint32_t settingsAddress = 0x20000100;
	constexpr uint32_t recorderAddress = 0x20001000;
	constexpr uint32_t counterAddress = 0x20002000;
	constexpr uint32_t levelAddress = 0x20002004;

	recorder::Settings settings()
	{
		recorder::Settings result;
		result.version = 8;
		result.revision = 3;
		result.timestepNs = 10000;
		result.maxBufferSize = 32768;
		result.maxVariables = 12;
		result.floatSupport = 1;
		return result;
	}

	class RecorderToolsTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("recorder-tools-test", std::make_shared<spdlog::sinks::null_sink_mt>());
			simulator = std::make_unique<RecorderSimulator>(target, settingsAddress, recorderAddress, settings());
			probe = std::make_shared<SerialDebugProbe>(logger.get(), std::make_unique<BorrowedTransport>(target));

			IDebugProbe::DebugProbeSettings probeSettings;
			probeSettings.debugProbe = IDebugProbe::Serial;
			probeSettings.serialNumber = "SIM";
			probeSettings.baudrate = 115200;
			std::vector<std::pair<uint32_t, uint8_t>> sampleList = {{counterAddress, 4}, {levelAddress, 2}};
			ASSERT_TRUE(probe->startAcqusition(probeSettings, sampleList, 100));

			applicationDone = true;
			viewer = std::make_unique<ViewerDataHandler>(&groupHandler, &variableHandler, &plotHandler, &tracePlotHandler,
				applicationDone, &sharedMutex, logger.get());
			viewer->setDebugProbe(probe);
			viewer->setState(DataHandlerBase::State::RUN);

			auto counter = std::make_shared<Variable>("counter");
			counter->setType(Variable::Type::I32);
			counter->setAddress(counterAddress);
			variableHandler.addVariable(counter);

			auto level = std::make_shared<Variable>("level");
			level->setType(Variable::Type::U16);
			level->setAddress(levelAddress);
			variableHandler.addVariable(level);

			auto plot = plotHandler.addPlot("capture");
			plot->addSeries(counter.get());
			plot->addSeries(level.get());
			auto group = groupHandler.addGroup("recorder", PlotGroup::Type::Recorder);
			group->addPlot(plot);
			groupHandler.setActiveGroup("recorder");

			handler = std::make_unique<recorder::RecorderHandler>(logger.get());
			handler->setSymbols(settingsAddress, recorderAddress);

			context.variableHandler = &variableHandler;
			context.plotGroupHandler = &groupHandler;
			context.viewerDataHandler = viewer.get();
			context.recorderHandler = handler.get();
			context.getApiWritesEnabled = [this]() { return apiWritesEnabled; };
			mcp::RecorderTools::registerAll(registry);
		}

		Json call(const std::string& name, const Json& arguments = Json::object({}))
		{
			const mcp::Tool* tool = registry.find(name);
			if (tool == nullptr)
				throw mcp::ToolError("no tool named " + name);
			return tool->handler(context, arguments);
		}

		void feed(uint32_t index)
		{
			target.poke32(counterAddress, index);
			target.poke16(levelAddress, static_cast<uint16_t>(index * 7));
			simulator->step();
		}

		std::shared_ptr<spdlog::logger> logger;
		SerialTargetSimulator target;
		std::unique_ptr<RecorderSimulator> simulator;
		std::shared_ptr<SerialDebugProbe> probe;
		std::atomic<bool> applicationDone{false};
		std::mutex sharedMutex;
		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotHandler tracePlotHandler;
		PlotGroupHandler groupHandler;
		std::unique_ptr<ViewerDataHandler> viewer;
		std::unique_ptr<recorder::RecorderHandler> handler;
		McpContext context;
		mcp::ToolRegistry registry;
		bool apiWritesEnabled = true;
	};
}

TEST_F(RecorderToolsTest, StartsDownloadsAndStopsThroughTheSharedProbe)
{
	call("set_recorder_enabled", Json::object({{"enabled", true}}));
	const Json started = call("start_recorder");

	EXPECT_TRUE(started.at("running").get<bool>());
	EXPECT_EQ(started.at("group"), "recorder");
	EXPECT_EQ(started.at("variable_count"), 2);
	EXPECT_EQ(started.at("bytes_per_sample"), 6);
	EXPECT_EQ(simulator->state(), recorder::State::Running);

	for (uint32_t index = 0; index < 20; ++index)
		feed(index);

	const Json downloaded = call("download_recorder");
	EXPECT_TRUE(downloaded.at("running").get<bool>());
	EXPECT_EQ(downloaded.at("samples"), 20);
	EXPECT_EQ(downloaded.at("bytes_downloaded"), 120);
	EXPECT_FALSE(downloaded.at("overran").get<bool>());
	ASSERT_EQ(downloaded.at("timestamps_s").size(), 20u);
	ASSERT_EQ(downloaded.at("series").size(), 2u);
	EXPECT_EQ(downloaded.at("series").at(0).at("name"), "counter");
	EXPECT_EQ(downloaded.at("series").at(0).at("values").at(19), 19.0);
	EXPECT_EQ(downloaded.at("series").at(1).at("name"), "level");
	EXPECT_EQ(downloaded.at("series").at(1).at("values").at(19), 133.0);

	const Json stopped = call("stop_recorder");
	EXPECT_FALSE(stopped.at("running").get<bool>());
	EXPECT_FALSE(stopped.at("already_stopped").get<bool>());
	EXPECT_EQ(simulator->state(), recorder::State::Init);

	const Json stoppedAgain = call("stop_recorder");
	EXPECT_TRUE(stoppedAgain.at("already_stopped").get<bool>());
}

TEST_F(RecorderToolsTest, RefusesStartUntilEnabled)
{
	EXPECT_THROW(call("start_recorder"), mcp::ToolError);
	EXPECT_EQ(simulator->state(), recorder::State::Init);
}

TEST_F(RecorderToolsTest, RefusesTargetControlWhenApiWritesAreDisabled)
{
	call("set_recorder_enabled", Json::object({{"enabled", true}}));
	apiWritesEnabled = false;

	try
	{
		call("start_recorder");
		FAIL() << "start_recorder should obey the API write guard";
	}
	catch (const mcp::ToolError& error)
	{
		EXPECT_NE(std::string(error.what()).find("disabled"), std::string::npos);
	}

	EXPECT_EQ(simulator->state(), recorder::State::Init);
}

TEST_F(RecorderToolsTest, RefusesStartWithoutTheAcquisitionConnection)
{
	call("set_recorder_enabled", Json::object({{"enabled", true}}));
	viewer->setState(DataHandlerBase::State::STOP);

	try
	{
		call("start_recorder");
		FAIL() << "start_recorder should require a running acquisition";
	}
	catch (const mcp::ToolError& error)
	{
		EXPECT_NE(std::string(error.what()).find("start_acquisition"), std::string::npos);
	}
}

TEST_F(RecorderToolsTest, StartRefreshesASavedProjectsInternalRecorderSymbols)
{
	handler->clearSymbols();
	uint32_t syncCalls = 0;
	uint32_t refreshCalls = 0;
	context.getElfPath = []() { return std::string("saved-project.axf"); };
	context.refreshVariableAddresses = [&](const std::string& path)
	{
		EXPECT_EQ(path, "saved-project.axf");
		refreshCalls++;
		return true;
	};
	context.syncRecorderSymbols = [&]()
	{
		syncCalls++;
		if (refreshCalls != 0)
			handler->setSymbols(settingsAddress, recorderAddress);
		return handler->hasSymbols();
	};

	call("set_recorder_enabled", Json::object({{"enabled", true}}));
	const Json started = call("start_recorder");

	EXPECT_TRUE(started.at("running").get<bool>());
	EXPECT_EQ(refreshCalls, 1u);
	EXPECT_EQ(syncCalls, 2u);
}
