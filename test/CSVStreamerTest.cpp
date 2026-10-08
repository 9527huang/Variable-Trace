#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "CSVStreamer.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

/*
 * The log a run writes.
 *
 * A run opens its file once and writes to it from then on. In the middle of a
 * run the acquisition asks for that file again, every time it rebuilds what it
 * reads from the target: a variable was added to a plot, a group was activated,
 * a series was dragged off one. The tests below are about what those repeated
 * calls do to a file that is already being written.
 */

namespace
{
	/* The streamer writes into the file rather than to a log, so nothing it
	   says about a failure would be seen. A null sink keeps the test output
	   clean all the same. */
	std::shared_ptr<spdlog::logger> makeSilentLogger()
	{
		return std::make_shared<spdlog::logger>("csv-streamer-test", std::make_shared<spdlog::sinks::null_sink_mt>());
	}

	/* Read back the way the file is written, so that the line endings of the
	   platform are not part of what the tests compare. */
	std::string readFile(const std::string& filePath)
	{
		std::ifstream file(filePath);

		std::ostringstream content;
		content << file.rdbuf();

		return content.str();
	}

	class CSVStreamerTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = makeSilentLogger();

			directory = std::filesystem::temp_directory_path() / "variable-trace-csv-streamer-test";
			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
			std::filesystem::create_directories(directory);
		}

		void TearDown() override
		{
			std::error_code errorCode;
			std::filesystem::remove_all(directory, errorCode);
		}

		/* The name is fixed, so the directory is the whole of what says which
		   file a run is in. */
		std::string logFileIn(const std::filesystem::path& where) const
		{
			return (where / "logfile.csv").string();
		}

		std::shared_ptr<spdlog::logger> logger;
		std::filesystem::path directory;
	};
}  // namespace

/* The file of a run is opened once and kept for the rest of it.
 *
 * A second call is what a restart in the middle of a run makes. Opening again
 * would cut off what had been written, and because a stream that is open
 * refuses to be opened a second time, the attempt would instead leave the
 * stream in a state where every line after it is dropped without a word. */
TEST_F(CSVStreamerTest, TheFileOfARunIsKeptWhenItIsAskedForAgain)
{
	CSVStreamer streamer(logger.get());

	ASSERT_TRUE(streamer.prepareFile(directory.string()));
	streamer.createHeader({"adc"});

	ASSERT_TRUE(streamer.prepareFile(directory.string()));

	std::unordered_map<std::string, double> values{{"adc", 1.5}};
	streamer.writeLine(1.0, values);
	streamer.finishLogging();

	EXPECT_EQ(readFile(logFileIn(directory)), "time,adc\n1.000000,1.500000\n");
}

/* The columns are named at the top of the file, and named again only where they
   change. A restart with the same columns would otherwise leave a second header
   in the middle of the data, which is a line a reader of the file has to step
   over. */
TEST_F(CSVStreamerTest, TheColumnsAreNamedWhereTheyChange)
{
	CSVStreamer streamer(logger.get());

	ASSERT_TRUE(streamer.prepareFile(directory.string()));

	/* The same set twice, which is what a restart with nothing new looks like,
	   and then a set that has grown by one variable. */
	streamer.createHeader({"adc"});
	streamer.createHeader({"adc"});
	streamer.createHeader({"adc", "dac"});

	streamer.finishLogging();

	EXPECT_EQ(readFile(logFileIn(directory)), "time,adc\ntime,adc,dac\n");
}

/* The log directory can be pointed somewhere else while a run is going, from
   the preferences or over the API. What has been collected stays in the file it
   was asked for, and the rest of the run carries on in the new one. */
TEST_F(CSVStreamerTest, AChangeOfLogDirectoryKeepsTheFileThatWasBeingWritten)
{
	CSVStreamer streamer(logger.get());

	const std::filesystem::path second = directory / "second";
	std::filesystem::create_directories(second);

	std::unordered_map<std::string, double> values{{"adc", 1.5}};

	ASSERT_TRUE(streamer.prepareFile(directory.string()));
	streamer.createHeader({"adc"});
	streamer.writeLine(1.0, values);

	ASSERT_TRUE(streamer.prepareFile(second.string()));
	streamer.createHeader({"adc"});
	streamer.writeLine(2.0, values);

	streamer.finishLogging();

	EXPECT_EQ(readFile(logFileIn(directory)), "time,adc\n1.000000,1.500000\n");
	EXPECT_EQ(readFile(logFileIn(second)), "time,adc\n2.000000,1.500000\n");
}
