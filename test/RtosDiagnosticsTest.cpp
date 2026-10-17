#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "RtosDiagnostics.hpp"

namespace
{
	RtosTask makeTask(const std::string& name, uint32_t address, uint32_t rawPriority, uint32_t basePriority, const std::string& waitingOn)
	{
		RtosTask task;

		task.name = name;
		task.tcbAddress = address;
		task.state = waitingOn.empty() ? RtosTaskState::Ready : RtosTaskState::Blocked;
		task.hasPriority = true;
		task.rawPriority = rawPriority;
		task.hasBasePriority = true;
		task.basePriority = basePriority;
		task.waitingOn = waitingOn;

		return task;
	}

	RtosSyncObject makeMutex(const std::string& name, uint32_t address, uint32_t holder)
	{
		RtosSyncObject object;

		object.kind = RtosSyncKind::Mutex;
		object.name = name;
		object.address = address;
		object.capacity = 1;
		object.items = holder == 0 ? 1 : 0;
		object.hasCounts = true;
		object.holder = holder;
		object.hasHolder = true;

		return object;
	}

	std::string join(const std::vector<std::string>& parts)
	{
		std::string text;

		for (const std::string& part : parts)
			text += (text.empty() ? "" : " -> ") + part;

		return text;
	}
}  // namespace

TEST(RtosDiagnostics, FindsTheLockHeldWhileSomebodyWaits)
{
	RtosSnapshot snapshot;

	/* The high priority task is waiting for the mutex the low one holds, so the
	   kernel has lent the low one its priority: that is what the raised current
	   priority says. */
	snapshot.tasks.push_back(makeTask("high", 0x20000100, 3, 3, "mtx"));
	snapshot.tasks.push_back(makeTask("low", 0x20000200, 3, 1, ""));
	snapshot.syncObjects.push_back(makeMutex("mtx", 0x20001000, 0x20000200));

	const std::vector<RtosDiagnostics::Finding> findings = RtosDiagnostics::diagnose(snapshot);

	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].kind, RtosDiagnostics::Kind::PriorityInheritance);
	EXPECT_EQ(join(findings[0].chain), "high -> mtx -> low");

	/* The tasks are named for selecting them in the table, with a hole where an
	   object sits in the chain. */
	ASSERT_EQ(findings[0].tasks.size(), 3u);
	EXPECT_EQ(findings[0].tasks[0], 0x20000100u);
	EXPECT_EQ(findings[0].tasks[1], 0u);
	EXPECT_EQ(findings[0].tasks[2], 0x20000200u);
}

TEST(RtosDiagnostics, FindsARingOfTwoTasks)
{
	RtosSnapshot snapshot;

	/* Each holds what the other is waiting for. */
	snapshot.tasks.push_back(makeTask("a", 0x20000100, 2, 2, "m2"));
	snapshot.tasks.push_back(makeTask("b", 0x20000200, 2, 2, "m1"));

	snapshot.syncObjects.push_back(makeMutex("m1", 0x20001000, 0x20000100));
	snapshot.syncObjects.push_back(makeMutex("m2", 0x20002000, 0x20000200));

	const std::vector<RtosDiagnostics::Finding> findings = RtosDiagnostics::diagnose(snapshot);

	/* One ring, reported once however many tasks are on it. */
	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].kind, RtosDiagnostics::Kind::SuspectedDeadlock);
	EXPECT_NE(join(findings[0].chain).find("a"), std::string::npos);
	EXPECT_NE(join(findings[0].chain).find("b"), std::string::npos);
}

TEST(RtosDiagnostics, AOneWayWaitIsNotADeadlock)
{
	RtosSnapshot snapshot;

	snapshot.tasks.push_back(makeTask("waiter", 0x20000100, 2, 2, "m1"));
	snapshot.tasks.push_back(makeTask("holder", 0x20000200, 2, 2, ""));

	snapshot.syncObjects.push_back(makeMutex("m1", 0x20001000, 0x20000200));

	EXPECT_TRUE(RtosDiagnostics::diagnose(snapshot).empty());
}

TEST(RtosDiagnostics, ACleanRoundHasNoFindings)
{
	RtosSnapshot snapshot;

	snapshot.tasks.push_back(makeTask("idle", 0x20000100, 0, 0, ""));

	EXPECT_TRUE(RtosDiagnostics::diagnose(snapshot).empty());
}

TEST(RtosDiagnostics, AWaitOnSomethingTheRoundDidNotSeeEndsTheWalk)
{
	RtosSnapshot snapshot;

	/* The task waits for an object whose holder was not read. Nothing is
	   claimed about it: a guess here would be a deadlock that is not there. */
	snapshot.tasks.push_back(makeTask("waiter", 0x20000100, 2, 2, "missing"));

	EXPECT_TRUE(RtosDiagnostics::diagnose(snapshot).empty());
}
