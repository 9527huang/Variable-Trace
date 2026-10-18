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

	/* The findings of one round, with no history behind it: the three rules that
	   only need the round itself. */
	std::vector<RtosDiagnostics::Finding> diagnoseOnly(const RtosSnapshot& snapshot)
	{
		RtosTimeline empty(4);

		return RtosDiagnostics::diagnose(snapshot, empty);
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

	const std::vector<RtosDiagnostics::Finding> findings = diagnoseOnly(snapshot);

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

	const std::vector<RtosDiagnostics::Finding> findings = diagnoseOnly(snapshot);

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

	EXPECT_TRUE(diagnoseOnly(snapshot).empty());
}

TEST(RtosDiagnostics, ACleanRoundHasNoFindings)
{
	RtosSnapshot snapshot;

	snapshot.tasks.push_back(makeTask("idle", 0x20000100, 0, 0, ""));

	EXPECT_TRUE(diagnoseOnly(snapshot).empty());
}

TEST(RtosDiagnostics, AWaitOnSomethingTheRoundDidNotSeeEndsTheWalk)
{
	RtosSnapshot snapshot;

	/* The task waits for an object whose holder was not read. Nothing is
	   claimed about it: a guess here would be a deadlock that is not there. */
	snapshot.tasks.push_back(makeTask("waiter", 0x20000100, 2, 2, "missing"));

	EXPECT_TRUE(diagnoseOnly(snapshot).empty());
}

namespace
{
	/* One round: a waiter that is ready (or not), and a runner at a priority. */
	RtosSnapshot starvationRound(bool withWaiter, RtosTaskState waiterState, uint32_t runnerRaw, uint32_t runnerBase)
	{
		RtosSnapshot snapshot;

		if (withWaiter)
		{
			RtosTask waiter = makeTask("waiter", 0x20000100, 2, 2, "");

			waiter.state = waiterState;
			snapshot.tasks.push_back(waiter);
		}

		RtosTask runner = makeTask("runner", 0x20000200, runnerRaw, runnerBase, "");

		runner.state = RtosTaskState::Running;
		snapshot.tasks.push_back(runner);

		return snapshot;
	}

	void fillRounds(RtosTimeline& timeline, int rounds, bool withWaiter, RtosTaskState waiterState, uint32_t runnerRaw, uint32_t runnerBase)
	{
		for (int round = 0; round < rounds; round++)
			timeline.add(starvationRound(withWaiter, waiterState, runnerRaw, runnerBase));
	}
}  // namespace

TEST(RtosDiagnostics, AReadyTaskAboveTheRunnerIsStarving)
{
	RtosTimeline timeline(64);
	fillRounds(timeline, 5, true, RtosTaskState::Ready, 1, 1);

	const std::vector<RtosDiagnostics::Finding> findings =
	    RtosDiagnostics::diagnose(starvationRound(true, RtosTaskState::Ready, 1, 1), timeline);

	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].kind, RtosDiagnostics::Kind::ReadyStarvation);
	ASSERT_EQ(findings[0].tasks.size(), 1u);
	EXPECT_EQ(findings[0].tasks[0], 0x20000100u);
	EXPECT_EQ(findings[0].heldRounds, 4u);
}

TEST(RtosDiagnostics, AHigherPriorityRunnerIsNotStarvation)
{
	RtosTimeline timeline(64);
	fillRounds(timeline, 5, true, RtosTaskState::Ready, 3, 3);

	/* The scheduler is running something above the waiter, which is what it is
	   supposed to do. */
	EXPECT_TRUE(RtosDiagnostics::diagnose(starvationRound(true, RtosTaskState::Ready, 3, 3), timeline).empty());
}

TEST(RtosDiagnostics, ARunnerHoldingALockIsNotStarvation)
{
	RtosTimeline timeline(64);

	/* Boosted: it was created at 1 and is running at 3 because someone above it
	   is waiting for a lock it holds. That is the kernel working. */
	fillRounds(timeline, 5, true, RtosTaskState::Ready, 3, 1);

	EXPECT_TRUE(RtosDiagnostics::diagnose(starvationRound(true, RtosTaskState::Ready, 3, 1), timeline).empty());
}

TEST(RtosDiagnostics, ThreeRoundsSayNothing)
{
	RtosTimeline timeline(64);
	fillRounds(timeline, 3, true, RtosTaskState::Ready, 1, 1);

	/* A moment is not a fault: the switch may be about to happen. */
	EXPECT_TRUE(RtosDiagnostics::diagnose(starvationRound(true, RtosTaskState::Ready, 1, 1), timeline).empty());
}

TEST(RtosDiagnostics, ATaskThatRanInTheMiddleIsNotStarving)
{
	RtosTimeline timeline(64);

	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Running, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));

	EXPECT_TRUE(RtosDiagnostics::diagnose(starvationRound(true, RtosTaskState::Ready, 1, 1), timeline).empty());
}

TEST(RtosDiagnostics, ARoundWithoutTheTaskEndsTheWindow)
{
	RtosTimeline timeline(64);

	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(false, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));
	timeline.add(starvationRound(true, RtosTaskState::Ready, 1, 1));

	/* A round that did not carry the task says nothing either way. */
	EXPECT_TRUE(RtosDiagnostics::diagnose(starvationRound(true, RtosTaskState::Ready, 1, 1), timeline).empty());
}

namespace
{
	/* A task blocked on an object, with the list its state item sits on. */
	RtosSnapshot blockedRound(RtosTaskState state, const std::string& sourceList)
	{
		RtosSnapshot snapshot;

		RtosTask task = makeTask("stuck", 0x20000100, 2, 2, "sem");

		task.state = state;
		task.sourceList = sourceList;

		snapshot.tasks.push_back(task);

		return snapshot;
	}
}  // namespace

TEST(RtosDiagnostics, ABlockedTaskWithNoTimeoutIsReported)
{
	RtosTimeline timeline(16);
	timeline.add(blockedRound(RtosTaskState::Blocked, "suspended"));
	timeline.add(blockedRound(RtosTaskState::Blocked, "suspended"));

	/* Blocked, with its state on the suspended list: there is no time to wake it
	   at, so it runs again only when the object is given. */
	const std::vector<RtosDiagnostics::Finding> findings =
	    RtosDiagnostics::diagnose(blockedRound(RtosTaskState::Blocked, "suspended"), timeline);

	ASSERT_EQ(findings.size(), 1u);
	EXPECT_EQ(findings[0].kind, RtosDiagnostics::Kind::UnboundedWait);

	ASSERT_EQ(findings[0].chain.size(), 2u);
	EXPECT_EQ(findings[0].chain[0], "stuck");
	EXPECT_EQ(findings[0].chain[1], "sem");

	/* Two rounds of history, both carried the task blocked. */
	EXPECT_EQ(findings[0].heldRounds, 2u);
}

TEST(RtosDiagnostics, ATaskSuspendedOnPurposeIsNotWaitingForAnything)
{
	RtosTimeline timeline(16);

	/* Same list, different state: nobody is waiting for anything. */
	EXPECT_TRUE(RtosDiagnostics::diagnose(blockedRound(RtosTaskState::Suspended, "suspended"), timeline).empty());
}

TEST(RtosDiagnostics, ATimedBlockIsNotReported)
{
	RtosTimeline timeline(16);

	/* On the delayed list: the kernel knows when to wake it. */
	EXPECT_TRUE(RtosDiagnostics::diagnose(blockedRound(RtosTaskState::Blocked, "delayed"), timeline).empty());
}
