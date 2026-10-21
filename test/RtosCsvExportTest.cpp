#include "RtosCsvExport.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
	RtosSnapshot makeRound(double atSeconds, bool withStackReadings)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = atSeconds;
		snapshot.captureFinishedAt = atSeconds + 0.05;
		snapshot.captureSpanSeconds = 0.05;
		snapshot.consistency = RtosConsistency::BestEffort;
		snapshot.listsWalked = 8;
		snapshot.queueRegistryAvailable = true;
		snapshot.sessionId = "session-1";

		snapshot.stackScan.enabled = true;
		snapshot.stackScan.fillSeen = withStackReadings;
		snapshot.stackScan.tasksScanned = withStackReadings ? 2 : 0;
		snapshot.stackScan.oldestReadingAgeSeconds = 0.5;

		snapshot.cpuLoad.supported = true;
		snapshot.cpuLoad.counting = true;
		snapshot.cpuLoad.tasksMeasured = 2;
		snapshot.cpuLoad.tasksKnown = 2;
		snapshot.cpuLoad.windowSeconds = 1.75;

		for (uint32_t index = 0; index < 2; index++)
		{
			RtosTask task;

			task.tcbAddress = 0x20001000u + index * 0x40u;
			task.name = "task" + std::to_string(index);
			task.state = index == 0 ? RtosTaskState::Running : RtosTaskState::Blocked;
			task.rawPriority = 3 - index;
			task.hasPriority = true;
			task.priorityRank = 3 - index;
			task.basePriority = 1;
			task.hasBasePriority = true;
			task.stackBase = 0x20002000u;
			task.stackTop = task.stackBase + 512;
			task.hasStack = true;
			task.hasStackWatermark = withStackReadings;
			task.stackFreeBytes = 200 + index;
			task.stackWatermarkStackBytes = 512;
			task.stackWatermarkAgeSeconds = 0.25 * index;
			task.hasCpuShare = true;
			task.cpuSharePercent = 10.0 * index;
			task.waitingOn = index == 0 ? "" : "sem_task";
			task.sourceList = index == 0 ? "ready" : "suspended";
			task.hasEventList = index != 0;
			task.eventList = index == 0 ? 0 : 0x20003000u;

			snapshot.tasks.push_back(task);
		}

		return snapshot;
	}

	RtosTaskRow makeRow()
	{
		RtosTaskRow row;
		row.tcbAddress = 0x20001B60u;
		row.name = "worker";
		row.state = RtosTaskState::Blocked;
		row.hasReading = true;
		row.hasPriority = true;
		row.priority = 3;
		row.hasBasePriority = true;
		row.basePriority = 3;
		row.hasStack = true;
		row.stackBytes = 504;
		row.hasStackWatermark = true;
		row.stackFreeBytes = 368;
		row.stackWatermarkStackBytes = 504;
		row.hasCpuShare = true;
		row.cpuSharePercent = 12.5;
		row.waitingOn = "sem_stuck";
		row.sourceList = "blocked";

		return row;
	}

	std::vector<std::string> splitFields(const std::string& written)
	{
		/* The row ends with a newline; the field parser is about the row. */
		std::string line = written;

		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
			line.pop_back();

		std::vector<std::string> fields;
		std::string current;
		bool quoted = false;

		for (size_t index = 0; index < line.size(); index++)
		{
			const char character = line[index];

			if (quoted)
			{
				if (character == '"' && index + 1 < line.size() && line[index + 1] == '"')
				{
					current += '"';
					index++;
				}
				else if (character == '"')
					quoted = false;
				else
					current += character;
			}
			else if (character == '"')
				quoted = true;
			else if (character == ',')
			{
				fields.push_back(current);
				current.clear();
			}
			else
				current += character;
		}

		fields.push_back(current);

		return fields;
	}
}

TEST(RtosCsvExportTest, HeaderHasThirteenColumnsAndNoNewline)
{
	const std::string header = RtosCsvExport::header();

	EXPECT_EQ(13u, splitFields(header).size());
	EXPECT_EQ(std::string::npos, header.find('\n'));
	EXPECT_EQ("round", splitFields(header).front());
	EXPECT_EQ("waiting_on", splitFields(header).back());
}

TEST(RtosCsvExportTest, WritesOneLinePerTaskWithTheTablesNumbers)
{
	std::ostringstream out;

	const size_t written = RtosCsvExport::writeRound(out, 7, 12.3456, {makeRow()});

	EXPECT_EQ(1u, written);

	const std::string line = out.str();
	EXPECT_EQ(line.size() - 1, line.find_last_of('\n'));

	const std::vector<std::string> fields = splitFields(line);
	ASSERT_EQ(13u, fields.size());

	EXPECT_EQ("7", fields[0]);
	EXPECT_EQ("12.3456", fields[1]);
	EXPECT_EQ("worker", fields[2]);
	EXPECT_EQ("0x20001B60", fields[3]);
	EXPECT_EQ("blocked", fields[4]);
	EXPECT_EQ("blocked", fields[5]);
	EXPECT_EQ("3", fields[6]);
	EXPECT_EQ("3", fields[7]);
	EXPECT_EQ("136", fields[8]);
	EXPECT_EQ("504", fields[9]);
	EXPECT_EQ("73", fields[10]);
	EXPECT_EQ("12.5", fields[11]);
	EXPECT_EQ("sem_stuck", fields[12]);
}

TEST(RtosCsvExportTest, LeavesAFieldTheRoundDidNotMeasureEmptyRatherThanZero)
{
	RtosTaskRow row = makeRow();
	row.hasStackWatermark = false;
	row.hasCpuShare = false;
	row.hasPriority = false;
	row.hasBasePriority = false;

	std::ostringstream out;
	RtosCsvExport::writeRound(out, 0, 1.0, {row});

	const std::vector<std::string> fields = splitFields(out.str());
	ASSERT_EQ(13u, fields.size());

	/* Three stack columns, the share, and both priorities: empty, and the row
	   still has thirteen of them. A zero in any of these is a claim. */
	EXPECT_TRUE(fields[6].empty());
	EXPECT_TRUE(fields[7].empty());
	EXPECT_TRUE(fields[8].empty());
	EXPECT_TRUE(fields[9].empty());
	EXPECT_TRUE(fields[10].empty());
	EXPECT_TRUE(fields[11].empty());
}

TEST(RtosCsvExportTest, KeepsIdentityForATaskTheRoundDidNotRead)
{
	RtosTaskRow row = makeRow();
	row.hasReading = false;
	row.state = RtosTaskState::Unknown;
	row.hasStackWatermark = false;
	row.hasCpuShare = false;
	row.waitingOn.clear();
	row.sourceList.clear();

	std::ostringstream out;
	RtosCsvExport::writeRound(out, 3, 4.0, {row});

	const std::vector<std::string> fields = splitFields(out.str());

	/* Name and address are identity, not a reading: a row nobody can attribute
	   is worse than a row with holes in it. */
	EXPECT_EQ("worker", fields[2]);
	EXPECT_EQ("0x20001B60", fields[3]);
	EXPECT_TRUE(fields[4].empty());
}

TEST(RtosCsvExportTest, QuotesAFieldThatWouldBreakTheRow)
{
	std::ostringstream out;

	RtosTaskRow row = makeRow();
	row.name = "queue, worker";
	RtosCsvExport::writeRound(out, 0, 1.0, {row});

	const std::vector<std::string> fields = splitFields(out.str());
	ASSERT_EQ(13u, fields.size());
	EXPECT_EQ("queue, worker", fields[2]);
}

TEST(RtosCsvExportTest, DoublesTheQuoteInsideAQuotedField)
{
	EXPECT_EQ("\"say \"\"hi\"\"\"", RtosCsvExport::field("say \"hi\""));
}

TEST(RtosCsvExportTest, LeavesAPlainFieldAlone)
{
	EXPECT_EQ("sem_stuck", RtosCsvExport::field("sem_stuck"));
	EXPECT_EQ("-", RtosCsvExport::field("-"));
}

TEST(RtosCsvExportTest, WritesNothingForARoundWithoutTasks)
{
	std::ostringstream out;

	EXPECT_EQ(0u, RtosCsvExport::writeRound(out, 2, 3.0, {}));
	EXPECT_TRUE(out.str().empty());
}

TEST(RtosCsvExportTest, WritesEveryRoundOfAnOpenRecording)
{
	const std::filesystem::path directory = std::filesystem::temp_directory_path() / "variable-trace-csv-export-test";
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);

	const std::string file = (directory / "session.vtrec").string();

	{
		RtosRecording writer;
		RtosRecording::Problem problem = RtosRecording::Problem::None;

		ASSERT_TRUE(writer.begin(file, problem)) << RtosRecording::describe(problem);

		/* The second round was read before the stack sweep reached anything, so
		   its stack columns must come out empty rather than zero. */
		writer.write(makeRound(10.0, true));
		writer.write(makeRound(10.5, false));

		writer.finish();
	}

	RtosModel model;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(model.openRecording(file, problem)) << RtosRecording::describe(problem);
	ASSERT_TRUE(model.isPlayback());

	std::ostringstream out;
	const RtosCsvExport::Counts counts = RtosCsvExport::writeRecording(out, model);

	EXPECT_EQ(2u, counts.rounds);
	EXPECT_EQ(4u, counts.rows);
	EXPECT_EQ(0u, counts.skipped);

	std::istringstream lines(out.str());
	std::vector<std::string> all;

	for (std::string line; std::getline(lines, line);)
		all.push_back(line);

	/* Header plus two rows per round. */
	ASSERT_EQ(5u, all.size());
	EXPECT_EQ(std::string(RtosCsvExport::header()), all[0]);

	/* Found by name rather than by line: the table's order is the table's
	   business, and the export follows it. */
	std::vector<std::string> firstRound;
	std::vector<std::string> secondRound;

	for (size_t index = 1; index < all.size(); index++)
	{
		const std::vector<std::string> fields = splitFields(all[index]);

		ASSERT_EQ(13u, fields.size());

		if (fields[2] != "task0")
			continue;

		if (fields[0] == "0")
			firstRound = fields;
		else
			secondRound = fields;
	}

	ASSERT_EQ(13u, firstRound.size());
	EXPECT_EQ("running", firstRound[4]);
	EXPECT_EQ("312", firstRound[8]);

	ASSERT_EQ(13u, secondRound.size());
	EXPECT_TRUE(secondRound[8].empty());
	EXPECT_TRUE(secondRound[9].empty());
	EXPECT_TRUE(secondRound[10].empty());

	model.closePlayback();
	std::filesystem::remove_all(directory);
}
