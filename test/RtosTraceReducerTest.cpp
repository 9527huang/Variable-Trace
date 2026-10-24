#include "RtosTraceReducer.hpp"

#include <gtest/gtest.h>

#include <set>

namespace
{
	RtosNormalizedEvent make(RtosTraceEventType type, uint32_t subject, uint32_t object, uint32_t arg0, uint32_t arg1)
	{
		RtosNormalizedEvent normalized;
		normalized.event.type = type;
		normalized.event.subject.rawHandle = subject;
		normalized.event.subject.kind = RtosEntityKind::Task;
		normalized.event.object.rawHandle = object;
		normalized.event.object.kind = object != 0 ? RtosEntityKind::Sync : RtosEntityKind::None;
		normalized.event.arg0 = arg0;
		normalized.event.arg1 = arg1;

		return normalized;
	}

	RtosNormalizedEvent at(RtosNormalizedEvent normalized, double seconds)
	{
		normalized.hasDisplaySeconds = true;
		normalized.displaySeconds = seconds;

		return normalized;
	}
}  // namespace

TEST(RtosTraceReducerTest, NestedInterruptsBecomeIntervalsInsideEachOther)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::IsrEnter, 0, 0, 5, 1), 0.0));
	events.push_back(at(make(RtosTraceEventType::IsrEnter, 0, 0, 6, 2), 0.5));
	events.push_back(at(make(RtosTraceEventType::IsrExit, 0, 0, 6, 2), 1.0));
	events.push_back(at(make(RtosTraceEventType::IsrExit, 0, 0, 5, 1), 2.0));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_EQ(2u, projection.isr.size());
	EXPECT_EQ(0u, projection.unclosedIsr);
	EXPECT_EQ(0u, projection.orphanIsr);

	/* The inner one closes first, which is what a stack does. */
	EXPECT_EQ(6u, projection.isr[0].address);
	EXPECT_DOUBLE_EQ(0.5, projection.isr[0].fromSeconds);
	EXPECT_DOUBLE_EQ(1.0, projection.isr[0].toSeconds);
	EXPECT_EQ(2u, projection.isr[0].depth);

	EXPECT_EQ(5u, projection.isr[1].address);
	EXPECT_DOUBLE_EQ(0.0, projection.isr[1].fromSeconds);
	EXPECT_DOUBLE_EQ(2.0, projection.isr[1].toSeconds);
}

TEST(RtosTraceReducerTest, AnInterruptThatNeverLeftIsCountedNotDrawn)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::IsrEnter, 0, 0, 5, 1), 1.0));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	/* An interval of unknown length is not a shorter interval. The count is the
	   answer; the drawing has nothing to draw. */
	EXPECT_TRUE(projection.isr.empty());
	EXPECT_EQ(1u, projection.unclosedIsr);
}

TEST(RtosTraceReducerTest, AnExitWithoutAnEntryIsCountedAsOrphan)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::IsrExit, 0, 0, 5, 1), 1.0));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	EXPECT_TRUE(projection.isr.empty());
	EXPECT_EQ(1u, projection.orphanIsr);
}

TEST(RtosTraceReducerTest, AFaultKeepsItsRegistersAndSaysWhichWereThere)
{
	RtosNormalizedEvent fault = at(make(RtosTraceEventType::Fault, 0, 0, 0, 0), 3.5);

	/* class UsageFault, flags hasCFSR | hasPC, CFSR INVSTATE, PC 0x08002010. */
	const uint8_t payload[12] = {1, 0x03, 0x02, 0x00, 0x10, 0x20, 0x00, 0x08, 0, 0, 0, 0};
	fault.event.payloadBytes = 12;

	for (uint8_t index = 0; index < 12; index++)
		fault.event.payload[index] = payload[index];

	std::vector<RtosNormalizedEvent> events;
	events.push_back(fault);

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_EQ(1u, projection.fault.size());
	EXPECT_EQ(1u, projection.fault[0].faultClass);
	EXPECT_EQ(0x02u, projection.fault[0].cfsr);
	EXPECT_EQ(0x08002010u, projection.fault[0].pc);
	EXPECT_DOUBLE_EQ(3.5, projection.fault[0].fromSeconds);

	/* LR was not in the frame and is left at zero with its flag clear: a zero
	   that means "missing" must not be mistaken for an address. */
	EXPECT_EQ(0u, projection.fault[0].lr);
	EXPECT_EQ(0u, projection.fault[0].faultFlags & 0x04u);
}

TEST(RtosTraceReducerTest, AWaitIsPairedByTaskAndObject)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::WaitBegin, 0x1000, 0x2000, 100, 0), 1.0));
	events.push_back(at(make(RtosTraceEventType::WaitBegin, 0x1040, 0x2000, 50, 0), 1.2));
	events.push_back(at(make(RtosTraceEventType::WaitEnd, 0x1040, 0x2000, 0, 0), 1.4));
	events.push_back(at(make(RtosTraceEventType::WaitEnd, 0x1000, 0x2000, 1, 0), 2.0));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_EQ(2u, projection.sync.size());

	/* Two tasks waiting on one object are two waits, not one: the key is the
	   pair, and the first one to end is the second one to start. */
	EXPECT_EQ(0x1040u, events[projection.sync[0].firstEvent].event.subject.rawHandle);
	EXPECT_EQ(0u, projection.sync[0].waitResult); /* signalled */
	EXPECT_DOUBLE_EQ(0.2, projection.sync[0].toSeconds - projection.sync[0].fromSeconds);

	EXPECT_EQ(0x1000u, events[projection.sync[1].firstEvent].event.subject.rawHandle);
	EXPECT_EQ(1u, projection.sync[1].waitResult); /* timeout */
	EXPECT_DOUBLE_EQ(1.0, projection.sync[1].toSeconds - projection.sync[1].fromSeconds);

	EXPECT_EQ(0u, projection.unclosedSync);
	EXPECT_EQ(0u, projection.orphanSync);
}

TEST(RtosTraceReducerTest, AnUnclosedWaitIsCountedAndAnOrphanEndIsToo)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::WaitBegin, 0x1000, 0x2000, 100, 0), 1.0));
	events.push_back(at(make(RtosTraceEventType::WaitEnd, 0x9999, 0x2000, 0, 0), 2.0));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	EXPECT_EQ(1u, projection.unclosedSync);
	EXPECT_EQ(1u, projection.orphanSync);
}

TEST(RtosTraceReducerTest, HeapIsANetAndAnUnknownSizeIsNotZero)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::Alloc, 0, 0, 0x8000, 64), 1.0));
	events.push_back(at(make(RtosTraceEventType::Alloc, 0, 0, 0x8100, 32), 1.1));
	events.push_back(at(make(RtosTraceEventType::Free, 0, 0, 0x8000, 64), 1.2));
	events.push_back(at(make(RtosTraceEventType::Free, 0, 0, 0x8100, 0), 1.3));  /* size unknown */

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_EQ(4u, projection.heap.size());

	/* 64 in, 64 out, 32 in: the net is 32, and the free of unknown size is not
	   counted as zero bytes out of a number that would then be too small. */
	EXPECT_EQ(32, projection.heapNetBytes);
	EXPECT_EQ(1u, projection.heapUnknownSize);
	EXPECT_TRUE(projection.heap[0].allocated);
	EXPECT_FALSE(projection.heap[2].allocated);
}

TEST(RtosTraceReducerTest, WithoutATargetClockTheEntriesExistButHaveNoLength)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(make(RtosTraceEventType::IsrEnter, 0, 0, 5, 1));
	events.push_back(make(RtosTraceEventType::IsrExit, 0, 0, 5, 1));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	/* MTF v1: the order is real, the duration is not. Saying "0 ms" would be a
	   measurement nobody made. */
	ASSERT_EQ(1u, projection.isr.size());
	EXPECT_FALSE(projection.isr[0].hasSeconds);
}

TEST(RtosTraceReducerTest, AGapIsItsOwnEntryWithTheReasonItHappened)
{
	RtosNormalizedEvent gap = at(make(RtosTraceEventType::Gap, 0, 0, 12, 1), 4.0);

	std::vector<RtosNormalizedEvent> events;
	events.push_back(gap);

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_EQ(1u, projection.gap.size());
	EXPECT_EQ(12u, projection.gap[0].lostRecords);
	EXPECT_EQ(1u, projection.gap[0].gapReason);
}

TEST(RtosTraceReducerTest, SwitchesMakeTheirOwnTrack)
{
	std::vector<RtosNormalizedEvent> events;

	events.push_back(at(make(RtosTraceEventType::TaskSwitchIn, 0x1000, 0, 0, 0), 1.0));
	events.push_back(at(make(RtosTraceEventType::TaskSwitchIn, 0x1040, 0, 0, 0), 1.1));
	events.push_back(at(make(RtosTraceEventType::TaskSwitchIn, 0x1000, 0, 0, 0), 1.2));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	/* The lanes show who ran when; this track shows how busy the scheduler was,
	   which is not readable from a boundary between two colours. */
	ASSERT_EQ(3u, projection.task.size());
	EXPECT_EQ(0x1000u, projection.task[0].address);
	EXPECT_DOUBLE_EQ(1.1, projection.task[1].fromSeconds);
}

TEST(RtosTraceReducerTest, ASwitchWithoutATimeIsStillTrackedButHasNoPlace)
{
	std::vector<RtosNormalizedEvent> events;
	events.push_back(make(RtosTraceEventType::TaskSwitchIn, 0x1000, 0, 0, 0));

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_EQ(1u, projection.task.size());
	EXPECT_FALSE(projection.task[0].hasSeconds);
}

TEST(RtosTraceReducerTest, EveryTrackIsListedOnceForTheCallersThatNeedAllOfThem)
{
	/* The window the bands are drawn against has to consider every track. It did
	   not, when the task track was added, and the bands drew against a made up
	   0..1 second axis - a picture that looked like data over an axis that was
	   fiction. This list is the thing those callers loop over. */
	const RtosTrackProjection projection = RtosTraceReducer::project({});

	EXPECT_EQ(6u, projection.tracks().size());

	std::set<const void*> unique;

	for (const std::vector<RtosTrackEntry>* track : projection.tracks())
		unique.insert(track);

	EXPECT_EQ(6u, unique.size());
}

/*
 * The span the events cover.
 *
 * The bands are drawn against the strip's window, and the strip's clock starts
 * when acquisition did while the events' starts at the first event. A band over
 * a stretch no event covers reads as "nothing happened", so the panel needs to
 * know where the events actually are - and that has to come out of the reducer,
 * which is the only place that has seen them all.
 */
TEST(RtosTraceReducerTest, TheSpanCoversTheEventsThatCarryATime)
{
	const std::vector<RtosNormalizedEvent> events = {
	    at(make(RtosTraceEventType::TaskSwitchIn, 0x1000, 0, 0, 0), 4.0),
	    at(make(RtosTraceEventType::IsrEnter, 0, 0, 1, 1), 9.5),
	    at(make(RtosTraceEventType::IsrExit, 0, 0, 1, 1), 9.6),
	    at(make(RtosTraceEventType::TaskSwitchIn, 0x1040, 0, 0, 0), 6.0),
	};

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_TRUE(projection.hasSpan);
	EXPECT_DOUBLE_EQ(4.0, projection.firstSeconds);
	EXPECT_DOUBLE_EQ(9.6, projection.lastSeconds);
}

TEST(RtosTraceReducerTest, AnEventWithoutATimeDoesNotStretchTheSpan)
{
	std::vector<RtosNormalizedEvent> events = {
	    at(make(RtosTraceEventType::TaskSwitchIn, 0x1000, 0, 0, 0), 4.0),
	    make(RtosTraceEventType::Fault, 0x20000, 0, 0x1000, 0),
	};

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	ASSERT_TRUE(projection.hasSpan);
	EXPECT_DOUBLE_EQ(4.0, projection.firstSeconds);
	EXPECT_DOUBLE_EQ(4.0, projection.lastSeconds);
}

TEST(RtosTraceReducerTest, NoTimedEventsMeansNoSpan)
{
	const std::vector<RtosNormalizedEvent> events = {make(RtosTraceEventType::TaskSwitchIn, 0x1000, 0, 0, 0)};

	const RtosTrackProjection projection = RtosTraceReducer::project(events);

	EXPECT_FALSE(projection.hasSpan);
}
