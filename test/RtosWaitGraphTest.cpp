#include "RtosWaitGraph.hpp"

#include <gtest/gtest.h>

namespace
{
	RtosTask makeTask(uint32_t address, const std::string& name, const std::string& waitingOn)
	{
		RtosTask task;

		task.tcbAddress = address;
		task.name = name;
		task.state = waitingOn.empty() ? RtosTaskState::Ready : RtosTaskState::Blocked;
		task.waitingOn = waitingOn;
		task.hasPriority = true;
		task.rawPriority = 3;

		return task;
	}

	RtosSyncObject makeObject(uint32_t address, const std::string& name, uint32_t holder, uint32_t waiting)
	{
		RtosSyncObject object;

		object.address = address;
		object.name = name;
		object.kind = RtosSyncKind::Mutex;
		object.capacity = 1;
		object.items = holder == 0 ? 1 : 0;
		object.hasCounts = true;
		object.holder = holder;
		object.hasHolder = holder != 0;
		object.waitingToReceive = waiting;
		object.hasWaiters = true;

		return object;
	}

	RtosSnapshot makeSnapshot()
	{
		RtosSnapshot snapshot;

		snapshot.tasks.push_back(makeTask(0x1000, "worker", "mtx_a"));
		snapshot.tasks.push_back(makeTask(0x1040, "holder", ""));
		snapshot.syncObjects.push_back(makeObject(0x2000, "mtx_a", 0x1040, 1));

		return snapshot;
	}
}

TEST(RtosWaitGraphTest, StartsAtTheTaskEvenWhenItIsNotWaiting)
{
	const RtosSnapshot snapshot = makeSnapshot();
	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0x1040);

	ASSERT_EQ(1u, chain.nodes.size());
	EXPECT_EQ(RtosWaitGraph::NodeKind::Task, chain.nodes[0].kind);
	EXPECT_EQ("holder", chain.nodes[0].name);
	EXPECT_TRUE(chain.steps.empty());
	EXPECT_FALSE(chain.cycle);
}

TEST(RtosWaitGraphTest, FollowsTheWaitToTheObjectAndTheObjectToItsHolder)
{
	const RtosSnapshot snapshot = makeSnapshot();
	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0x1000);

	ASSERT_EQ(3u, chain.nodes.size());

	EXPECT_EQ(RtosWaitGraph::NodeKind::Task, chain.nodes[0].kind);
	EXPECT_EQ("worker", chain.nodes[0].name);
	EXPECT_EQ(RtosWaitGraph::NodeKind::Object, chain.nodes[1].kind);
	EXPECT_EQ("mtx_a", chain.nodes[1].name);
	EXPECT_EQ(1u, chain.nodes[1].waitingOnThis);
	EXPECT_EQ(RtosWaitGraph::NodeKind::Task, chain.nodes[2].kind);
	EXPECT_EQ("holder", chain.nodes[2].name);

	ASSERT_EQ(2u, chain.steps.size());
	EXPECT_FALSE(chain.steps[0].heldBy);
	EXPECT_EQ(1u, chain.steps[0].to);
	EXPECT_TRUE(chain.steps[1].heldBy);
	EXPECT_EQ(2u, chain.steps[1].to);
	EXPECT_FALSE(chain.cycle);
}

TEST(RtosWaitGraphTest, MarksTheRingInsteadOfDrawingASecondCopyOfTheFirstBox)
{
	/* worker waits for mtx_a, held by holder; holder waits for mtx_b, held by
	   worker. The walk has to come back to the first task and stop. */
	RtosSnapshot snapshot;

	snapshot.tasks.push_back(makeTask(0x1000, "worker", "mtx_a"));
	snapshot.tasks.push_back(makeTask(0x1040, "holder", "mtx_b"));
	snapshot.syncObjects.push_back(makeObject(0x2000, "mtx_a", 0x1040, 1));
	snapshot.syncObjects.push_back(makeObject(0x2040, "mtx_b", 0x1000, 1));

	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0x1000);

	ASSERT_EQ(4u, chain.nodes.size());
	EXPECT_TRUE(chain.cycle);

	ASSERT_EQ(4u, chain.steps.size());

	const RtosWaitGraph::Step& closing = chain.steps.back();
	EXPECT_TRUE(closing.closesCycle);
	EXPECT_EQ(0u, closing.to);
	EXPECT_EQ(3u, closing.from);
}

TEST(RtosWaitGraphTest, StopsAtAWaitTheRegistryCannotName)
{
	RtosSnapshot snapshot;
	snapshot.tasks.push_back(makeTask(0x1000, "worker", "mtx_missing"));

	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0x1000);

	ASSERT_EQ(1u, chain.nodes.size());
	EXPECT_TRUE(chain.steps.empty());
}

TEST(RtosWaitGraphTest, StopsAtAnObjectThatNobodyHolds)
{
	RtosSnapshot snapshot;
	snapshot.tasks.push_back(makeTask(0x1000, "worker", "sem_a"));
	snapshot.syncObjects.push_back(makeObject(0x2000, "sem_a", 0, 1));

	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0x1000);

	ASSERT_EQ(2u, chain.nodes.size());
	ASSERT_EQ(1u, chain.steps.size());
	EXPECT_FALSE(chain.steps[0].heldBy);
	EXPECT_FALSE(chain.cycle);
}

TEST(RtosWaitGraphTest, CutsTheChainAtTheLimit)
{
	const RtosSnapshot snapshot = makeSnapshot();
	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0x1000, 2);

	EXPECT_LE(chain.nodes.size(), 2u);
}

TEST(RtosWaitGraphTest, AnswersWithNothingForATaskThatIsNotInTheRound)
{
	const RtosSnapshot snapshot = makeSnapshot();
	const RtosWaitGraph::Chain chain = RtosWaitGraph::from(snapshot, 0xDEAD);

	EXPECT_TRUE(chain.nodes.empty());
	EXPECT_TRUE(chain.steps.empty());
}

TEST(RtosWaitGraphTest, TwoWalksOfTheSameTargetGiveTheSameShape)
{
	const RtosSnapshot snapshot = makeSnapshot();

	const RtosWaitGraph::Chain first = RtosWaitGraph::from(snapshot, 0x1000);
	RtosSnapshot later = makeSnapshot();

	/* The numbers inside the boxes move between rounds; the picture does not. */
	later.tasks[0].state = RtosTaskState::Ready;
	later.syncObjects[0].items = 1;
	later.syncObjects[0].waitingToReceive = 3;

	const RtosWaitGraph::Chain second = RtosWaitGraph::from(later, 0x1000);

	EXPECT_TRUE(RtosWaitGraph::sameShape(first, second));
}

TEST(RtosWaitGraphTest, ADifferentHolderIsADifferentShape)
{
	RtosSnapshot snapshot = makeSnapshot();
	const RtosWaitGraph::Chain first = RtosWaitGraph::from(snapshot, 0x1000);

	/* The lock moved to a third task: one box is replaced by another. */
	snapshot.tasks.push_back(makeTask(0x1080, "other", ""));
	snapshot.syncObjects[0].holder = 0x1080;

	const RtosWaitGraph::Chain second = RtosWaitGraph::from(snapshot, 0x1000);

	EXPECT_FALSE(RtosWaitGraph::sameShape(first, second));
}

TEST(RtosWaitGraphTest, AChainThatStopsEarlierIsADifferentShape)
{
	RtosSnapshot snapshot = makeSnapshot();
	const RtosWaitGraph::Chain first = RtosWaitGraph::from(snapshot, 0x1000);

	/* Same task, same object, but nobody holds it any more. */
	snapshot.syncObjects[0].holder = 0;
	snapshot.syncObjects[0].hasHolder = false;

	const RtosWaitGraph::Chain second = RtosWaitGraph::from(snapshot, 0x1000);

	EXPECT_FALSE(RtosWaitGraph::sameShape(first, second));
}

TEST(RtosWaitGraphTest, ARingAndAnOpenChainAreNotTheSameShape)
{
	RtosSnapshot ring;

	ring.tasks.push_back(makeTask(0x1000, "worker", "mtx_a"));
	ring.tasks.push_back(makeTask(0x1040, "holder", "mtx_b"));
	ring.syncObjects.push_back(makeObject(0x2000, "mtx_a", 0x1040, 1));
	ring.syncObjects.push_back(makeObject(0x2040, "mtx_b", 0x1000, 1));

	const RtosWaitGraph::Chain closed = RtosWaitGraph::from(ring, 0x1000);

	RtosSnapshot open = ring;
	open.tasks[1].waitingOn.clear();

	const RtosWaitGraph::Chain unbounded = RtosWaitGraph::from(open, 0x1000);

	EXPECT_TRUE(closed.cycle);
	EXPECT_FALSE(unbounded.cycle);
	EXPECT_FALSE(RtosWaitGraph::sameShape(closed, unbounded));
}
