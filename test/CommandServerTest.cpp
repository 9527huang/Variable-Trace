#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "CommandServer.hpp"
#include "McpBridge.hpp"
#include "McpContext.hpp"
#include "McpServer.hpp"
#include "McpTypes.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The command channel speaks one JSON line in and one JSON line out. The
 * parsing and the reply shapes are what the tests here cover, because those are
 * what a client written against the protocol depends on. A socket that is
 * actually bound and read from is exercised by the manual run.
 *
 * The socket headers are reached through CommandServer.hpp rather than included
 * here, for the same reason the HTTP library stays out of McpServerTest.cpp:
 * this file would otherwise take the object past the section count a COFF
 * object can address.
 */
namespace
{
	using mcp::Json;

	class CommandServerTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::null_sink_mt>());

			server = std::make_unique<mcp::Server>(&context, &bridge, logger.get());
			command = std::make_unique<CommandServer>(server.get(), logger.get());
		}

		void TearDown() override
		{
			stopGuiThread();

			command.reset();
			server.reset();
		}

		/*
		 * Stands in for the frame loop.
		 *
		 * A tool that touches the model is handed to the GUI thread and the
		 * caller waits for it. In the application that thread is the one drawing
		 * the window; here nothing draws, so the tasks would sit in the queue
		 * until the call timed out. This pumps the queue from a thread of its
		 * own, which is what the window would be doing.
		 */
		void startGuiThread()
		{
			if (guiThread.joinable())
				return;

			guiRunning.store(true);

			guiThread = std::thread(
				[this]()
				{
					while (guiRunning.load())
					{
						bridge.processPendingTasks();
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
				});
		}

		void stopGuiThread()
		{
			guiRunning.store(false);

			if (guiThread.joinable())
				guiThread.join();
		}

		/* Runs one request line and returns the parsed reply. The trailing
		   newline is what separates the replies on a real connection, so it is
		   asserted here rather than stripped away and forgotten. */
		Json exchange(const std::string& request)
		{
			const std::string reply = command->handleCommand(request);

			EXPECT_FALSE(reply.empty());
			EXPECT_EQ(reply.back(), '\n');

			return Json::parse(reply.substr(0, reply.size() - 1));
		}

		McpContext context;
		mcp::Bridge bridge;
		std::shared_ptr<spdlog::logger> logger;
		std::unique_ptr<mcp::Server> server;
		std::unique_ptr<CommandServer> command;

		std::thread guiThread;
		std::atomic<bool> guiRunning{false};
	};

	TEST_F(CommandServerTest, ARequestThatIsNotJsonIsRefused)
	{
		const Json reply = exchange("this is not json");

		EXPECT_FALSE(reply.at("ok").get<bool>());
		EXPECT_NE(reply.at("error").get<std::string>().find("not valid JSON"), std::string::npos);
	}

	TEST_F(CommandServerTest, ARequestWithoutAToolNameIsRefused)
	{
		const Json reply = exchange(R"({"arguments":{"name":"a"}})");

		EXPECT_FALSE(reply.at("ok").get<bool>());
		EXPECT_NE(reply.at("error").get<std::string>().find("'tool'"), std::string::npos);
	}

	TEST_F(CommandServerTest, AToolThatDoesNotExistIsRefused)
	{
		const Json reply = exchange(R"({"tool":"no_such_tool"})");

		EXPECT_FALSE(reply.at("ok").get<bool>());
		EXPECT_NE(reply.at("error").get<std::string>().find("no_such_tool"), std::string::npos);
	}

	TEST_F(CommandServerTest, ArgumentsThatAreNotAnObjectAreRefused)
	{
		const Json reply = exchange(R"({"tool":"get_instance_info","arguments":42})");

		EXPECT_FALSE(reply.at("ok").get<bool>());
		EXPECT_NE(reply.at("error").get<std::string>().find("'arguments'"), std::string::npos);
	}

	/* An argument the tool does not declare is a typo, and a typo that is
	   ignored is worse than one that is refused: the caller would be told that
	   a setting it never changed had been applied. */
	TEST_F(CommandServerTest, AnUnknownArgumentIsRefusedAndTheAcceptedOnesAreListed)
	{
		const Json reply = exchange(R"({"tool":"get_variable_value","arguments":{"name":"a","nmae":"b"}})");

		EXPECT_FALSE(reply.at("ok").get<bool>());

		const std::string error = reply.at("error").get<std::string>();
		EXPECT_NE(error.find("nmae"), std::string::npos);
		EXPECT_NE(error.find("name"), std::string::npos);
	}

	/* The reply carries the payload itself rather than the text envelope an MCP
	   client is sent, which is what makes the channel usable from a script. */
	TEST_F(CommandServerTest, ASuccessfulCallReturnsThePayloadObject)
	{
		startGuiThread();

		const Json reply = exchange(R"({"tool":"get_instance_info"})");

		ASSERT_TRUE(reply.at("ok").get<bool>()) << reply.dump();
		ASSERT_TRUE(reply.contains("result"));
		EXPECT_TRUE(reply.at("result").is_object());
		EXPECT_FALSE(reply.contains("error"));

		stopGuiThread();
	}

	/* A tool that fails reports the reason in the error field rather than
	   throwing, so a client can read what went wrong and try something else. */
	TEST_F(CommandServerTest, AToolThatFailsReportsTheReasonInsteadOfThrowing)
	{
		startGuiThread();

		const Json reply = exchange(R"({"tool":"get_flash_status"})");

		/* What the failure is depends on which subsystems the test build wires
		   up; that it comes back as a reported error and not as an exception is
		   the part the channel guarantees. */
		EXPECT_FALSE(reply.at("ok").get<bool>());
		EXPECT_TRUE(reply.contains("error"));
		EXPECT_TRUE(reply.at("error").is_string());
		EXPECT_FALSE(reply.contains("result"));

		stopGuiThread();
	}

	TEST_F(CommandServerTest, SpacedAndEmptyLinesAreNotCountedAsRequests)
	{
		const size_t before = command->getRequestCount();

		/* A line of only spaces is not a request, so it must not move the counter
		   the status line reports. */
		const Json reply = exchange("   ");

		EXPECT_FALSE(reply.at("ok").get<bool>());
		EXPECT_EQ(command->getRequestCount(), before);
	}

	TEST_F(CommandServerTest, EveryHandledRequestIsCounted)
	{
		const size_t before = command->getRequestCount();

		/* The count is taken before the tool is looked up, so a call that fails
		   still counts: it says how much traffic the channel carried. */
		exchange(R"({"tool":"no_such_tool"})");
		exchange(R"({"tool":"no_such_tool"})");

		EXPECT_EQ(command->getRequestCount(), before + 2);
	}

	TEST_F(CommandServerTest, TheEndpointIsEmptyWhileNothingIsListening)
	{
		EXPECT_FALSE(command->isRunning());
		EXPECT_EQ(command->getPort(), 0);
		EXPECT_EQ(command->getEndpoint(), "");
	}

	TEST_F(CommandServerTest, ADisabledChannelDoesNotBindAPort)
	{
		EXPECT_FALSE(command->start(false, 0));
		EXPECT_FALSE(command->isRunning());
		EXPECT_EQ(command->getPort(), 0);
	}

	TEST_F(CommandServerTest, StartingBindsAPortAndStoppingReleasesIt)
	{
		ASSERT_TRUE(command->start(true, 0)) << command->getLastError();
		const uint16_t bound = command->getPort();

		/* Zero means "pick a port", and the one that is picked is the documented
		   default. Anything the channel binds has to be above the privileged
		   range in any case. */
		EXPECT_GE(bound, 1024);
		EXPECT_TRUE(command->isRunning());
		EXPECT_NE(command->getEndpoint().find("127.0.0.1"), std::string::npos);

		command->stop();

		EXPECT_FALSE(command->isRunning());
		EXPECT_EQ(command->getPort(), 0);
		EXPECT_EQ(command->getEndpoint(), "");
	}

	/* Loading a project asks for a restart with the project's settings, so the
	   listener has to be rebuilt rather than left where it was. */
	TEST_F(CommandServerTest, RestartingRebuildsTheListener)
	{
		ASSERT_TRUE(command->start(true, 0)) << command->getLastError();
		ASSERT_GE(command->getPort(), 1024);

		command->restart(true, 0);

		EXPECT_TRUE(command->isRunning());
		EXPECT_GE(command->getPort(), 1024);

		command->restart(false, 0);

		EXPECT_FALSE(command->isRunning());
		EXPECT_EQ(command->getPort(), 0);
	}
}  // namespace
