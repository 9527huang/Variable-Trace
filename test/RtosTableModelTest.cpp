#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "I18n.hpp"
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

	const RtosTaskRow* findRow(const std::vector<RtosTaskRow>& rows, const std::string& name)
	{
		for (const RtosTaskRow& row : rows)
		{
			if (row.name == name)
				return &row;
		}

		return nullptr;
	}

	std::vector<std::string> namesOf(const std::vector<RtosTaskRow>& rows)
	{
		std::vector<std::string> names;

		for (const RtosTaskRow& row : rows)
			names.push_back(row.name);

		return names;
	}

	/* The lanes of a snapshot with nothing narrowed, in the order the strip
	   draws them. The order used to be asked of the snapshot directly; it is
	   asked of the rows now, because the strip shows the tasks the table kept. */
	std::vector<uint32_t> lanesOf(const RtosSnapshot& snapshot)
	{
		return RtosTableModel::laneOrderOf(RtosTableModel::taskRows(snapshot, {}));
	}
}  // namespace

TEST(RtosTableModel, ReturnsEveryTaskWhenNothingIsAskedFor)
{
	const RtosSnapshot snapshot = makeSnapshot();

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, {});

	EXPECT_EQ(rows.size(), snapshot.tasks.size());
}

TEST(RtosTableModel, SortsByNameWhenAskedTo)
{
	const RtosSnapshot snapshot = makeSnapshot();

	/* The default query is the problem order - this is the column, asked for on
	   purpose. It used to pass by accident: every row had the same problem
	   rank and the same share, so the tie-break put them in name order anyway. */
	RtosTaskTableQuery query;
	query.problemFirst = false;
	query.sortColumn = RtosTaskColumn::Name;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"IDLE", "deep", "led", "prod", "work"}));
}

TEST(RtosTableModel, SortsByAnyColumnInEitherDirection)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery byAddress;
	byAddress.problemFirst = false;
	byAddress.sortColumn = RtosTaskColumn::Address;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byAddress)), (std::vector<std::string>{"led", "IDLE", "prod", "work", "deep"}));

	byAddress.descending = true;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byAddress)), (std::vector<std::string>{"deep", "work", "prod", "IDLE", "led"}));

	RtosTaskTableQuery byStack;
	byStack.problemFirst = false;
	byStack.sortColumn = RtosTaskColumn::StackSize;
	byStack.descending = true;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byStack)).front(), "work");
}

TEST(RtosTableModel, FiltersByState)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.states = {RtosTaskState::Blocked};

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"prod", "deep"}));
}

TEST(RtosTableModel, FiltersByPriorityFloor)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.hasPriorityFloor = true;
	query.priorityFloor = 2;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"prod", "led", "work"}));
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

	/* An empty name is missing data. Sorting by name would put it first and
	   the row that needs attention where it is least likely to be seen. This
	   is about the name order, so the problem order is off - under the problem
	   order the row goes wherever its problem puts it. */
	RtosTaskTableQuery byName;
	byName.problemFirst = false;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, byName)).back(), "");
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
	query.problemFirst = false;
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
	query.problemFirst = false;
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

TEST(RtosTableModel, TheCpuColumnKeepsRowsWithoutAShareLast)
{
	RtosSnapshot snapshot = makeSnapshot();

	snapshot.tasks[0].hasCpuShare = true;
	snapshot.tasks[0].cpuSharePercent = 4.5;
	snapshot.tasks[4].hasCpuShare = true;
	snapshot.tasks[4].cpuSharePercent = 62.5;

	RtosTaskTableQuery query;
	query.problemFirst = false;
	query.sortColumn = RtosTaskColumn::CpuLoad;
	query.descending = true;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, query);

	ASSERT_EQ(rows.size(), 5u);

	/* Busiest first, which is the question this column gets asked. */
	EXPECT_EQ(rows[0].name, "deep");
	EXPECT_DOUBLE_EQ(rows[0].cpuSharePercent, 62.5);

	EXPECT_EQ(rows[1].name, "IDLE");
	EXPECT_DOUBLE_EQ(rows[1].cpuSharePercent, 4.5);

	/* The rest have no share: the first round of a session has none for anyone.
	   They go to the end rather than to the top, where a zero would have put
	   them. */
	for (size_t index = 2; index < rows.size(); ++index)
		EXPECT_FALSE(rows[index].hasCpuShare);
}

TEST(RtosTableModel, MarksOnlyTheStacksBelowTheAlertThreshold)
{
	RtosSnapshot snapshot = makeSnapshot();

	/* A quarter of one stack free, half of another. */
	snapshot.tasks[0].hasStackWatermark = true;
	snapshot.tasks[0].stackFreeBytes = 126;
	snapshot.tasks[0].stackWatermarkStackBytes = 504;
	snapshot.tasks[2].hasStackWatermark = true;
	snapshot.tasks[2].stackFreeBytes = 252;
	snapshot.tasks[2].stackWatermarkStackBytes = 504;

	RtosTaskTableQuery query;

	/* Under the threshold, nothing is marked. */
	query.stackAlertFreePercent = 20;

	for (const RtosTaskRow& row : RtosTableModel::taskRows(snapshot, query))
		EXPECT_FALSE(row.stackIsLow) << row.name;

	/* At the threshold exactly, still nothing: the alert is for a stack below
	   it, and a threshold nobody can sit on is a threshold nobody trusts. */
	query.stackAlertFreePercent = 25;

	const std::vector<RtosTaskRow> atTheLine = RtosTableModel::taskRows(snapshot, query);

	ASSERT_NE(findRow(atTheLine, "IDLE"), nullptr);
	EXPECT_EQ(findRow(atTheLine, "IDLE")->stackFreePercent(), 25u);
	EXPECT_FALSE(findRow(atTheLine, "IDLE")->stackIsLow);

	/* One point higher, and the quarter-empty stack is marked while the
	   half-empty one is not. */
	query.stackAlertFreePercent = 26;

	const std::vector<RtosTaskRow> marked = RtosTableModel::taskRows(snapshot, query);

	ASSERT_NE(findRow(marked, "IDLE"), nullptr);
	EXPECT_TRUE(findRow(marked, "IDLE")->stackIsLow);

	ASSERT_NE(findRow(marked, "prod"), nullptr);
	EXPECT_FALSE(findRow(marked, "prod")->stackIsLow);

	/* A row with no reading is never marked, however high the threshold. Not
	   knowing is not the same as being comfortable. */
	query.stackAlertFreePercent = 99;

	const std::vector<RtosTaskRow> everything = RtosTableModel::taskRows(snapshot, query);

	ASSERT_NE(findRow(everything, "led"), nullptr);
	EXPECT_FALSE(findRow(everything, "led")->hasStackWatermark);
	EXPECT_FALSE(findRow(everything, "led")->stackIsLow);

	/* Zero is the way to turn it off. */
	query.stackAlertFreePercent = 0;

	const std::vector<RtosTaskRow> off = RtosTableModel::taskRows(snapshot, query);

	ASSERT_NE(findRow(off, "IDLE"), nullptr);
	EXPECT_FALSE(findRow(off, "IDLE")->stackIsLow);
}

TEST(RtosTableModel, TheLanesAreOrderedByBasePriorityAndNotByTheCurrentOne)
{
	RtosSnapshot snapshot;

	/* Three tasks: one at base priority 3, one at 2, one at 1 that is currently
	   raised to 3 because it holds a mutex another task wants. */
	RtosTask high = makeTask("zeta", 0x20000300, RtosTaskState::Ready, 3, 504);
	RtosTask middle = makeTask("beta", 0x20000200, RtosTaskState::Ready, 2, 504);
	RtosTask boosted = makeTask("alpha", 0x20000100, RtosTaskState::Ready, 3, 504);

	boosted.hasBasePriority = true;
	boosted.basePriority = 1;

	high.hasBasePriority = true;
	high.basePriority = 3;

	middle.hasBasePriority = true;
	middle.basePriority = 2;

	snapshot.tasks = {middle, boosted, high};

	const std::vector<uint32_t> lanes = lanesOf(snapshot);

	/* Highest base priority first. Sorted by the current priority instead, the
	   boosted task would be somewhere in the top group and would move there and
	   back every time inheritance happened - a lane that jumps while the reader
	   is looking at it. */
	ASSERT_EQ(lanes.size(), 3u);
	EXPECT_EQ(lanes[0], 0x20000300u);  /* zeta, base 3 */
	EXPECT_EQ(lanes[1], 0x20000200u);  /* beta, base 2 */
	EXPECT_EQ(lanes[2], 0x20000100u);  /* alpha, base 1 despite running at 3 */
}

TEST(RtosTableModel, ATaskWithNoPriorityGoesToTheBottomLane)
{
	RtosSnapshot snapshot;

	RtosTask known = makeTask("known", 0x20000200, RtosTaskState::Ready, 1, 504);
	known.hasBasePriority = true;
	known.basePriority = 1;

	RtosTask unread;
	unread.tcbAddress = 0x20000100;
	unread.state = RtosTaskState::Unknown;
	unread.hasPriority = false;

	snapshot.tasks = {unread, known};

	const std::vector<uint32_t> lanes = lanesOf(snapshot);

	ASSERT_EQ(lanes.size(), 2u);
	EXPECT_EQ(lanes[0], 0x20000200u);

	/* Nothing was read for it, which is not the same as the lowest priority -
	   but it is still below everything that was read. */
	EXPECT_EQ(lanes[1], 0x20000100u);
}

TEST(RtosTableModel, EveryStateHasATranslatedName)
{
	const RtosTaskState states[] = {RtosTaskState::Running,    RtosTaskState::Ready,  RtosTaskState::Blocked, RtosTaskState::Suspended,
	                                RtosTaskState::PendingReady, RtosTaskState::Deleted, RtosTaskState::Unknown};

	const I18n::Language was = I18n::currentLanguage;

	for (const RtosTaskState state : states)
	{
		const char* english = RtosTableModel::taskStateLabel(state);

		ASSERT_NE(english, nullptr);
		EXPECT_GT(std::strlen(english), 0u);

		/* The label is the same word the API reports while the interface is in
		   English, so a reader looking at both sees one spelling. */
		EXPECT_STREQ(english, toString(state));
	}

	I18n::currentLanguage = I18n::Language::Chinese;

	for (const RtosTaskState state : states)
	{
		const char* chinese = RtosTableModel::taskStateLabel(state);

		ASSERT_NE(chinese, nullptr);

		/* A state with no entry comes back as its English, which is how a
		   missing translation reaches the screen - silently. This is where it
		   stops being silent. */
		EXPECT_STRNE(chinese, toString(state)) << toString(state);
	}

	I18n::currentLanguage = was;
}

TEST(RtosTableModel, TheSummaryCountsWhatTheRoundHolds)
{
	RtosSnapshot snapshot = makeSnapshot();

	/* Under a fifth of one stack free, so one alert at a threshold of twenty. */
	snapshot.tasks[0].hasStackWatermark = true;
	snapshot.tasks[0].stackFreeBytes = 90;
	snapshot.tasks[0].stackWatermarkStackBytes = 504;

	/* And one task whose current priority is above its base one. */
	snapshot.tasks[3].rawPriority = 3;
	snapshot.tasks[3].basePriority = 2;
	snapshot.tasks[3].hasBasePriority = true;

	const RtosTableModel::Summary summary = RtosTableModel::summaryOf(snapshot, 20);

	EXPECT_EQ(summary.tasks, 5u);
	EXPECT_EQ(summary.ready, 2u);      /* IDLE, led */
	EXPECT_EQ(summary.blocked, 2u);    /* prod, deep */
	EXPECT_EQ(summary.suspended, 1u);  /* work */
	EXPECT_EQ(summary.stackAlerts, 1u);
	EXPECT_EQ(summary.inherited, 1u);

	/* Zero is the switch: no threshold, no alerts. */
	EXPECT_EQ(RtosTableModel::summaryOf(snapshot, 0).stackAlerts, 0u);
}

TEST(RtosTableModel, TheStackAlertFilterKeepsOnlyTheMarkedRows)
{
	RtosSnapshot snapshot = makeSnapshot();

	snapshot.tasks[0].hasStackWatermark = true;
	snapshot.tasks[0].stackFreeBytes = 90;
	snapshot.tasks[0].stackWatermarkStackBytes = 504;

	RtosTaskTableQuery query;
	query.stackAlertFreePercent = 20;
	query.onlyStackAlerts = true;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, query);

	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows.front().name, "IDLE");
	EXPECT_TRUE(rows.front().stackIsLow);
}

TEST(RtosTableModel, OnlyBlockedKeepsTheTasksThatAreWaiting)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::Blocked;

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"prod", "deep"}));
}

TEST(RtosTableModel, ProblemsOnlyKeepsStacksInheritanceAndWhateverTheFindingsName)
{
	RtosSnapshot snapshot = makeSnapshot();

	/* IDLE's stack is nearly full, and work has had a priority lent to it. */
	snapshot.tasks[0].hasStackWatermark = true;
	snapshot.tasks[0].stackFreeBytes = 90;
	snapshot.tasks[0].stackWatermarkStackBytes = 504;

	snapshot.tasks[3].basePriority = 1;
	snapshot.tasks[3].hasBasePriority = true;
	snapshot.tasks[3].rawPriority = 2;

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::Anomalies;

	/* The order is the problem order: the stack first, then the inheritance. */
	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"IDLE", "work"}));

	/* And a task the findings named gets in on that alone - a ready task the
	   scheduler keeps passing over carries nothing on its own row that says so,
	   which is why the addresses are handed in. */
	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query, {0x20000100u})), (std::vector<std::string>{"IDLE", "work", "led"}));
}

TEST(RtosTableModel, ProblemsOnlyDoesNotKeepEveryTaskWaitingOnAnObject)
{
	RtosSnapshot snapshot = makeSnapshot();

	/* Waiting on a queue is what most tasks do most of the time. Only a wait
	   with no timeout, a ring, or an inheritance is a problem, and none of them
	   is a field on a row - a filter that kept every blocked task would answer
	   "everything is wrong", which is an answer nobody reads twice. */
	snapshot.tasks[2].waitingOn = "queue_prod";

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::Anomalies;

	EXPECT_TRUE(RtosTableModel::taskRows(snapshot, query).empty());
}

TEST(RtosTableModel, HighShareKeepsTheRowsAtOrAboveTheFloorAndNothingUnmeasured)
{
	RtosSnapshot snapshot = makeSnapshot();

	snapshot.tasks[1].hasCpuShare = true;  /* led */
	snapshot.tasks[1].cpuSharePercent = 42.0;
	snapshot.tasks[2].hasCpuShare = true;  /* prod */
	snapshot.tasks[2].cpuSharePercent = 10.0;
	snapshot.tasks[3].hasCpuShare = true;  /* work */
	snapshot.tasks[3].cpuSharePercent = 9.9;

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::HighShare;
	query.shareFloorPercent = 10.0;

	/* At the floor counts: a reader who typed ten means ten and above. The two
	   tasks with no share at all are not small, they are unmeasured. */
	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"prod", "led"}));

	/* Zero is the switch: every task that has a share, however little. */
	query.shareFloorPercent = 0.0;

	EXPECT_EQ(RtosTableModel::taskRows(snapshot, query).size(), 3u);
}

TEST(RtosTableModel, TheModeNarrowsFirstAndTheFiltersNarrowInsideIt)
{
	RtosSnapshot snapshot = makeSnapshot();

	snapshot.tasks[1].hasCpuShare = true;  /* led, ready */
	snapshot.tasks[1].cpuSharePercent = 40.0;
	snapshot.tasks[2].hasCpuShare = true;  /* prod, blocked */
	snapshot.tasks[2].cpuSharePercent = 30.0;

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::HighShare;
	query.shareFloorPercent = 10.0;
	query.states = {RtosTaskState::Blocked};

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(snapshot, query)), (std::vector<std::string>{"prod"}));
}

TEST(RtosTableModel, TheLanesAreOnlyTheRowsTheFilterKept)
{
	const RtosSnapshot snapshot = makeSnapshot();

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::Blocked;

	const std::vector<uint32_t> lanes = RtosTableModel::laneOrderOf(RtosTableModel::taskRows(snapshot, query));

	/* prod at 3 and deep at 1, so prod keeps the top lane; the three tasks the
	   filter dropped have no lane at all - a strip of fourteen lanes over a
	   three row table would leave the reader counting. */
	ASSERT_EQ(lanes.size(), 2u);
	EXPECT_EQ(lanes[0], 0x20000300u);
	EXPECT_EQ(lanes[1], 0x20000500u);
}

namespace
{
	/* A roster built the way the model builds one: every round observed, in
	   order, never removing. */
	RtosTaskRoster rosterOf(std::initializer_list<RtosSnapshot> rounds)
	{
		RtosTaskRoster roster;

		for (const RtosSnapshot& round : rounds)
			roster.observe(round);

		return roster;
	}

	/* The second round of the fixture, with `prod` missing: a walk of the delay
	   list that came back short. */
	RtosSnapshot makeSnapshotMissing(const std::string& name)
	{
		RtosSnapshot snapshot = makeSnapshot();

		snapshot.tasks.erase(std::remove_if(snapshot.tasks.begin(), snapshot.tasks.end(), [&](const RtosTask& task)
		{
			return task.name == name;
		}), snapshot.tasks.end());

		snapshot.captureStartedAt = 3.0;

		return snapshot;
	}
}  // namespace

TEST(RtosTableModel, ATaskTheRoundDidNotReadKeepsItsRowAndCarriesSlashes)
{
	const RtosSnapshot first = makeSnapshot();
	const RtosSnapshot second = makeSnapshotMissing("prod");

	const RtosTaskRoster roster = rosterOf({first, second});

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(roster.entries(), second, {});

	/* Five tasks were seen and five rows are there, although this round holds
	   four: the row set is the session's, not the round's. */
	ASSERT_EQ(rows.size(), 5u);

	const RtosTaskRow* prod = findRow(rows, "prod");

	ASSERT_NE(prod, nullptr);
	EXPECT_FALSE(prod->hasReading);

	/* Nothing about it was read this round, so nothing in it claims to be. */
	EXPECT_FALSE(prod->hasPriority);
	EXPECT_FALSE(prod->hasStack);
	EXPECT_FALSE(prod->hasCpuShare);
	EXPECT_TRUE(prod->waitingOn.empty());

	/* The identity it is known by is still there. */
	EXPECT_EQ(prod->tcbAddress, 0x20000300u);
	EXPECT_DOUBLE_EQ(prod->sinceLastReadSeconds, 3.0);

	/* And the four the round did read are unchanged. */
	EXPECT_TRUE(findRow(rows, "led")->hasReading);
	EXPECT_EQ(findRow(rows, "led")->priority, 2u);
}

TEST(RtosTableModel, AnUnreadRowAnswersNoneOfTheNarrowQuestions)
{
	const RtosSnapshot first = makeSnapshot();
	const RtosSnapshot second = makeSnapshotMissing("prod");

	const RtosTaskRoster roster = rosterOf({first, second});

	RtosTaskTableQuery query;
	query.mode = RtosTaskFilterMode::Blocked;

	/* `deep` is blocked in this round; `prod` was blocked in a round that is
	   gone. A question about what is blocked now has one answer. */
	EXPECT_EQ(namesOf(RtosTableModel::taskRows(roster.entries(), second, query)), (std::vector<std::string>{"deep"}));

	query.mode = RtosTaskFilterMode::Anomalies;

	/* Nor is it an anomaly: this round cannot say either way. */
	EXPECT_TRUE(RtosTableModel::taskRows(roster.entries(), second, query).empty());

	/* It can still be found by name, which is the one thing identity answers. */
	query.mode = RtosTaskFilterMode::All;
	query.nameSearch = "pro";

	EXPECT_EQ(namesOf(RtosTableModel::taskRows(roster.entries(), second, query)), (std::vector<std::string>{"prod"}));
}

TEST(RtosTableModel, AnUnreadRowIsPutLastInTheProblemOrder)
{
	const RtosSnapshot first = makeSnapshot();
	const RtosSnapshot second = makeSnapshotMissing("prod");

	const RtosTaskRoster roster = rosterOf({first, second});

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(roster.entries(), second, {});

	/* `prod` is blocked with a waiting object in the first round, which would
	   put it second - but this round has nothing on it, and a row nobody can
	   judge belongs at the bottom rather than in the middle. */
	ASSERT_EQ(rows.size(), 5u);
	EXPECT_EQ(rows.back().name, "prod");
}

TEST(RtosTableModel, AnUnreadRowDoesNotTakePartInColumnSorting)
{
	const RtosSnapshot first = makeSnapshot();
	const RtosSnapshot second = makeSnapshotMissing("prod");

	const RtosTaskRoster roster = rosterOf({first, second});

	RtosTaskTableQuery query;
	query.problemFirst = false;
	query.sortColumn = RtosTaskColumn::Priority;
	query.descending = true;

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(roster.entries(), second, query);

	/* Whichever way the column runs, the row with no value for it is at the end
	   - a gap is not a small number. */
	ASSERT_EQ(rows.size(), 5u);
	EXPECT_FALSE(rows.back().hasReading);
	EXPECT_EQ(rows.back().name, "prod");
}

TEST(RtosTableModel, OneRoundOnItsOwnStillFillsOneRowPerTask)
{
	/* The single-round overload is what an API client gets: it never marks a
	   row, because it never looks at a roster wider than the round. */
	const RtosSnapshot snapshot = makeSnapshot();

	const std::vector<RtosTaskRow> rows = RtosTableModel::taskRows(snapshot, {});

	ASSERT_EQ(rows.size(), 5u);

	for (const RtosTaskRow& row : rows)
		EXPECT_TRUE(row.hasReading);
}
