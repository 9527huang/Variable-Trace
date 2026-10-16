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
