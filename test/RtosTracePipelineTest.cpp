#include "RtosTracePipeline.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>

#include "RtosModel.hpp"
#include "RtosRecording.hpp"

namespace
{
	/* A source that hands over exactly the batches the test built. Everything
	   interesting happens above this line; the fake exists so that "above this
	   line" can be tested without a probe. */
	class FakeTraceSource final : public IRtosTraceSource
	{
	   public:
		bool start(RtosTraceProblem& problem) override
		{
			if (refuseToStart)
			{
				problem = RtosTraceProblem::ControlBlockMissing;

				return false;
			}

			sourceState = RtosTraceSourceState::Running;

			return true;
		}

		void stop() override
		{
			sourceState = RtosTraceSourceState::Stopped;
			stopCount++;
		}

		void requestNewEpoch() override
		{
			newEpochRequests++;
		}

		RtosTraceCapabilities capabilities() const override
		{
			return caps;
		}

		RtosTraceSourceState state() const override
		{
			return sourceState;
		}

		const char* name() const override
		{
			return "fake";
		}

		RtosTraceBatch poll(double) override
		{
			if (batches.empty())
			{
				RtosTraceBatch empty;
				empty.cursorAfter = cursor;

				return empty;
			}

			RtosTraceBatch next = batches.front();
			batches.pop_front();
			cursor = next.cursorAfter;

			return next;
		}

		std::deque<RtosTraceBatch> batches;
		RtosTraceCapabilities caps;
		bool refuseToStart = false;
		int stopCount = 0;
		int newEpochRequests = 0;
		uint64_t cursor = 0;

	   private:
		RtosTraceSourceState sourceState = RtosTraceSourceState::NotStarted;
	};

	RtosEntityRef taskRef(uint32_t handle)
	{
		RtosEntityRef ref;
		ref.kind = RtosEntityKind::Task;
		ref.rawHandle = handle;
		ref.quality = RtosIdentityQuality::Exact;

		return ref;
	}

	RtosEntityRef objectRef(uint32_t handle)
	{
		RtosEntityRef ref = taskRef(handle);
		ref.kind = RtosEntityKind::Sync;

		return ref;
	}

	RtosTraceEvent makeEvent(RtosTraceEventType type, uint32_t handle, uint64_t epoch, bool hasTicks, uint32_t ticks)
	{
		RtosTraceEvent event;

		event.epoch = epoch;
		event.type = type;
		event.subject = taskRef(handle);
		event.hasTargetTicks = hasTicks;
		event.targetTicks = ticks;
		event.quality = RtosEvidenceQuality::Good;

		return event;
	}

	RtosTraceBatch batchOf(std::vector<RtosTraceEvent> events, uint64_t from, uint64_t to)
	{
		RtosTraceBatch batch;
		batch.events = std::move(events);
		batch.cursorBefore = from;
		batch.cursorAfter = to;

		return batch;
	}

	RtosSnapshot makeSnapshot(double atSeconds)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = atSeconds;
		snapshot.captureFinishedAt = atSeconds + 0.02;
		snapshot.captureSpanSeconds = 0.02;
		snapshot.consistency = RtosConsistency::BestEffort;
		snapshot.sessionId = "session-recording";

		RtosTask task;
		task.tcbAddress = 0x1000;
		task.name = "worker";

		snapshot.tasks.push_back(task);

		return snapshot;
	}
}  // namespace

TEST(RtosTracePipelineTest, ACommittedBatchBecomesHistoryWithIdentities)
{
	FakeTraceSource* source = nullptr;
	auto owned = std::make_unique<FakeTraceSource>();
	source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskCreate, 0x1000, 1, true, 0),
	                                   makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 40)},
	                                  0, 2));

	const RtosTracePipeline::Pump pump = pipeline.pump(0.01);

	EXPECT_TRUE(pump.committed);
	EXPECT_EQ(2u, pump.applied);
	ASSERT_EQ(2u, pipeline.eventCount());

	/* One lifetime, one key: the switch belongs to the task the creation made. */
	EXPECT_EQ(pipeline.events()[0].subject, pipeline.events()[1].subject);
	EXPECT_EQ(1u, pipeline.events()[0].subject.generation);
	EXPECT_FALSE(pipeline.events()[0].subjectGenerationUnknown);
}

TEST(RtosTracePipelineTest, ATornBatchLeavesTheHistoryEmptyAndTheCursorWhereItWas)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	RtosTraceBatch torn = batchOf({makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 40)}, 0, 1);
	torn.torn = true;

	source->batches.push_back(torn);

	const RtosTracePipeline::Pump pump = pipeline.pump(0.01);

	EXPECT_FALSE(pump.committed);
	EXPECT_TRUE(pump.torn);
	EXPECT_EQ(0u, pump.applied);

	/* Not even the events decoded before the tear: they were read against a
	   record that turned out to be changing. */
	EXPECT_EQ(0u, pipeline.eventCount());
}

TEST(RtosTracePipelineTest, TwoLifetimesOfOneAddressAreTwoKeysInTheHistory)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskCreate, 0x2000, 1, true, 0),
	                                   makeEvent(RtosTraceEventType::TaskDelete, 0x2000, 1, true, 100),
	                                   makeEvent(RtosTraceEventType::TaskCreate, 0x2000, 1, true, 200),
	                                   makeEvent(RtosTraceEventType::TaskSwitchIn, 0x2000, 1, true, 300)},
	                                  0, 4));

	ASSERT_TRUE(pipeline.pump(0.01).committed);
	ASSERT_EQ(4u, pipeline.eventCount());

	const RtosEntityKey first = pipeline.events()[0].subject;
	const RtosEntityKey second = pipeline.events()[2].subject;

	EXPECT_EQ(1u, first.generation);
	EXPECT_EQ(2u, second.generation);
	EXPECT_NE(first, second);

	/* The switch after the second creation belongs to the second lifetime. */
	EXPECT_EQ(second, pipeline.events()[3].subject);

	/* And asking for one lifetime's events gives only that one's. */
	EXPECT_EQ(2u, pipeline.eventsFor(first).size());
}

TEST(RtosTracePipelineTest, DisplaySecondsComeFromTheTargetClock)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	RtosTraceEvent sync;
	sync.epoch = 1;
	sync.type = RtosTraceEventType::ClockSync;
	sync.hasTargetTicks = true;
	sync.targetTicks = 0;
	sync.arg0 = 1000;  /* 1 kHz */

	source->batches.push_back(batchOf({sync, makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 500)}, 0, 2));

	ASSERT_TRUE(pipeline.pump(0.01).committed);
	ASSERT_EQ(2u, pipeline.eventCount());

	EXPECT_TRUE(pipeline.events()[1].hasDisplaySeconds);
	EXPECT_DOUBLE_EQ(0.5, pipeline.events()[1].displaySeconds);
}

TEST(RtosTracePipelineTest, WithoutATargetClockThereAreNoDisplaySeconds)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, false, 0)}, 0, 1));

	ASSERT_TRUE(pipeline.pump(0.01).committed);
	ASSERT_EQ(1u, pipeline.eventCount());

	/* MTF v1: order and counts only. A zero here would sit on the axis at the
	   start of the session and look like a measurement. */
	EXPECT_FALSE(pipeline.events()[0].hasDisplaySeconds);
}

TEST(RtosTracePipelineTest, AGapMarksTheLifetimeThatFollowsIt)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	RtosTraceEvent gap;
	gap.epoch = 1;
	gap.type = RtosTraceEventType::Gap;
	gap.arg0 = 12;

	source->batches.push_back(batchOf({gap, makeEvent(RtosTraceEventType::TaskCreate, 0x3000, 1, true, 10)}, 0, 2));

	const RtosTracePipeline::Pump pump = pipeline.pump(0.01);

	EXPECT_TRUE(pump.committed);
	EXPECT_EQ(1u, pump.gaps);
	ASSERT_EQ(2u, pipeline.eventCount());

	EXPECT_TRUE(pipeline.events()[1].afterGap);

	const RtosIdentityStore::Entity* entity = pipeline.identity().find(pipeline.events()[1].subject);
	ASSERT_NE(nullptr, entity);
	EXPECT_TRUE(entity->afterGap);
}

TEST(RtosTracePipelineTest, EpochBeginStartsTheIdentityOver)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskCreate, 0x4000, 1, true, 0)}, 0, 1));
	ASSERT_TRUE(pipeline.pump(0.01).committed);

	RtosTraceEvent epochBegin;
	epochBegin.epoch = 2;
	epochBegin.type = RtosTraceEventType::EpochBegin;

	source->batches.push_back(batchOf({epochBegin, makeEvent(RtosTraceEventType::TaskCreate, 0x4000, 2, true, 0)}, 1, 2));
	ASSERT_TRUE(pipeline.pump(0.01).committed);

	ASSERT_EQ(3u, pipeline.eventCount());

	/* The same address, a new epoch: new key, generation starts at one again. */
	EXPECT_EQ(1u, pipeline.events()[0].subject.epoch);
	EXPECT_EQ(2u, pipeline.events()[2].subject.epoch);
	EXPECT_EQ(1u, pipeline.events()[2].subject.generation);
	EXPECT_NE(pipeline.events()[0].subject, pipeline.events()[2].subject);
}

TEST(RtosTracePipelineTest, ARefusedStartIsReportedAndClosesTheSource)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();
	source->refuseToStart = true;

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	EXPECT_FALSE(pipeline.start(problem));
	EXPECT_EQ(RtosTraceProblem::ControlBlockMissing, problem);

	/* A source that half started would hold the probe; it is closed here. */
	EXPECT_EQ(1, source->stopCount);
}

TEST(RtosTracePipelineTest, TheModelKeepsTheEventHistoryBesideTheSnapshots)
{
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosModel model;
	EXPECT_EQ(nullptr, model.trace());

	model.attachTraceSource(std::move(owned));
	ASSERT_NE(nullptr, model.trace());

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(model.trace()->start(problem));

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskCreate, 0x5000, 1, true, 0)}, 0, 1));

	const RtosTracePipeline::Pump pump = model.trace()->pump(0.01);

	EXPECT_TRUE(pump.committed);
	EXPECT_EQ(1u, model.trace()->eventCount());
	EXPECT_EQ(std::string("fake"), std::string(model.trace()->sourceName()));

	/* The snapshot side is untouched: no round was published, so the table's
	   source is still empty. Two histories, neither faking the other. */
	EXPECT_EQ(nullptr, model.current());
}

TEST(RtosTracePipelineTest, ReplacingASourceStopsTheOneItReplaces)
{
	auto first = std::make_unique<FakeTraceSource>();
	FakeTraceSource* firstRaw = first.get();

	RtosModel model;
	model.attachTraceSource(std::move(first));

	auto second = std::make_unique<FakeTraceSource>();
	model.attachTraceSource(std::move(second));

	/* Twice on purpose: the replacement stops it, and the pipeline it belonged
	   to is destroyed right after and stops it again. `stop` is documented as
	   safe to call twice, and this is the test that keeps that true. */
	EXPECT_EQ(2, firstRaw->stopCount);
	EXPECT_EQ(RtosTraceSourceState::Stopped, firstRaw->state());
}

TEST(RtosTracePipelineTest, ARecordingKeepsTheEventsAndOpeningItPutsThemBack)
{
	/* The whole point of writing the events down: press record, drive the
	   target, close the program, open the file on another machine, and the bands
	   and the fault list are still there. Before this the file held the rounds
	   and the runs, so everything made from events came back empty. */
	const std::filesystem::path directory = std::filesystem::temp_directory_path() / "variable-trace-model-recording";

	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);

	const std::string path = (directory / "session.vtrec").string();

	RtosModel live;

	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	live.attachTraceSource(std::move(owned));

	RtosTraceProblem traceProblem = RtosTraceProblem::None;

	ASSERT_TRUE(live.trace()->start(traceProblem));

	/* An event that arrives before record is pressed: it belongs to the session
	   and not to the file, because a reader of the file would otherwise think
	   the target began there. */
	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskCreate, 0x1000, 1, true, 0)}, 0, 1));
	live.trace()->pump(0.01);

	RtosRecording::Problem problem = RtosRecording::Problem::None;

	ASSERT_TRUE(live.startRecording(path, problem)) << RtosRecording::describe(problem);

	live.publish(std::make_shared<RtosSnapshot>(makeSnapshot(1.0)));

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 100),
	                                   makeEvent(RtosTraceEventType::IsrEnter, 0x1000, 1, true, 140)},
	                                  1, 3));
	live.trace()->pump(0.01);

	live.publish(std::make_shared<RtosSnapshot>(makeSnapshot(1.5)));

	live.stopRecording();

	RtosModel playback;

	ASSERT_TRUE(playback.openRecording(path, problem)) << RtosRecording::describe(problem);

	ASSERT_NE(nullptr, playback.trace());
	EXPECT_TRUE(playback.trace()->attached());
	EXPECT_TRUE(playback.trace()->isReplay());
	EXPECT_EQ("fake", std::string(playback.trace()->sourceName()));

	/* Two, not three: the creation happened before record was pressed. */
	EXPECT_EQ(2u, playback.trace()->eventCount());
	ASSERT_LT(0u, playback.trace()->eventCount());
	EXPECT_EQ(RtosTraceEventType::TaskSwitchIn, playback.trace()->events()[0].event.type);
	EXPECT_EQ(RtosTraceEventType::IsrEnter, playback.trace()->events()[1].event.type);

	/* The names travel with the file, or the bands would be unlabelled. */
	EXPECT_GT(playback.trace()->identity().entityCount(), 0u);

	EXPECT_TRUE(playback.isPlayback());
	EXPECT_EQ(2u, playback.playbackInfo().rounds);

	/* The panel prints this beside the round counter and the export names its
	   CSV after it. A v3 playback used to come back with an empty path, which
	   read as a file with no name. */
	EXPECT_EQ(path, playback.playbackInfo().path);

	std::filesystem::remove_all(directory);
}

TEST(RtosTracePipelineTest, TheAxisStartsWhereTheUnitIsKnownNotWhereTheDataDoes)
{
	/* A stream carries ticks and nothing that says what they are in: the unit
	   arrives with the first ClockSync, a second after the first records. An
	   origin taken at the first record makes every later duration span a part
	   of the axis whose frequency nobody stated, and the pipeline answers "not
	   computable" for all of them - measured on the wire: 46774 events, every
	   one untimed. */
	auto owned = std::make_unique<FakeTraceSource>();
	FakeTraceSource* source = owned.get();

	RtosTracePipeline pipeline(std::move(owned));

	RtosTraceProblem problem = RtosTraceProblem::None;
	ASSERT_TRUE(pipeline.start(problem));

	RtosTraceEvent clockSync = makeEvent(RtosTraceEventType::ClockSync, 0, 1, true, 3000);
	clockSync.arg0 = 1000000;

	source->batches.push_back(batchOf({makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 1000),
	                                   makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 2000), clockSync,
	                                   makeEvent(RtosTraceEventType::TaskSwitchIn, 0x1000, 1, true, 4000)},
	                                  0, 4));

	ASSERT_TRUE(pipeline.pump(0.01).committed);
	ASSERT_EQ(4u, pipeline.eventCount());

	/* Before the sync there is no unit, so no time: saying zero would be a
	   measurement nobody made. */
	EXPECT_FALSE(pipeline.events()[0].hasDisplaySeconds);
	EXPECT_FALSE(pipeline.events()[1].hasDisplaySeconds);

	ASSERT_TRUE(pipeline.events()[3].hasDisplaySeconds);
	EXPECT_NEAR(0.001, pipeline.events()[3].displaySeconds, 1e-9);
}
