#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "GlobalConfig.hpp"
#include "McpContext.hpp"
#include "McpToolRegistry.hpp"
#include "McpTools.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "TraceDataHandler.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The three tools that replace the whole project, called the way the server
 * calls them.
 *
 * Loading a project takes every variable, plot and group away at once and puts
 * another set in their place, while an acquisition that is running walks all
 * three. The interface offers its own New and Open entries only while nothing
 * is running, and these tests pin the same rule on the tools: a rule that only
 * one of the two doors onto an operation obeys is not a rule.
 *
 * Nothing here opens a window or reaches a target. The handlers are built for
 * the state flags they carry, and the tools are looked up in the registry and
 * invoked directly.
 */

namespace
{
	using mcp::Json;

	class ProjectToolsTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("project-tools-test", std::make_shared<spdlog::sinks::null_sink_mt>());

			/* Only ever read from. No test below saves, and that is what keeps
			   the file the user actually has out of the test. */
			globalConfig = std::make_unique<GlobalConfig>(logger.get());
			context.globalConfig = globalConfig.get();

			/* The two operations are stubbed, because what is under test is
			   whether they are reached at all. */
			context.newProject = [this]()
			{ newProjectCalls++; };

			context.openProject = [this](const std::string& path)
			{
				openProjectCalls++;
				openedPath = path;
				return true;
			};

			mcp::ProjectTools::registerAll(registry);
		}

		/* The handlers are built for their state flags only. Their sampling
		   threads read a target and there is none here, so the quit flag is
		   raised before either object exists and both threads leave at once.
		   Starting them for real would fail on the first read rather than say
		   anything about the tools. */
		void startHandlers()
		{
			applicationDone = true;

			viewerDataHandler = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &probeMutex, logger.get());
			traceDataHandler = std::make_unique<TraceDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &probeMutex, logger.get());

			context.viewerDataHandler = viewerDataHandler.get();
			context.traceDataHandler = traceDataHandler.get();
		}

		Json call(const std::string& name, const Json& arguments = Json::object({}))
		{
			const mcp::Tool* tool = registry.find(name);

			if (tool == nullptr)
				throw mcp::ToolError("no tool named " + name);

			return tool->handler(context, arguments);
		}

		/* A client has only the text of the failure to correct itself with, so
		   the message has to name the step it skipped. */
		void expectRefusedWhileRunning(const std::string& tool, const Json& arguments)
		{
			try
			{
				call(tool, arguments);
				FAIL() << tool << " was expected to refuse while an acquisition is running";
			}
			catch (const mcp::ToolError& error)
			{
				EXPECT_NE(std::string(error.what()).find("stop_acquisition"), std::string::npos);
			}
		}

		std::shared_ptr<spdlog::logger> logger;
		std::unique_ptr<GlobalConfig> globalConfig;

		/* Declared before the handlers that point at it, so it outlives them:
		   the two threads read this flag on their way out. */
		std::atomic<bool> applicationDone{false};
		std::mutex probeMutex;
		PlotGroupHandler plotGroupHandler;
		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotHandler tracePlotHandler;
		std::unique_ptr<ViewerDataHandler> viewerDataHandler;
		std::unique_ptr<TraceDataHandler> traceDataHandler;

		uint32_t newProjectCalls = 0;
		uint32_t openProjectCalls = 0;
		std::string openedPath;

		McpContext context;
		mcp::ToolRegistry registry;
	};

	TEST_F(ProjectToolsTest, ANewProjectIsRefusedWhileTheViewerIsSampling)
	{
		startHandlers();
		viewerDataHandler->setState(DataHandlerBase::State::RUN);

		expectRefusedWhileRunning("new_project", Json::object({}));

		/* The model is untouched, which is the whole point: the refusal is what
		   stands between a running reader and a rebuilt model. */
		EXPECT_EQ(newProjectCalls, 0u);
	}

	TEST_F(ProjectToolsTest, ANewProjectIsRefusedWhileTheTraceAcquisitionIsRunning)
	{
		startHandlers();
		traceDataHandler->setState(DataHandlerBase::State::RUN);

		expectRefusedWhileRunning("new_project", Json::object({}));

		EXPECT_EQ(newProjectCalls, 0u);
	}

	TEST_F(ProjectToolsTest, ANewProjectGoesThroughOnceNothingIsRunning)
	{
		startHandlers();

		EXPECT_TRUE(call("new_project").at("new").get<bool>());
		EXPECT_EQ(newProjectCalls, 1u);
	}

	TEST_F(ProjectToolsTest, OpeningAProjectIsRefusedWhileTheViewerIsSampling)
	{
		startHandlers();
		viewerDataHandler->setState(DataHandlerBase::State::RUN);

		expectRefusedWhileRunning("open_project", Json::object({{"path", "some.mcvproj"}}));

		EXPECT_EQ(openProjectCalls, 0u);
	}

	/* The refusal comes before the picker. Without that, a client whose
	   acquisition is running would be walked through a dialog that can only
	   end in the same refusal. */
	TEST_F(ProjectToolsTest, TheFilePickerIsNotReachedWhileTheViewerIsSampling)
	{
		startHandlers();
		viewerDataHandler->setState(DataHandlerBase::State::RUN);

		expectRefusedWhileRunning("open_project", Json::object({}));

		EXPECT_EQ(openProjectCalls, 0u);
	}

	TEST_F(ProjectToolsTest, OpeningAProjectGoesThroughOnceNothingIsRunning)
	{
		startHandlers();

		const Json opened = call("open_project", Json::object({{"path", "some.mcvproj"}}));

		EXPECT_TRUE(opened.at("opened").get<bool>());
		EXPECT_EQ(openProjectCalls, 1u);
		EXPECT_EQ(openedPath, "some.mcvproj");
	}

	TEST_F(ProjectToolsTest, OpeningARecentProjectIsRefusedWhileTheViewerIsSampling)
	{
		startHandlers();
		viewerDataHandler->setState(DataHandlerBase::State::RUN);

		globalConfig->addRecentProject("some.mcvproj");

		expectRefusedWhileRunning("open_recent_project", Json::object({{"index", 0}}));

		EXPECT_EQ(openProjectCalls, 0u);
	}

	TEST_F(ProjectToolsTest, OpeningARecentProjectGoesThroughOnceNothingIsRunning)
	{
		startHandlers();

		globalConfig->addRecentProject("some.mcvproj");

		const Json opened = call("open_recent_project", Json::object({{"index", 0}}));

		EXPECT_TRUE(opened.at("opened").get<bool>());
		EXPECT_EQ(openProjectCalls, 1u);
	}
}  // namespace
