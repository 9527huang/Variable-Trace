#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "FlashingService.hpp"

/*
 * The service runs a real child process, so these tests use commands every
 * shell has rather than a stand-in. What is checked is the contract around the
 * process: what the command line becomes, what the state machine reports, and
 * what happens to a run that is cut short.
 */

namespace
{
	/* A command that prints nothing for noticeably longer than the one second
	   timeout used below. The pipe has to stay quiet, otherwise every line
	   restarts the idle timer. */
	std::string quietCommand()
	{
#ifdef _WIN32
		return "ping 127.0.0.1 -n 6 >nul";
#else
		return "sleep 5";
#endif
	}

	std::string echoCommand()
	{
		return "echo hello";
	}

	std::string exitCommand(int code)
	{
		return "exit " + std::to_string(code);
	}

	/* One line per number, so that the bound on the output buffer can be
	   reached without generating a megabyte. */
	std::string manyLinesCommand(size_t lines)
	{
#ifdef _WIN32
		return "for /l %i in (1,1," + std::to_string(lines) + ") do @echo line";
#else
		return "seq 1 " + std::to_string(lines);
#endif
	}
}  // namespace

TEST(FlashingServiceTest, initialStateIsIdle)
{
	FlashingService service;

	EXPECT_EQ(service.getState(), FlashingService::State::Idle);
	EXPECT_FALSE(service.isRunning());
	EXPECT_TRUE(service.getOutput().empty());
	EXPECT_EQ(service.getLastError(), "");
	EXPECT_EQ(FlashingService::stateToString(service.getState()), "idle");
}

TEST(FlashingServiceTest, abortWithNothingRunningIsRefused)
{
	FlashingService service;

	EXPECT_FALSE(service.abort());
}

TEST(FlashingServiceTest, everyStateHasATextForm)
{
	EXPECT_EQ(FlashingService::stateToString(FlashingService::State::Idle), "idle");
	EXPECT_EQ(FlashingService::stateToString(FlashingService::State::Running), "running");
	EXPECT_EQ(FlashingService::stateToString(FlashingService::State::Success), "success");
	EXPECT_EQ(FlashingService::stateToString(FlashingService::State::Failed), "failed");
	EXPECT_EQ(FlashingService::stateToString(FlashingService::State::Aborted), "aborted");
}

TEST(FlashingServiceTest, emptyCommandLineComesFromAnEmptyTemplate)
{
	EXPECT_EQ(FlashingService::buildCommandLine("", "firmware.hex"), "");
}

TEST(FlashingServiceTest, aTemplateWithoutTheMarkerGetsThePathAppended)
{
	EXPECT_EQ(FlashingService::buildCommandLine("openocd -f probe.cfg", "firmware.hex"), "openocd -f probe.cfg firmware.hex");
}

TEST(FlashingServiceTest, everyMarkerIsReplaced)
{
	EXPECT_EQ(FlashingService::buildCommandLine("prog {file} --verify {file}", "fw.hex"), "prog fw.hex --verify fw.hex");
}

TEST(FlashingServiceTest, aPathWithASpaceIsQuoted)
{
	EXPECT_EQ(FlashingService::buildCommandLine("prog {file}", "C:/my builds/fw.hex"), "prog \"C:/my builds/fw.hex\"");

	/* A template that quotes the marker has already said where the quotes go, so
	   a second pair around the path would end the argument early. */
	EXPECT_EQ(FlashingService::buildCommandLine("prog -f \"{file}\"", "C:/my builds/fw.hex"), "prog -f \"C:/my builds/fw.hex\"");
}

TEST(FlashingServiceTest, aSuccessfulRunReportsSuccessAndItsOutput)
{
	FlashingService service;

	auto future = service.startFlash(echoCommand(), "", 0);
	ASSERT_TRUE(future.valid());

	const FlashingService::FlashResult result = future.get();

	EXPECT_EQ(result.state, FlashingService::State::Success);
	EXPECT_EQ(result.exitCode, 0);
	EXPECT_FALSE(result.timedOut);
	EXPECT_EQ(service.getLastError(), "");

	const std::vector<std::string> output = service.getOutput();
	ASSERT_EQ(output.size(), 1u);

	/* The carriage return the shell prints in front of the newline must not end
	   up in the line, or every panel would draw it as a stray character. */
	EXPECT_EQ(output.front(), "hello");
	EXPECT_GT(service.getOutputRevision(), 0u);
}

TEST(FlashingServiceTest, aFailedRunKeepsItsExitCode)
{
	FlashingService service;

	auto future = service.startFlash(exitCommand(3), "", 0);
	ASSERT_TRUE(future.valid());

	const FlashingService::FlashResult result = future.get();

	EXPECT_EQ(result.state, FlashingService::State::Failed);
	EXPECT_EQ(result.exitCode, 3);
	EXPECT_TRUE(service.getOutput().empty());
	EXPECT_NE(service.getLastError().find("3"), std::string::npos);
}

TEST(FlashingServiceTest, aSecondRunIsRefusedWhileTheFirstIsInFlight)
{
	FlashingService service;

	auto future = service.startFlash(quietCommand(), "", 0);
	ASSERT_TRUE(future.valid());
	ASSERT_TRUE(service.isRunning());

	/* An invalid future is the answer to "start another one": the caller that
	   needs to tell the two refusal reasons apart asks isRunning() first. */
	EXPECT_FALSE(service.startFlash(echoCommand(), "", 0).valid());
	EXPECT_EQ(service.getState(), FlashingService::State::Running);

	ASSERT_TRUE(service.abort());
	future.get();
}

TEST(FlashingServiceTest, anAbortedRunReportsTheWayThePanelWordsIt)
{
	FlashingService service;

	auto future = service.startFlash(quietCommand(), "", 0);
	ASSERT_TRUE(future.valid());

	ASSERT_TRUE(service.abort());
	const FlashingService::FlashResult result = future.get();

	EXPECT_EQ(result.state, FlashingService::State::Aborted);
	EXPECT_EQ(result.exitCode, -1);
	EXPECT_EQ(service.getLastError(), "Flash aborted by user.");
	EXPECT_FALSE(service.isRunning());
}

TEST(FlashingServiceTest, anEmptyRunReportsNoOutputRatherThanSucceedingQuietly)
{
	FlashingService service;

	auto future = service.startFlash(exitCommand(0), "", 0);
	ASSERT_TRUE(future.valid());

	EXPECT_EQ(future.get().state, FlashingService::State::Success);
	EXPECT_TRUE(service.getOutput().empty());
}

TEST(FlashingServiceTest, aRunThatStopsPrintingIsCutShort)
{
	FlashingService service;

	auto future = service.startFlash(quietCommand(), "", 1);
	ASSERT_TRUE(future.valid());

	const FlashingService::FlashResult result = future.get();

	EXPECT_EQ(result.state, FlashingService::State::Failed);
	EXPECT_TRUE(result.timedOut);
	EXPECT_NE(service.getLastError().find("Flash timed out: no output for 1 s"), std::string::npos);

	/* A timeout is bounded by the idle time, not by how long the command would
	   have run, which is the whole point of the setting. */
	EXPECT_LT(result.durationSeconds, 3.0);
}

TEST(FlashingServiceTest, theOutputBufferKeepsTheTailAndCountsWhatItDropped)
{
	FlashingService service;

	const size_t lines = FlashingService::maximumOutputLines + 250;

	auto future = service.startFlash(manyLinesCommand(lines), "", 0);
	ASSERT_TRUE(future.valid());

	EXPECT_EQ(future.get().state, FlashingService::State::Success);

	EXPECT_EQ(service.getOutput().size(), FlashingService::maximumOutputLines);
	EXPECT_EQ(service.getDroppedLineCount(), 250u);

	/* The count grows by one per line whether it was kept or dropped, which is
	   what a panel compares against to decide whether to copy the buffer. */
	EXPECT_EQ(service.getOutputRevision(), lines);
}

TEST(FlashingServiceTest, theServiceWorksWithoutALogger)
{
	FlashingService service(nullptr);

	auto future = service.startFlash(echoCommand(), "", 0);
	ASSERT_TRUE(future.valid());

	EXPECT_EQ(future.get().state, FlashingService::State::Success);
}

TEST(FlashingServiceTest, theFileIsHandedToTheCommand)
{
	FlashingService service;

	/* The path travels through {file}, so a command that echoes its argument
	   shows what the programmer would have been given. */
	const std::string command = FlashingService::buildCommandLine(echoCommand() + " {file}", "fw.hex");
	ASSERT_EQ(command, "echo hello fw.hex");

	auto future = service.startFlash(command, "", 0);
	ASSERT_TRUE(future.valid());
	ASSERT_EQ(future.get().state, FlashingService::State::Success);

	const std::vector<std::string> output = service.getOutput();
	ASSERT_EQ(output.size(), 1u);
	EXPECT_EQ(output.front(), "hello fw.hex");
}
