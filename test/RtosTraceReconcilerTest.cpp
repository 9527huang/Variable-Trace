#include "RtosTraceReconciler.hpp"

#include <gtest/gtest.h>

namespace
{
	RtosTask task(uint32_t address, const std::string& name)
	{
		RtosTask made;

		made.tcbAddress = address;
		made.name = name;

		return made;
	}

	RtosSnapshot snapshotWith(std::vector<RtosTask> tasks)
	{
		RtosSnapshot snapshot;
		snapshot.tasks = std::move(tasks);

		return snapshot;
	}

	RtosEntityRef refFor(uint32_t address)
	{
		RtosEntityRef ref;
		ref.kind = RtosEntityKind::Task;
		ref.rawHandle = address;
		ref.quality = RtosIdentityQuality::Exact;

		return ref;
	}
}  // namespace

TEST(RtosTraceReconcilerTest, ATaskOnlyTheSnapshotSeesIsReported)
{
	const RtosSnapshot snapshot = snapshotWith({task(0x1000, "led")});
	const RtosIdentityStore store;

	const auto findings = RtosTraceReconciler::compare(snapshot, store, 1);

	ASSERT_EQ(1u, findings.size());
	EXPECT_EQ(RtosReconcileFinding::Kind::SnapshotTaskNotInEvents, findings[0].kind);
	EXPECT_EQ(0x1000u, findings[0].tcbAddress);
	EXPECT_EQ(std::string("led"), findings[0].snapshotName);
}

TEST(RtosTraceReconcilerTest, ATaskBothKnowsWithTheSameNameIsNotAFinding)
{
	RtosIdentityStore store;
	const RtosIdentityStore::Outcome created = store.observe(refFor(0x1000), RtosTraceEventType::TaskCreate, 1);
	store.nameFromDictionary(created.key, "led");

	const RtosSnapshot snapshot = snapshotWith({task(0x1000, "led")});
	const auto findings = RtosTraceReconciler::compare(snapshot, store, 1);

	EXPECT_TRUE(findings.empty());
}

TEST(RtosTraceReconcilerTest, AnOpenLifetimeTheRoundDidNotShowIsNotADeletion)
{
	RtosIdentityStore store;
	const RtosIdentityStore::Outcome created = store.observe(refFor(0x1000), RtosTraceEventType::TaskCreate, 1);
	store.nameFromDictionary(created.key, "led");

	const RtosSnapshot snapshot = snapshotWith({});

	const auto findings = RtosTraceReconciler::compare(snapshot, store, 1);

	ASSERT_EQ(1u, findings.size());
	EXPECT_EQ(RtosReconcileFinding::Kind::EventEntityNotInSnapshot, findings[0].kind);
	EXPECT_EQ(std::string("led"), findings[0].eventName);
}

TEST(RtosTraceReconcilerTest, AClosedLifetimeTheRoundDidNotShowIsSimplyGone)
{
	RtosIdentityStore store;
	store.observe(refFor(0x1000), RtosTraceEventType::TaskCreate, 1);
	store.observe(refFor(0x1000), RtosTraceEventType::TaskDelete, 1);

	const RtosSnapshot snapshot = snapshotWith({});
	const auto findings = RtosTraceReconciler::compare(snapshot, store, 1);

	EXPECT_TRUE(findings.empty());
}

TEST(RtosTraceReconcilerTest, TwoNamesForOneTaskAreSaidAndNotFixed)
{
	RtosIdentityStore store;
	const RtosIdentityStore::Outcome created = store.observe(refFor(0x1000), RtosTraceEventType::TaskCreate, 1);
	store.nameFromDictionary(created.key, "worker-old");

	const RtosSnapshot snapshot = snapshotWith({task(0x1000, "worker-new")});
	const auto findings = RtosTraceReconciler::compare(snapshot, store, 1);

	ASSERT_EQ(1u, findings.size());
	EXPECT_EQ(RtosReconcileFinding::Kind::NameDiffers, findings[0].kind);
	EXPECT_EQ(std::string("worker-new"), findings[0].snapshotName);
	EXPECT_EQ(std::string("worker-old"), findings[0].eventName);

	/* The store is untouched: reconciling by editing history is how a debugger
	   ends up showing a target that never existed. */
	const RtosIdentityStore::Entity* entity = store.find(created.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_EQ(std::string("worker-old"), entity->name);
}

TEST(RtosTraceReconcilerTest, TheListIsOrderedByAddressSoItDoesNotReshuffle)
{
	RtosIdentityStore store;

	const RtosSnapshot snapshot = snapshotWith({task(0x3000, "c"), task(0x1000, "a"), task(0x2000, "b")});
	const auto findings = RtosTraceReconciler::compare(snapshot, store, 1);

	ASSERT_EQ(3u, findings.size());
	EXPECT_EQ(0x1000u, findings[0].tcbAddress);
	EXPECT_EQ(0x2000u, findings[1].tcbAddress);
	EXPECT_EQ(0x3000u, findings[2].tcbAddress);
}
