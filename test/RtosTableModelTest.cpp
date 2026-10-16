#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RtosTableModel.hpp"

namespace
{
	RtosTask makeTask(const std::string& name, uint32_t address, RtosTaskState state, uint32_t priority, uint32_t stackBytes)
	{
		RtosTask task;

		task.name = name;
		task.tcbAddress = address;
		task.state = state;
		task.hasPriority = true;
		task.rawPriority = priority;
		task.priorityRank = priority;
		task.basePriority = priority;
		task.hasBasePriority = true;
		task.hasStack = true;
		task.stackBase = 0x20001000;
		task.stackTop = task.stackBase + stackBytes;

		return task;
	}

	RtosSnapshot makeSnapshot()
	{
		RtosSnapshot snapshot;

		snapshot.tasks.push_back(makeTask("IDLE", 0x20000200, RtosTaskState::Ready, 0, 504));
		snapshot.tasks.push_back(makeTask("led", 0x20000100, RtosTaskState::Ready, 2, 504));
		snapshot.tasks.push_back(makeTask("prod", 0x20000300, RtosTaskState::Blocked, 3, 504));
		snapshot.tasks.push_back(makeTask("work", 0x20000400, RtosTaskState::Suspended, 2, 1016));
		snapshot.tasks.push_back(makeTask("deep", 0x20000500, RtosTaskState::Blocked, 1, 760));

		return snapshot;
	}

	std::vector<std::string> namesOf(const std::vector<RtosTaskRow>& rows)
	{
		std::vector<std::string> names;

		for (const RtosTaskRow& row : rows)
			names.push_back(row.name);

		return names;
	}
}  // namespace

TEST(RtosTableModel, ReturnsEveryTaskWhenNothingIsAskedFor)
{
	const RtosSnapshot snapshot = makeSnapshot();

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, {});

	EXPECT_EQ(rows.size(), snapshot.tasks.size());
}

TEST(RtosTableModel, SortsByNameByDefault)
{
	const RtosSnapshot snapshot = makeSnapshot();

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, {})), (std::vector<std::string>{"IDLE", "deep", "led", "prod", "work"}));
}

TEST(RtosTableModel, SortsByAnyColumnInEitherDirection)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery byAddress;
	byAddress.sortColumn = RtosTaskColumn::Address;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byAddress)), (std::vector<std::string>{"led", "IDLE", "prod", "work", "deep"}));

	byAddress.descending = true;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byAddress)), (std::vector<std::string>{"deep", "work", "prod", "IDLE", "led"}));

	RtosTaskTableQuery byStack;
	byStack.sortColumn = RtosTaskColumn::StackSize;
	byStack.descending = true;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byStack)).front(), "work");
}

TEST(RtosTableModel, FiltersByState)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.states = {RtosTaskState::Blocked};

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"deep", "prod"}));
}

TEST(RtosTableModel, FiltersByPriorityFloor)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.hasPriorityFloor = true;
	query.priorityFloor = 2;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"led", "prod", "work"}));
}

TEST(RtosTableModel, ATaskWithNoPriorityDoesNotPassAFloor)
{
	RtosSnapshot snapshot = makeSnapshot();

	/* A task seen only on a list that carries no priority has nothing to
	   compare against. Reporting it as below the floor would be a claim about
	   a value nobody read. */
	RtosTask withoutPriority;
	withoutPriority.name = "suspended_one";
	withoutPriority.tcbAddress = 0x20000600;
	withoutPriority.state = RtosTaskState::Suspended;
	withoutPriority.hasPriority = false;

	snapshot.tasks.push_back(withoutPriority);

	RtosTaskTableQuery query;
	query.hasPriorityFloor = true;
	query.priorityFloor = 0;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, query);

	/* The names are taken once. `namesOf` returns a vector by value, so two
	   calls in one expression give two different vectors, and comparing a
	   begin() from one against an end() from the other is not a comparison of
	   anything. */
	const std::vector<std::string> names = namesOf(rows);

	EXPECT_EQ(std::find(names.begin(), names.end(), "suspended_one"), names.end());
}

TEST(RtosTableModel, SearchesTheNameWithoutRegardToCase)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;

	/* Mixed case on purpose, and a substring that only one name contains:
	   `LE` would match IDLE as well as led, which says nothing about case. */
	query.nameSearch = "Wo";

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"work"}));
}

TEST(RtosTableModel, ATaskWithNoNameSortsLastRatherThanFirst)
{
	RtosSnapshot snapshot = makeSnapshot();

	RtosTask unnamed;
	unnamed.tcbAddress = 0x20000700;
	unnamed.state = RtosTaskState::Suspended;

	snapshot.tasks.push_back(unnamed);

	/* An empty name is missing data. Sorting it first would put the row that
	   needs attention where it is least likely to be seen. */
	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, {})).back(), "");
}

TEST(RtosTableModel, ARowCarriesOnlyWhatWasRead)
{
	const RtosSnapshot snapshot = makeSnapshot();

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, {});

	const auto work = std::find_if(rows.begin(), rows.end(), [](const RtosTaskRow& row) { return row.name == "work"; });

	ASSERT_NE(work, rows.end());

	EXPECT_TRUE(work->hasStack);
	EXPECT_EQ(work->stackBytes, 1016u);
	EXPECT_TRUE(work->hasPriority);
	EXPECT_EQ(work->priority, 2u);

	/* The flags are what separate "not read" from "zero", and the panel needs
	   them to show a blank rather than a number it made up. */
	EXPECT_EQ(work->basePriority, work->priority);
}

TEST(RtosTableModel, TheAddressListFollowsTheRowsAndNotTheSnapshot)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.sortColumn = RtosTaskColumn::Priority;
	query.descending = true;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, query);

	const std::vector<uint32_t> addresses = RtosTableModel::addressesOf(rows);

	ASSERT_EQ(addresses.size(), rows.size());

	for (size_t index = 0; index < rows.size(); ++index)
		EXPECT_EQ(addresses[index], rows[index].tcbAddress);

	/* First by priority, so the highest one leads. */
	EXPECT_EQ(rows.front().priority, 3u);
}

TEST(RtosTableModel, TheStackColumnSortsRowsWithNoReadingLast)
{
	RtosSnapshot snapshot = makeSnapshot();

	/* Two tasks have a reading and the rest do not: the sweep goes round a few
	   tasks a round, so a table in which every row has one is the exception. */
	snapshot.tasks[0].hasStackWatermark = true;
	snapshot.tasks[0].stackFreeBytes = 400;
	snapshot.tasks[0].stackWatermarkStackBytes = 504;
	snapshot.tasks[2].hasStackWatermark = true;
	snapshot.tasks[2].stackFreeBytes = 20;
	snapshot.tasks[2].stackWatermarkStackBytes = 504;

	RtosTaskTableQuery query;
	query.sortColumn = RtosTaskColumn::StackWatermark;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, query);

	ASSERT_EQ(rows.size(), 5u);

	/* Least free first, which is the order a reader wants: the task closest to
	   the end of its stack leads. */
	EXPECT_EQ(rows[0].name, "prod");
	EXPECT_EQ(rows[0].stackUsedBytes(), 484u);
	EXPECT_EQ(rows[0].stackUsedPercent(), 96u);

	EXPECT_EQ(rows[1].name, "IDLE");
	EXPECT_EQ(rows[1].stackUsedBytes(), 104u);
	EXPECT_EQ(rows[1].stackUsedPercent(), 20u);

	/* A row with no reading goes last either way round, the way a task whose
	   name could not be read does. Missing data is not the smallest number. */
	for (size_t index = 2; index < rows.size(); ++index)
		EXPECT_FALSE(rows[index].hasStackWatermark);

	query.descending = true;

	const std::vector<RtosTaskRow> reversed = RtosTableModel::taskRows(snapshot, query);

	ASSERT_EQ(reversed.size(), 5u);
	EXPECT_EQ(reversed[0].name, "IDLE");
	EXPECT_EQ(reversed[1].name, "prod");

	for (size_t index = 2; index < reversed.size(); ++index)
		EXPECT_FALSE(reversed[index].hasStackWatermark);

	/* The size column is untouched by the scan being behind: a row can know
	   how big its stack is and not how much of it is used. */
	EXPECT_TRUE(reversed[2].hasStack);
}

TEST(RtosTableModel, AReadingThatStoppedAtTheCapIsMarked)
{
	RtosSnapshot snapshot = makeSnapshot();

	snapshot.tasks[3].hasStackWatermark = true;
	snapshot.tasks[3].stackFreeBytes = 256;
	snapshot.tasks[3].stackWatermarkStackBytes = 1016;
	snapshot.tasks[3].stackWatermarkReachedCap = true;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, {});
	const RtosTaskRow* work = nullptr;

	for (const RtosTaskRow& row : rows)
	{
		if (row.name == "work")
			work = &row;
	}

	ASSERT_NE(work, nullptr);

	/* 1016 - 256 is 760 used, but the scan stopped at its limit and only knows
	   that at least 256 bytes are free, so the used part is at most that. The
	   panel says so with a leading marker rather than presenting it as a
	   count. */
	EXPECT_TRUE(work->hasStackWatermark);
	EXPECT_TRUE(work->stackWatermarkReachedCap);
	EXPECT_EQ(work->stackUsedPercent(), 74u);
}
