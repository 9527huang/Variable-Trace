#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FlashingService.hpp"
#include "GlobalConfig.hpp"
#include "McpContext.hpp"
#include "McpToolRegistry.hpp"
#include "McpTools.hpp"
#include "ViewerDataHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The flashing tools, called the way the server calls them.
 *
 * The global config the tools write to is the one in the user's application
 * data directory, so the directory is redirected to one of this test's own for
 * as long as a test runs. Nothing else here opens a socket or a window: the
 * handlers are looked up in the registry and invoked directly.
 */

namespace
{
	using mcp::Json;

	/* The path is built from an environment variable rather than passed in, so
	   redirecting that variable is the only way to keep the user's own file out
	   of the test. The previous value is handed back to be restored. */
	std::string redirectApplicationDataDirectory(const std::string& directory)
	{
#ifdef _WIN32
		const char* name = "APPDATA";
#else
		const char* name = "HOME";
#endif
		const char* previous = std::getenv(name);
		const std::string saved = previous == nullptr ? std::string() : std::string(previous);

#ifdef _WIN32
		_putenv_s(name, directory.c_str());
#else
		::setenv(name, directory.c_str(), 1);
#endif

		return saved;
	}

	void restoreApplicationDataDirectory(const std::string& saved)
	{
#ifdef _WIN32
		_putenv_s("APPDATA", saved.c_str());
#else
		if (saved.empty())
			::unsetenv("HOME");
		else
			::setenv("HOME", saved.c_str(), 1);
#endif
	}

	std::string echoCommand()
	{
		return "echo hello";
	}

	/* Prints nothing for longer than any test below is willing to wait, so the
	   run is still in flight when the test asks about it.
	   The command carries the {file} marker on purpose: a template without one
	   gets the firmware path appended, and neither ping nor sleep would know
	   what to do with a stray argument. */
	std::string quietCommand()
	{
#ifdef _WIN32
		return "ping 127.0.0.1 -n 6 >nul & rem {file}";
#else
		return "sleep 5; : {file}";
#endif
	}

	class FlashingToolsTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("flashing-tools-test", std::make_shared<spdlog::sinks::null_sink_mt>());

			directory = std::filesystem::temp_directory_path() / "variable-trace-flashing-tools-test";
			std::filesystem::create_directories(directory);

			savedApplicationDataDirectory = redirectApplicationDataDirectory(directory.string());

			globalConfig = std::make_unique<GlobalConfig>(logger.get());
			flasher = std::make_unique<FlashingService>(logger.get());

			context.globalConfig = globalConfig.get();
			context.flasher = flasher.get();

			mcp::FlashingTools::registerAll(registry);
		}

		void TearDown() override
		{
			/* A run that is still going writes into a member of this fixture, so
			   it has to be over before the fixture goes away. */
			if (flasher != nullptr)
				flasher->waitForCompletion();

			/* The acquisition thread runs until the flag is set, and the handler
			   joins it on the way out. */
			applicationDone = true;
			viewerDataHandler.reset();

			restoreApplicationDataDirectory(savedApplicationDataDirectory);

			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
		}

		/* The handler is built for its state flag only. Its sampling thread reads
		   a target, and there is no target here, so the quit flag is raised
		   before the object exists and the thread leaves at once. Starting it
		   would crash on the first read rather than say anything about the flash
		   tool. */
		void startViewerDataHandler()
		{
			applicationDone = true;

			viewerDataHandler = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &probeMutex, logger.get());
			context.viewerDataHandler = viewerDataHandler.get();
		}

		Json call(const std::string& name, const Json& arguments = Json::object({}))
		{
			const mcp::Tool* tool = registry.find(name);

			if (tool == nullptr)
				throw mcp::ToolError("no tool named " + name);

			return tool->handler(context, arguments);
		}

		/* Polls until the run finishes, so that a test does not depend on how
		   long a shell takes to start. */
		std::string waitForState(const std::string& expected)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

			std::string state;

			while (std::chrono::steady_clock::now() < deadline)
			{
				state = call("get_flash_status").at("state").get<std::string>();

				if (state == expected)
					return state;

				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}

			return state;
		}

		std::shared_ptr<spdlog::logger> logger;
		std::filesystem::path directory;
		std::string savedApplicationDataDirectory;

		std::unique_ptr<GlobalConfig> globalConfig;
		std::unique_ptr<FlashingService> flasher;

		/* Only reachable through startViewerDataHandler(). Declared after the
		   handlers it points at, so it is destroyed before them. */
		std::atomic<bool> applicationDone{false};
		std::mutex probeMutex;
		PlotGroupHandler plotGroupHandler;
		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotHandler tracePlotHandler;
		std::unique_ptr<ViewerDataHandler> viewerDataHandler;

		McpContext context;
		mcp::ToolRegistry registry;
	};

	TEST_F(FlashingToolsTest, EveryToolSaysWhichSubsystemIsMissing)
	{
		context.flasher = nullptr;

		for (const std::string& name : {"flash", "get_flash_status", "abort_flash", "get_flash_settings", "set_flash_settings"})
		{
			try
			{
				call(name);
				FAIL() << "expected a ToolError from " << name;
			}
			catch (const mcp::ToolError& error)
			{
				/* The message points at get_instance_info, which is how a caller
				   learns what this build does have. */
				EXPECT_NE(std::string(error.what()).find("flashing"), std::string::npos) << name;
			}
		}
	}

	TEST_F(FlashingToolsTest, TheStatusOfARunThatNeverHappenedIsIdle)
	{
		const Json status = call("get_flash_status");

		EXPECT_EQ(status.at("state").get<std::string>(), "idle");
		EXPECT_EQ(status.at("output").get<std::string>(), "");
	}

	TEST_F(FlashingToolsTest, AbortWithNothingRunningSaysSo)
	{
		try
		{
			call("abort_flash");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_EQ(std::string(error.what()), "No flash in progress.");
		}
	}

	TEST_F(FlashingToolsTest, FlashWithoutACommandSaysWhereToSetIt)
	{
		try
		{
			call("flash");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_EQ(std::string(error.what()), "No flash command configured. Provide 'command' or set it in Options -> Flashing.");
		}
	}

	TEST_F(FlashingToolsTest, FlashWithoutAFirmwareFileSaysWhereToSetIt)
	{
		globalConfig->getSettings().flash.command = echoCommand();

		try
		{
			call("flash");
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_EQ(std::string(error.what()), "No firmware file configured. Provide 'file' or set it in Options -> Flashing.");
		}
	}

	TEST_F(FlashingToolsTest, TheElfFileOfTheAcquisitionSettingsIsUsedWhenItIsAskedFor)
	{
		globalConfig->getSettings().flash.command = echoCommand();
		globalConfig->getSettings().flash.useElfFile = true;

		/* The setting wins over the path in the box, which is what ticking it
		   means, so the file has to come from the acquisition settings. */
		context.getElfPath = []()
		{ return std::string("build/target.elf"); };

		const Json started = call("flash");
		EXPECT_EQ(started.at("state").get<std::string>(), "running");
		EXPECT_EQ(started.at("file").get<std::string>(), "build/target.elf");
		EXPECT_NE(started.at("command_line").get<std::string>().find("build/target.elf"), std::string::npos);

		EXPECT_EQ(waitForState("success"), "success");
	}

	TEST_F(FlashingToolsTest, ASuccessfulRunIsReportedWithItsOutput)
	{
		/* The template has no {file} marker, so the firmware path is appended the
		   way a bare programmer invocation needs it to be. */
		const Json started = call("flash", Json::object({{"command", "echo hello"}, {"file", "fw.hex"}}));

		EXPECT_EQ(started.at("state").get<std::string>(), "running");
		EXPECT_EQ(started.at("file").get<std::string>(), "fw.hex");
		EXPECT_EQ(started.at("command_line").get<std::string>(), "echo hello fw.hex");
		EXPECT_FALSE(started.at("acquisition_stopped").get<bool>());
		EXPECT_NE(started.at("message").get<std::string>().find("get_flash_status"), std::string::npos);

		EXPECT_EQ(waitForState("success"), "success");

		const Json status = call("get_flash_status");

		EXPECT_EQ(status.at("state").get<std::string>(), "success");
		EXPECT_EQ(status.at("output").get<std::string>(), "hello fw.hex\n");
		EXPECT_EQ(status.at("exit_code").get<int>(), 0);
		EXPECT_FALSE(status.contains("error"));

		/* The result stays available after the run, so a client that polls late
		   still learns the outcome instead of seeing an idle state. */
		EXPECT_GT(status.at("duration_s").get<double>(), 0.0);
	}

	TEST_F(FlashingToolsTest, ASecondRunIsRefusedWhileTheFirstIsInFlight)
	{
		const Json started = call("flash", Json::object({{"command", quietCommand()}, {"file", "fw.hex"}}));
		ASSERT_EQ(started.at("state").get<std::string>(), "running");

		try
		{
			call("flash", Json::object({{"command", echoCommand()}, {"file", "fw.hex"}}));
			FAIL() << "expected a ToolError";
		}
		catch (const mcp::ToolError& error)
		{
			EXPECT_EQ(std::string(error.what()), "Flash already in progress. Call abort_flash first or wait for it to finish.");
		}

		ASSERT_TRUE(flasher->abort());
		EXPECT_EQ(waitForState("aborted"), "aborted");

		EXPECT_EQ(call("get_flash_status").at("state").get<std::string>(), "aborted");
		EXPECT_EQ(call("get_flash_status").at("error").get<std::string>(), "Flash aborted by user.");
	}

	TEST_F(FlashingToolsTest, TheAcquisitionIsStoppedBeforeTheRunStarts)
	{
		startViewerDataHandler();
		viewerDataHandler->setState(DataHandlerBase::State::RUN);

		const Json started = call("flash", Json::object({{"command", echoCommand()}, {"file", "fw.hex"}}));

		/* A flash erases and rewrites the target, so nothing may be reading it
		   while it happens and the caller is told that it was stopped. */
		EXPECT_TRUE(started.at("acquisition_stopped").get<bool>());
		EXPECT_EQ(viewerDataHandler->getStateImmediate(), DataHandlerBase::State::STOP);

		EXPECT_EQ(waitForState("success"), "success");
	}

	TEST_F(FlashingToolsTest, AStoppedAcquisitionIsNotReportedAsStopped)
	{
		startViewerDataHandler();

		const Json started = call("flash", Json::object({{"command", echoCommand()}, {"file", "fw.hex"}}));

		EXPECT_FALSE(started.at("acquisition_stopped").get<bool>());

		EXPECT_EQ(waitForState("success"), "success");
	}

	TEST_F(FlashingToolsTest, ATimeoutIsReportedAsSuch)
	{
		globalConfig->getSettings().flash.command = quietCommand();
		globalConfig->getSettings().flash.file = "fw.hex";
		globalConfig->getSettings().flash.timeoutEnabled = true;
		globalConfig->getSettings().flash.timeoutSeconds = 1;

		ASSERT_EQ(call("flash").at("state").get<std::string>(), "running");

		EXPECT_EQ(waitForState("failed"), "failed");

		const Json status = call("get_flash_status");
		EXPECT_TRUE(status.at("timed_out").get<bool>());
		EXPECT_NE(status.at("error").get<std::string>().find("Flash timed out: no output for 1 s"), std::string::npos);
	}

	TEST_F(FlashingToolsTest, SetFlashSettingsUpdatesOnlyTheFieldsItIsGiven)
	{
		globalConfig->getSettings().flash.command = "old command";
		globalConfig->getSettings().flash.file = "old.hex";

		const Json updated = call("set_flash_settings", Json::object({{"file", "new.hex"}, {"timeout_enabled", true}, {"timeout_seconds", 30}}));

		EXPECT_EQ(updated.at("command").get<std::string>(), "old command");
		EXPECT_EQ(updated.at("file").get<std::string>(), "new.hex");
		EXPECT_TRUE(updated.at("timeout_enabled").get<bool>());
		EXPECT_EQ(updated.at("timeout_seconds").get<int>(), 30);
		EXPECT_FALSE(updated.at("use_elf_file").get<bool>());

		/* Written through to the file, because the settings belong to the
		   installation rather than to the running session. */
		EXPECT_TRUE(std::filesystem::exists(GlobalConfig::getGlobalConfigPath()));

		GlobalConfig reloaded(logger.get());
		ASSERT_TRUE(reloaded.load());
		EXPECT_EQ(reloaded.getSettings().flash.file, "new.hex");
		EXPECT_EQ(reloaded.getSettings().flash.command, "old command");
		EXPECT_EQ(reloaded.getSettings().flash.timeoutSeconds, 30);
	}

	TEST_F(FlashingToolsTest, ATimeoutOutsideTheAllowedRangeIsRefused)
	{
		EXPECT_THROW(call("set_flash_settings", Json::object({{"timeout_seconds", 0}})), mcp::ToolError);
		EXPECT_THROW(call("set_flash_settings", Json::object({{"timeout_seconds", 3601}})), mcp::ToolError);

		/* The refusal leaves the stored value alone. */
		EXPECT_EQ(globalConfig->getSettings().flash.timeoutSeconds, FlashingService::FlashSettings{}.timeoutSeconds);
	}

	TEST_F(FlashingToolsTest, GetFlashSettingsReportsTheElfPathItWouldUse)
	{
		context.getElfPath = []()
		{ return std::string("build/target.elf"); };

		EXPECT_EQ(call("get_flash_settings").at("elf_file").get<std::string>(), "build/target.elf");
	}

	TEST_F(FlashingToolsTest, EveryFlashingToolRunsOffTheGuiThread)
	{
		/* A run takes minutes and a poll has to answer while a dialog is open, so
		   none of these may be handed to the interface thread. */
		for (const std::string& name : {"flash", "get_flash_status", "abort_flash", "get_flash_settings", "set_flash_settings"})
		{
			const mcp::Tool* tool = registry.find(name);
			ASSERT_NE(tool, nullptr) << name;
			EXPECT_FALSE(tool->onGuiThread) << name;
		}
	}
}  // namespace
