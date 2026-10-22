#include "IRtosTraceSource.hpp"
#include "RtosTraceTypes.hpp"

#include <gtest/gtest.h>

#include <set>

/*
 * These tests hold the parts of the contract that a document cannot: the wire
 * numbers, the definition of a stable identity, and the rule that only a clean
 * batch moves the cursor. If any of them ever needs changing, the protocol
 * changed, and the documents have to change with it.
 */

TEST(RtosTraceTypesTest, EventTypeNumbersMatchTheWireFormat)
{
	/* §3.1 of the wire format. The numbers travel on the wire; the names do
	   not. Renumbering a constant would silently reinterpret every capture. */
	EXPECT_EQ(0x01, static_cast<int>(RtosTraceEventType::EpochBegin));
	EXPECT_EQ(0x03, static_cast<int>(RtosTraceEventType::Gap));
	EXPECT_EQ(0x10, static_cast<int>(RtosTraceEventType::TaskCreate));
	EXPECT_EQ(0x13, static_cast<int>(RtosTraceEventType::TaskSwitchIn));
	EXPECT_EQ(0x16, static_cast<int>(RtosTraceEventType::TaskBlock));
	EXPECT_EQ(0x22, static_cast<int>(RtosTraceEventType::WaitBegin));
	EXPECT_EQ(0x30, static_cast<int>(RtosTraceEventType::IsrEnter));
	EXPECT_EQ(0x40, static_cast<int>(RtosTraceEventType::Fault));
	EXPECT_EQ(0x51, static_cast<int>(RtosTraceEventType::Free));
}

TEST(RtosTraceTypesTest, LifetimeOpenersAndClosersAreTheOnlyOnesThatAre)
{
	EXPECT_TRUE(opensLifetime(RtosTraceEventType::TaskCreate));
	EXPECT_TRUE(opensLifetime(RtosTraceEventType::SyncCreate));
	EXPECT_TRUE(closesLifetime(RtosTraceEventType::TaskDelete));
	EXPECT_TRUE(closesLifetime(RtosTraceEventType::SyncDelete));

	/* A switch does not create anything: a task first seen by a switch in has
	   an unknown generation, and the store has to treat it as such. */
	EXPECT_FALSE(opensLifetime(RtosTraceEventType::TaskSwitchIn));
	EXPECT_FALSE(closesLifetime(RtosTraceEventType::TaskSwitchOut));
	EXPECT_FALSE(opensLifetime(RtosTraceEventType::Fault));
}

TEST(RtosTraceTypesTest, OnlyAnExactHandleWithAGenerationIsStable)
{
	RtosEntityRef ref;
	ref.kind = RtosEntityKind::Task;
	ref.rawHandle = 0x20001B60u;
	ref.generation = 3;
	ref.quality = RtosIdentityQuality::Exact;

	EXPECT_TRUE(ref.isStable());

	/* A legacy short id may be a perfectly good slot for as long as nobody
	   reuses it - which is exactly why it cannot key a history. */
	RtosEntityRef legacy = ref;
	legacy.quality = RtosIdentityQuality::Legacy;
	legacy.legacyId = 7;
	EXPECT_FALSE(legacy.isStable());

	RtosEntityRef noGeneration = ref;
	noGeneration.generation = 0;
	EXPECT_FALSE(noGeneration.isStable());

	RtosEntityRef noHandle = ref;
	noHandle.rawHandle = 0;
	EXPECT_FALSE(noHandle.isStable());
}

TEST(RtosTraceTypesTest, TwoGenerationsOfOneAddressAreTwoEntities)
{
	RtosEntityRef first;
	first.kind = RtosEntityKind::Task;
	first.rawHandle = 0x20001B60u;
	first.generation = 1;
	first.quality = RtosIdentityQuality::Exact;

	RtosEntityRef second = first;
	second.generation = 2;

	const RtosEntityKey a = RtosEntityKey::from(first, 5);
	const RtosEntityKey b = RtosEntityKey::from(second, 5);

	EXPECT_NE(a, b);
	EXPECT_TRUE(a < b);
}

TEST(RtosTraceTypesTest, TheSameAddressInANewEpochIsANewEntity)
{
	RtosEntityRef ref;
	ref.kind = RtosEntityKind::Task;
	ref.rawHandle = 0x20001B60u;
	ref.generation = 1;
	ref.quality = RtosIdentityQuality::Exact;

	EXPECT_NE(RtosEntityKey::from(ref, 1), RtosEntityKey::from(ref, 2));
}

TEST(RtosTraceTypesTest, KeysSortByEpochThenKindThenHandleThenGeneration)
{
	RtosEntityRef task = {RtosEntityKind::Task, 0x100, 1, 0, RtosIdentityQuality::Exact};
	RtosEntityRef sync = {RtosEntityKind::Sync, 0x100, 1, 0, RtosIdentityQuality::Exact};

	std::set<RtosEntityKey> keys;

	keys.insert(RtosEntityKey::from(task, 2));
	keys.insert(RtosEntityKey::from(task, 1));
	keys.insert(RtosEntityKey::from(sync, 1));

	ASSERT_EQ(3u, keys.size());

	/* Epoch first: the oldest session comes first. */
	EXPECT_EQ(1u, keys.begin()->epoch);
}

TEST(RtosTraceTypesTest, CapabilityBitsReadAsTheDocumentNamesThem)
{
	RtosTraceCapabilities capabilities;
	capabilities.set(0, true);  /* rawHandle32 */
	capabilities.set(3, true);  /* targetClock */
	capabilities.set(1, false);

	EXPECT_TRUE(capabilities.realHandles());
	EXPECT_TRUE(capabilities.targetClock());
	EXPECT_FALSE(capabilities.lifecycle());
	EXPECT_FALSE(capabilities.faultEvents());

	EXPECT_EQ(std::string("rawHandle32"), capabilityName(0));
	EXPECT_EQ(std::string("targetClock"), capabilityName(3));
	EXPECT_EQ(std::string("reserved"), capabilityName(31));
}

TEST(RtosTraceTypesTest, ACleanBatchMovesTheCursor)
{
	RtosTraceBatch batch;
	batch.cursorBefore = 100;
	batch.cursorAfter = 120;

	EXPECT_TRUE(batch.canCommit());
	EXPECT_EQ(120u, batch.committedCursor(100));
}

TEST(RtosTraceTypesTest, ATornBatchLeavesTheCursorWhereItWas)
{
	RtosTraceBatch batch;
	batch.cursorBefore = 100;
	batch.cursorAfter = 120;
	batch.torn = true;
	batch.events.resize(3);

	/* The events decoded before the tear are not trustworthy either: they were
	   read against a record that turned out to be changing. */
	EXPECT_FALSE(batch.canCommit());
	EXPECT_EQ(100u, batch.committedCursor(100));
}

TEST(RtosTraceTypesTest, AFailedReadLeavesTheCursorWhereItWas)
{
	RtosTraceBatch batch;
	batch.cursorBefore = 100;
	batch.cursorAfter = 140;
	batch.readFailed = true;

	EXPECT_FALSE(batch.canCommit());
	EXPECT_EQ(100u, batch.committedCursor(100));
}

TEST(RtosTraceTypesTest, AnEmptyCleanBatchStillCommits)
{
	/* Polling a quiet target must advance the cursor to the same place, not
	   leave the caller thinking nothing happened. */
	RtosTraceBatch batch;
	batch.cursorBefore = 42;
	batch.cursorAfter = 42;

	EXPECT_TRUE(batch.canCommit());
	EXPECT_EQ(42u, batch.committedCursor(10));
}
