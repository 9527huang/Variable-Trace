#include "RtosIdentityStore.hpp"

#include <gtest/gtest.h>

namespace
{
	RtosEntityRef task(uint32_t handle, uint32_t generation = 0)
	{
		RtosEntityRef ref;
		ref.kind = RtosEntityKind::Task;
		ref.rawHandle = handle;
		ref.generation = generation;
		ref.quality = RtosIdentityQuality::Exact;

		return ref;
	}

	RtosEntityRef sync(uint32_t handle)
	{
		RtosEntityRef ref = task(handle);
		ref.kind = RtosEntityKind::Sync;

		return ref;
	}
}

TEST(RtosIdentityStoreTest, CreateDeleteCreateGivesTwoGenerations)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome first = store.observe(task(0x1000), RtosTraceEventType::TaskCreate, 1);
	EXPECT_EQ(1u, first.key.generation);
	EXPECT_FALSE(first.implicitClose);

	store.observe(task(0x1000), RtosTraceEventType::TaskDelete, 1);

	const RtosIdentityStore::Outcome second = store.observe(task(0x1000), RtosTraceEventType::TaskCreate, 1);
	EXPECT_EQ(2u, second.key.generation);

	/* The whole point: one address, two lifetimes, two keys. */
	EXPECT_NE(first.key, second.key);

	const RtosIdentityStore::Entity* oldEntity = store.find(first.key);
	ASSERT_NE(nullptr, oldEntity);
	EXPECT_TRUE(oldEntity->closed);
}

TEST(RtosIdentityStoreTest, AnOrdinaryEventWithoutCreationIsMarkedAsAGuess)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome outcome = store.observe(task(0x2000), RtosTraceEventType::TaskSwitchIn, 3);

	EXPECT_TRUE(outcome.openedWithoutCreation);
	EXPECT_EQ(1u, outcome.key.generation);

	const RtosIdentityStore::Entity* entity = store.find(outcome.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_TRUE(entity->generationUnknown);
	EXPECT_FALSE(entity->closed);
}

TEST(RtosIdentityStoreTest, AfterADeletionAnOrdinaryEventIsANewLifetime)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome created = store.observe(task(0x3000), RtosTraceEventType::TaskCreate, 1);
	store.observe(task(0x3000), RtosTraceEventType::TaskDelete, 1);

	const RtosIdentityStore::Outcome later = store.observe(task(0x3000), RtosTraceEventType::TaskSwitchIn, 1);

	/* The old lifetime is closed, so this cannot be it: a new generation, and
	   the store is honest that it never saw the creation. */
	EXPECT_NE(created.key, later.key);
	EXPECT_EQ(2u, later.key.generation);
	EXPECT_TRUE(later.openedWithoutCreation);

	const RtosIdentityStore::Entity* entity = store.find(later.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_FALSE(entity->closed);
}

TEST(RtosIdentityStoreTest, ASecondCreationForAnOpenHandleClosesTheFirstAndSaysSo)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome first = store.observe(task(0x4000), RtosTraceEventType::TaskCreate, 1);
	const RtosIdentityStore::Outcome second = store.observe(task(0x4000), RtosTraceEventType::TaskCreate, 1);

	EXPECT_TRUE(second.implicitClose);
	EXPECT_EQ(2u, second.key.generation);

	const RtosIdentityStore::Entity* oldEntity = store.find(first.key);
	ASSERT_NE(nullptr, oldEntity);
	EXPECT_TRUE(oldEntity->closed);
}

TEST(RtosIdentityStoreTest, TasksAndObjectsWithTheSameAddressAreDifferentEntities)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome asTask = store.observe(task(0x5000), RtosTraceEventType::TaskCreate, 1);
	const RtosIdentityStore::Outcome asObject = store.observe(sync(0x5000), RtosTraceEventType::SyncCreate, 1);

	EXPECT_NE(asTask.key, asObject.key);
	EXPECT_EQ(RtosEntityKind::Task, asTask.key.kind);
	EXPECT_EQ(RtosEntityKind::Sync, asObject.key.kind);
	EXPECT_EQ(2u, store.entityCount());
}

TEST(RtosIdentityStoreTest, ANamedDeletionIsATombstoneWithAnUnknownGeneration)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome outcome = store.observe(task(0x6000), RtosTraceEventType::TaskDelete, 1);

	/* Nothing was ever created, so the generation is a guess - but the fact
	   that something ended here is still worth keeping. */
	const RtosIdentityStore::Entity* entity = store.find(outcome.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_TRUE(entity->closed);
	EXPECT_TRUE(entity->generationUnknown);
	EXPECT_TRUE(outcome.afterGap);
}

TEST(RtosIdentityStoreTest, ALifetimeThatFollowsAGapIsMarked)
{
	RtosIdentityStore store;

	store.noteGap();
	const RtosIdentityStore::Outcome outcome = store.observe(task(0x7000), RtosTraceEventType::TaskCreate, 1);

	EXPECT_TRUE(outcome.afterGap);

	const RtosIdentityStore::Entity* entity = store.find(outcome.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_TRUE(entity->afterGap);

	/* The mark is used up: the next creation is not blamed for the same gap. */
	const RtosIdentityStore::Outcome next = store.observe(task(0x7040), RtosTraceEventType::TaskCreate, 1);
	EXPECT_FALSE(next.afterGap);
}

TEST(RtosIdentityStoreTest, ADictionaryNameSurvivesASnapshotWithAnotherOne)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome outcome = store.observe(task(0x8000), RtosTraceEventType::TaskCreate, 1);

	store.nameFromDictionary(outcome.key, "worker");
	store.nameFromSnapshot(outcome.key, "something-else");

	const RtosIdentityStore::Entity* entity = store.find(outcome.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_EQ(std::string("worker"), entity->name);
	EXPECT_TRUE(entity->nameFromDictionary);
}

TEST(RtosIdentityStoreTest, ASnapshotMayNameAnEntityThatWasOnlyGuessed)
{
	RtosIdentityStore store;

	const RtosIdentityStore::Outcome outcome = store.observe(task(0x9000), RtosTraceEventType::TaskSwitchIn, 1);
	store.nameFromSnapshot(outcome.key, "led");

	const RtosIdentityStore::Entity* entity = store.find(outcome.key);
	ASSERT_NE(nullptr, entity);
	EXPECT_EQ(std::string("led"), entity->name);

	/* The name is presentable; the identity is still a guess. */
	EXPECT_TRUE(entity->generationUnknown);
}

TEST(RtosIdentityStoreTest, ClearingTheStoreIsWhatANewEpochDoes)
{
	RtosIdentityStore store;

	store.observe(task(0xA000), RtosTraceEventType::TaskCreate, 1);
	store.observe(task(0xA000), RtosTraceEventType::TaskCreate, 2);

	store.clear();

	EXPECT_EQ(0u, store.entityCount());

	const RtosIdentityStore::Outcome after = store.observe(task(0xA000), RtosTraceEventType::TaskCreate, 2);
	EXPECT_EQ(1u, after.key.generation);
	EXPECT_EQ(2u, after.key.epoch);
}

TEST(RtosIdentityStoreTest, AnEventWithNoHandleBelongsToNoEntity)
{
	RtosIdentityStore store;

	RtosEntityRef nothing;
	const RtosIdentityStore::Outcome outcome = store.observe(nothing, RtosTraceEventType::TaskSwitchIn, 1);

	EXPECT_EQ(0u, outcome.key.rawHandle);
	EXPECT_EQ(0u, store.entityCount());
}
