#include "TraceAdapters.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "RtosModel.hpp"
#include "RtosTracePipeline.hpp"

namespace
{
	RtosEventRing::Record record(uint32_t tcb, uint32_t microseconds)
	{
		RtosEventRing::Record made;
		made.tcbAddress = tcb;
		made.timeUs = microseconds;

		return made;
	}

	/* One software-source packet on the record port carrying four bytes: the
	   framing `RtosItmDecoder` documents. Two of them make one eight byte
	   record. */
	void appendPacket(std::vector<uint8_t>& out, uint32_t value)
	{
		out.push_back(static_cast<uint8_t>((RtosItmDecoder::recordPort << 3) | 0x03));

		for (int index = 0; index < 4; index++)
			out.push_back(static_cast<uint8_t>((value >> (index * 8)) & 0xFFu));
	}
}  // namespace

TEST(MtfTraceAdapterTest, RecordsBecomeSwitchEventsWithTheTargetClock)
{
	MtfTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	adapter.pushRecords({record(0x20001000u, 1500), record(0x20001040u, 2500)});

	const RtosTraceBatch batch = adapter.poll(0.01);

	ASSERT_EQ(2u, batch.events.size());
	EXPECT_EQ(RtosTraceEventType::TaskSwitchIn, batch.events[0].type);
	EXPECT_EQ(0x20001000u, batch.events[0].subject.rawHandle);
	EXPECT_EQ(RtosEntityKind::Task, batch.events[0].subject.kind);

	/* The record carries the target's microseconds, so the event has a real
	   target time - not the host's arrival time dressed up as one. */
	EXPECT_TRUE(batch.events[0].hasTargetTicks);
	EXPECT_EQ(1500u, batch.events[0].targetTicks);
	EXPECT_EQ(1000000u, batch.events[0].targetHz);
}

TEST(MtfTraceAdapterTest, BytesInTheItmFramingComeOutAsRecords)
{
	MtfTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	std::vector<uint8_t> bytes;
	appendPacket(bytes, 0x20001000u);
	appendPacket(bytes, 700u);

	adapter.feed(bytes.data(), bytes.size());

	const RtosTraceBatch batch = adapter.poll(0.01);

	ASSERT_EQ(1u, batch.events.size());
	EXPECT_EQ(0x20001000u, batch.events[0].subject.rawHandle);
	EXPECT_EQ(700u, batch.events[0].targetTicks);
}

TEST(MtfTraceAdapterTest, HalfARecordWaitsForTheRestOfIt)
{
	MtfTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	std::vector<uint8_t> first;
	appendPacket(first, 0x20001000u);

	adapter.feed(first.data(), first.size());
	EXPECT_EQ(0u, adapter.poll(0.01).events.size());

	std::vector<uint8_t> second;
	appendPacket(second, 900u);

	adapter.feed(second.data(), second.size());

	const RtosTraceBatch batch = adapter.poll(0.01);
	ASSERT_EQ(1u, batch.events.size());
	EXPECT_EQ(900u, batch.events[0].targetTicks);
}

TEST(MtfTraceAdapterTest, ItReportsDegradedAndWithoutTheCapabilitiesItLacks)
{
	MtfTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	/* Degraded, not running: switches and nothing else. A view built on this
	   source must be able to say so instead of drawing empty ISR lanes. */
	EXPECT_EQ(RtosTraceSourceState::Degraded, adapter.state());

	const RtosTraceCapabilities capabilities = adapter.capabilities();
	EXPECT_TRUE(capabilities.realHandles());
	EXPECT_TRUE(capabilities.targetClock());
	EXPECT_FALSE(capabilities.lossDetection());
	EXPECT_FALSE(capabilities.lifecycle());
	EXPECT_FALSE(capabilities.isrEvents());
}

TEST(SwdTraceAdapterTest, ALostRecordBecomesAGapBeforeTheRecordsThatSurvived)
{
	SwdTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	adapter.pushRecords({record(0x20001000u, 100), record(0x20001040u, 200)}, 7, false);

	const RtosTraceBatch batch = adapter.poll(0.01);

	ASSERT_EQ(3u, batch.events.size());

	/* The gap goes first, so the interval across it is closed rather than
	   stretched into one long run. */
	EXPECT_EQ(RtosTraceEventType::Gap, batch.events[0].type);
	EXPECT_EQ(7u, batch.events[0].arg0);
	EXPECT_EQ(static_cast<uint32_t>(RtosGapReason::RingOverwritten), batch.events[0].arg1);

	EXPECT_EQ(RtosTraceEventType::TaskSwitchIn, batch.events[1].type);
}

TEST(SwdTraceAdapterTest, ARestartedRingOpensANewEpoch)
{
	SwdTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	adapter.pushRecords({record(0x20001000u, 100)});
	EXPECT_EQ(1u, adapter.poll(0.01).events[0].epoch);

	/* The target's ring went back to zero: a new run, its clock starting over.
	   Continuing the old numbering would join two sessions into one axis. */
	adapter.pushRecords({record(0x20001040u, 50)}, 0, true);

	const RtosTraceBatch batch = adapter.poll(0.01);

	ASSERT_EQ(2u, batch.events.size());
	EXPECT_EQ(RtosTraceEventType::EpochBegin, batch.events[0].type);
	EXPECT_EQ(2u, batch.events[0].epoch);
	EXPECT_EQ(2u, batch.events[1].epoch);
}

TEST(SwdTraceAdapterTest, ItCountsLossesBecauseItCan)
{
	SwdTraceAdapter adapter;

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(adapter.start(problem));

	EXPECT_TRUE(adapter.capabilities().lossDetection());

	/* The ITM path cannot tell a quiet target from a lost byte; this one can,
	   and the capability bit is what lets the rules ask for an interval across
	   a gap instead of guessing. */
	EXPECT_FALSE(MtfTraceAdapter().capabilities().lossDetection());
}

TEST(TraceAdaptersTest, ALegacySourceThroughThePipelineStillKeysByLifetime)
{
	auto adapter = std::make_unique<SwdTraceAdapter>();
	SwdTraceAdapter* raw = adapter.get();

	RtosTracePipeline pipeline(std::move(adapter));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	raw->pushRecords({record(0x20001000u, 100), record(0x20001000u, 300)});
	ASSERT_TRUE(pipeline.pump(0.01).committed);

	ASSERT_EQ(2u, pipeline.eventCount());

	/* No lifecycle on this carrier: the generation is a guess and the entity
	   says so. What it must not do is pretend two lifetimes are one. */
	EXPECT_EQ(pipeline.events()[0].subject, pipeline.events()[1].subject);
	EXPECT_TRUE(pipeline.events()[0].subjectGenerationUnknown);

	/* And the microseconds become seconds on the session axis. */
	EXPECT_TRUE(pipeline.events()[1].hasDisplaySeconds);
	EXPECT_DOUBLE_EQ(0.0002, pipeline.events()[1].displaySeconds);
}

TEST(TraceAdaptersTest, ThePipelineCanNameWhatASourceCannotProvide)
{
	auto adapter = std::make_unique<MtfTraceAdapter>();

	RtosTracePipeline pipeline(std::move(adapter));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	const std::vector<std::string> missing = pipeline.missingCapabilities();

	auto contains = [&missing](const std::string& name)
	{
		return std::find(missing.begin(), missing.end(), name) != missing.end();
	};

	/* Named rather than counted: "no ISR events" can be acted on, "three bits
	   clear" cannot. */
	EXPECT_TRUE(contains("isrEvents"));
	EXPECT_TRUE(contains("lifecycleEvents"));
	EXPECT_FALSE(contains("rawHandle32"));
	EXPECT_FALSE(contains("targetClock"));
}

/*
 * The wiring: the acquisition thread reads the ring, and the same runs reach
 * the event side through one call.
 *
 * This is the arrangement the program actually uses - one reader on the probe,
 * fed to the trace pipeline - so the test drives it the same way rather than
 * giving the adapter a private feed of its own.
 */
TEST(TraceAdaptersTest, TheModelsEventSideGetsTheRunsTheAcquisitionThreadRead)
{
	RtosModel model;
	model.attachTraceSource(std::make_unique<SwdTraceAdapter>());

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(model.trace()->start(problem));

	RtosEventRing::Interval first;
	first.tcbAddress = 0x20001000u;
	first.fromSeconds = 1.25;
	first.toSeconds = 1.5;

	RtosEventRing::Interval second;
	second.tcbAddress = 0x20001040u;
	second.fromSeconds = 1.5;
	second.toSeconds = 1.75;

	model.appendEventIntervals({first, second}, 60.0);

	ASSERT_NE(nullptr, model.trace());
	ASSERT_EQ(2u, model.trace()->eventCount());

	const RtosNormalizedEvent& event = model.trace()->events()[0];
	EXPECT_EQ(RtosTraceEventType::TaskSwitchIn, event.event.type);
	EXPECT_EQ(0x20001000u, event.event.subject.rawHandle);

	/* The run's start is the moment of the switch, and it is placed on the
	   session's axis rather than left without a time. */
	ASSERT_TRUE(event.hasDisplaySeconds);
	EXPECT_DOUBLE_EQ(0.0, event.displaySeconds);

	/* And the old history still got them: one reader, two histories. */
	EXPECT_EQ(2u, model.events().size());
}

TEST(TraceAdaptersTest, WithoutASourceTheRunsStillReachTheOldHistory)
{
	RtosModel model;

	RtosEventRing::Interval only;
	only.tcbAddress = 0x20001000u;
	only.fromSeconds = 1.0;
	only.toSeconds = 1.2;

	/* No event source attached: the call must be harmless, which is what keeps
	   the snapshot path independent of the trace layer. */
	model.appendEventIntervals({only}, 60.0);

	EXPECT_EQ(nullptr, model.trace());
	EXPECT_EQ(1u, model.events().size());
}
