#include <gtest/gtest.h>

#include <string>

#include "RtosTaskRoster.hpp"

namespace
{
	RtosTask makeTask(const std::string& name, uint32_t address, uint32_t basePriority, uint32_t stackBytes)
	{
		RtosTask task;

		task.name = name;
		task.tcbAddress = address;
		task.state = RtosTaskState::Ready;
		task.hasPriority = true;
		task.rawPriority = basePriority;
		task.priorityRank = basePriority;
		task.basePriority = basePriority;
		task.hasBasePriority = true;
		task.hasStack = true;
		task.stackBase = 0x20001000;
		task.stackTop = task.stackBase + stackBytes;
		task.kernelTaskNumber = address & 0xFF;
		task.hasKernelTaskNumber = true;

		return task;
	}

	RtosSnapshot round(double atSeconds, const std::vector<RtosTask>& tasks)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = atSeconds;
		snapshot.tasks = tasks;

		return snapshot;
	}
}  // namespace

TEST(RtosTaskRoster, KeepsEveryTaskItHasSeen)
{
	RtosTaskRoster roster;

	EXPECT_EQ(roster.size(), 0u);

	roster.observe(round(1.0, {makeTask("a", 0x20000100, 3, 512), makeTask("b", 0x20000200, 2, 512)}));

	ASSERT_EQ(roster.size(), 2u);
	EXPECT_EQ(roster.entries()[0].name, "a");
	EXPECT_EQ(roster.entries()[1].name, "b");

	/* The second round lost `b`: a short walk of a list, not a task that
	   ended. The roster keeps it. */
	roster.observe(round(1.5, {makeTask("a", 0x20000100, 3, 512)}));

	ASSERT_EQ(roster.size(), 2u);

	const RtosTaskRoster::Entry* b = roster.find(0x20000200);

	ASSERT_NE(b, nullptr);
	EXPECT_EQ(b->name, "b");
	EXPECT_DOUBLE_EQ(b->lastSeenAt, 1.0);
	EXPECT_EQ(b->roundsSeen, 1u);

	EXPECT_DOUBLE_EQ(roster.find(0x20000100)->lastSeenAt, 1.5);
	EXPECT_EQ(roster.find(0x20000100)->roundsSeen, 2u);
}

TEST(RtosTaskRoster, TakesWhatTheRoundSaysAndKeepsWhatItDoesNot)
{
	RtosTaskRoster roster;

	RtosTask renamed = makeTask("old", 0x20000100, 1, 512);

	roster.observe(round(1.0, {renamed}));

	/* A later round renames it and cannot read its control block: the name is
	   taken again, the base priority that was already read is not wiped. */
	RtosTask nameless;
	nameless.tcbAddress = 0x20000100;
	nameless.name = "new";
	nameless.hasPriority = false;
	nameless.hasBasePriority = false;

	roster.observe(round(2.0, {nameless}));

	const RtosTaskRoster::Entry* entry = roster.find(0x20000100);

	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->name, "new");
	EXPECT_TRUE(entry->hasBasePriority);
	EXPECT_EQ(entry->basePriority, 1u);
	EXPECT_TRUE(entry->hasStack);
	EXPECT_EQ(entry->stackTop - entry->stackBase, 512u);
}

TEST(RtosTaskRoster, AnAddressOfZeroIsNotATask)
{
	RtosTaskRoster roster;

	RtosTask unplaced = makeTask("", 0, 1, 512);

	roster.observe(round(1.0, {unplaced}));

	EXPECT_EQ(roster.size(), 0u);
}

TEST(RtosTaskRoster, ClearingStartsAgain)
{
	RtosTaskRoster roster;

	roster.observe(round(1.0, {makeTask("a", 0x20000100, 1, 512)}));
	ASSERT_EQ(roster.size(), 1u);

	roster.clear();

	EXPECT_EQ(roster.size(), 0u);
	EXPECT_EQ(roster.find(0x20000100), nullptr);

	/* And it takes the address again rather than remembering the old entry. */
	roster.observe(round(5.0, {makeTask("a", 0x20000100, 1, 512)}));

	ASSERT_EQ(roster.size(), 1u);
	EXPECT_DOUBLE_EQ(roster.entries().front().lastSeenAt, 5.0);
}
