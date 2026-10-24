#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "RtosRecording.hpp"
#include "RtosSnapshot.hpp"
#include "RtosTraceArchive.hpp"
#include "RtosTracePipeline.hpp"
#include "RtosTraceReducer.hpp"

namespace
{
	RtosNormalizedEvent makeEvent(uint32_t ticks, RtosTraceEventType type, uint32_t subjectHandle, uint32_t objectHandle)
	{
		RtosNormalizedEvent normalized;

		normalized.event.epoch = 7;
		normalized.event.sequence = ticks / 10;
		normalized.event.hasTargetTicks = true;
		normalized.event.targetTicks = ticks;
		normalized.event.targetHz = 1000000;
		normalized.event.coreId = 0;
		normalized.event.type = type;
		normalized.event.arg0 = 0xDEADBEEFu;
		normalized.event.arg1 = 0x1234u;
		normalized.event.quality = RtosEvidenceQuality::Good;

		normalized.event.payloadBytes = 3;
		normalized.event.payload[0] = 0x11;
		normalized.event.payload[1] = 0x22;
		normalized.event.payload[2] = 0x33;

		normalized.event.subject.kind = RtosEntityKind::Task;
		normalized.event.subject.rawHandle = subjectHandle;
		normalized.event.subject.generation = 2;
		normalized.event.subject.quality = RtosIdentityQuality::Exact;

		normalized.event.object.kind = RtosEntityKind::Sync;
		normalized.event.object.rawHandle = objectHandle;
		normalized.event.object.generation = 1;
		normalized.event.object.quality = RtosIdentityQuality::Inferred;

		normalized.subject = RtosEntityKey::from(normalized.event.subject, 7);
		normalized.object = RtosEntityKey::from(normalized.event.object, 7);

		normalized.subjectGenerationUnknown = true;
		normalized.afterGap = true;
		normalized.hasDisplaySeconds = true;
		normalized.displaySeconds = 3.25 + ticks / 1000000.0;

		return normalized;
	}

	RtosSnapshot makeRound(double atSeconds)
	{
		RtosSnapshot snapshot;

		snapshot.captureStartedAt = atSeconds;
		snapshot.captureFinishedAt = atSeconds + 0.05;
		snapshot.captureSpanSeconds = 0.05;
		snapshot.consistency = RtosConsistency::BestEffort;
		snapshot.sessionId = "session-archive";

		RtosTask task;
		task.tcbAddress = 0x20001100u;
		task.name = "worker";
		task.state = RtosTaskState::Running;

		snapshot.tasks.push_back(task);

		return snapshot;
	}

	class ArchiveFile : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			directory = std::filesystem::temp_directory_path() / "variable-trace-archive-test";
			std::filesystem::remove_all(directory);
			std::filesystem::create_directories(directory);
		}

		void TearDown() override
		{
			std::filesystem::remove_all(directory);
		}

		std::string path(const std::string& name) const
		{
			return (directory / name).string();
		}

		std::filesystem::path directory;
	};
}  // namespace

TEST(RtosTraceArchiveTest, AnEventSurvivesTheRoundTrip)
{
	/* Every field is checked, and the check is on the value rather than on the
	   size: a field that lands in the wrong place keeps the size and changes the
	   meaning, which is the failure this encoding can have. */
	std::vector<RtosNormalizedEvent> written;
	written.push_back(makeEvent(1000, RtosTraceEventType::TaskSwitchIn, 0x20001100u, 0));
	written.push_back(makeEvent(1040, RtosTraceEventType::WaitBegin, 0x20001100u, 0x20002000u));

	std::vector<RtosNormalizedEvent> read;

	ASSERT_TRUE(RtosTraceArchive::decodeEvents(RtosTraceArchive::encodeEvents(written), read));
	ASSERT_EQ(2u, read.size());

	const RtosNormalizedEvent& event = read[1];

	EXPECT_EQ(7u, event.event.epoch);
	EXPECT_EQ(104u, event.event.sequence);
	EXPECT_TRUE(event.event.hasTargetTicks);
	EXPECT_EQ(1040u, event.event.targetTicks);
	EXPECT_EQ(1000000u, event.event.targetHz);
	EXPECT_EQ(RtosTraceEventType::WaitBegin, event.event.type);
	EXPECT_EQ(0xDEADBEEFu, event.event.arg0);
	EXPECT_EQ(RtosEntityKind::Task, event.event.subject.kind);
	EXPECT_EQ(0x20001100u, event.event.subject.rawHandle);
	EXPECT_EQ(2u, event.event.subject.generation);
	EXPECT_EQ(RtosIdentityQuality::Exact, event.event.subject.quality);
	EXPECT_EQ(RtosEntityKind::Sync, event.event.object.kind);
	EXPECT_EQ(0x20002000u, event.event.object.rawHandle);
	EXPECT_EQ(RtosIdentityQuality::Inferred, event.event.object.quality);
	EXPECT_EQ(3, static_cast<int>(event.event.payloadBytes));
	EXPECT_EQ(0x22, static_cast<int>(event.event.payload[1]));
	EXPECT_EQ(RtosEvidenceQuality::Good, event.event.quality);

	EXPECT_EQ(event.event.subject.rawHandle, event.subject.rawHandle);
	EXPECT_EQ(7u, event.subject.epoch);
	EXPECT_EQ(0x20002000u, event.object.rawHandle);

	EXPECT_TRUE(event.subjectGenerationUnknown);
	EXPECT_TRUE(event.afterGap);
	EXPECT_TRUE(event.hasDisplaySeconds);
	EXPECT_NEAR(3.25104, event.displaySeconds, 1e-9);
}

TEST(RtosTraceArchiveTest, AnEventWithoutATimeKeepsIt)
{
	/* "No time" is not zero: an event whose clock was absent must come back
	   absent, or it would sit at the start of the axis and shorten every
	   interval that spans it. */
	std::vector<RtosNormalizedEvent> written;
	written.push_back(makeEvent(0, RtosTraceEventType::Signal, 0x20001100u, 0x20002000u));

	written[0].hasDisplaySeconds = false;
	written[0].displaySeconds = 0.0;
	written[0].event.hasTargetTicks = false;

	std::vector<RtosNormalizedEvent> read;

	ASSERT_TRUE(RtosTraceArchive::decodeEvents(RtosTraceArchive::encodeEvents(written), read));
	ASSERT_EQ(1u, read.size());
	EXPECT_FALSE(read[0].hasDisplaySeconds);
	EXPECT_FALSE(read[0].event.hasTargetTicks);
}

TEST(RtosTraceArchiveTest, AShortPayloadIsRefusedWhole)
{
	std::vector<RtosNormalizedEvent> written;
	written.push_back(makeEvent(10, RtosTraceEventType::TaskSwitchIn, 0x20001100u, 0));

	std::vector<uint8_t> payload = RtosTraceArchive::encodeEvents(written);

	payload.resize(payload.size() - 4);

	std::vector<RtosNormalizedEvent> read;
	read.push_back(makeEvent(0, RtosTraceEventType::Unknown, 0, 0));

	EXPECT_FALSE(RtosTraceArchive::decodeEvents(payload, read));

	/* Nothing half decoded is left behind: a caller that ignored the answer
	   would otherwise draw events that never happened. */
	EXPECT_TRUE(read.empty());
}

TEST(RtosTraceArchiveTest, ACountThatCannotFitIsRefusedBeforeAllocating)
{
	std::vector<uint8_t> payload = {0xFF, 0xFF, 0xFF, 0xFF};

	std::vector<RtosNormalizedEvent> read;

	EXPECT_FALSE(RtosTraceArchive::decodeEvents(payload, read));
	EXPECT_TRUE(read.empty());
}

TEST(RtosTraceArchiveTest, TheDictionaryComesBackWithItsNames)
{
	RtosIdentityStore written;

	RtosIdentityStore::Entity task;
	task.key = RtosEntityKey{7, RtosEntityKind::Task, 0x20001100u, 2};
	task.name = "worker";
	task.nameFromDictionary = true;
	task.firstSeenTicks = 1234;
	task.closed = true;

	RtosIdentityStore::Entity unexplained;
	unexplained.key = RtosEntityKey{7, RtosEntityKind::Task, 0x20001200u, 0};
	unexplained.generationUnknown = true;
	unexplained.afterGap = true;

	written.restore(task);
	written.restore(unexplained);

	RtosIdentityStore read;

	ASSERT_TRUE(RtosTraceArchive::decodeIdentities(RtosTraceArchive::encodeIdentities(written), read));
	ASSERT_EQ(2u, read.entityCount());

	const RtosIdentityStore::Entity* found = read.find(task.key);

	ASSERT_NE(nullptr, found);
	EXPECT_EQ("worker", found->name);
	EXPECT_TRUE(found->nameFromDictionary);
	EXPECT_EQ(1234u, found->firstSeenTicks);
	EXPECT_TRUE(found->closed);

	/* A closed lifetime must not come back open: the reconciler asks which
	   lifetime of a handle is open, and a resurrected one would join two
	   histories that never met. */
	EXPECT_EQ(nullptr, read.openEntityFor(RtosEntityKind::Task, 0x20001100u));

	const RtosIdentityStore::Entity* unknown = read.find(unexplained.key);

	ASSERT_NE(nullptr, unknown);
	EXPECT_TRUE(unknown->generationUnknown);
	EXPECT_TRUE(unknown->afterGap);
	EXPECT_FALSE(unknown->closed);
}

TEST(RtosTraceArchiveTest, TheSourceComesBack)
{
	RtosTraceSourceRecord written;

	written.name = "rtec2";
	written.capabilities.set(0, true);
	written.capabilities.set(6, true);

	RtosTraceSourceRecord read;

	ASSERT_TRUE(RtosTraceArchive::decodeSource(RtosTraceArchive::encodeSource(written), read));
	EXPECT_EQ("rtec2", read.name);
	EXPECT_TRUE(read.capabilities.realHandles());
	EXPECT_TRUE(read.capabilities.isrEvents());
	EXPECT_FALSE(read.capabilities.faultEvents());
}

TEST_F(ArchiveFile, AVersionThreeFileCarriesItsEventsAndItsDictionary)
{
	/* The end to end case: a live session writes rounds, events and a
	   dictionary, and opening the file gives all three back. Before this, a
	   version 2 file held the rounds and the runs, so the bands, the budget and
	   the fault list came back empty. */
	RtosRecording recording;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	const std::string file = path("session.vtrec");

	ASSERT_TRUE(recording.begin(file, problem, RtosRecording::Format::V3)) << RtosRecording::describe(problem);

	std::vector<RtosEventRing::Interval> runs;

	RtosEventRing::Interval run;
	run.tcbAddress = 0x20001100u;
	run.fromSeconds = 0.10;
	run.toSeconds = 0.30;
	runs.push_back(run);

	recording.write(makeRound(1.0), runs);

	std::vector<RtosNormalizedEvent> events;
	events.push_back(makeEvent(1000, RtosTraceEventType::TaskSwitchIn, 0x20001100u, 0));
	events.push_back(makeEvent(1200, RtosTraceEventType::IsrEnter, 0, 0));

	RtosTraceSourceRecord source;
	source.name = "rtec2";
	source.capabilities.set(0, true);
	source.capabilities.set(6, true);

	recording.writeTraceEvents(events, source);

	RtosIdentityStore dictionary;

	RtosIdentityStore::Entity worker;
	worker.key = RtosEntityKey{7, RtosEntityKind::Task, 0x20001100u, 2};
	worker.name = "worker";

	dictionary.restore(worker);
	recording.writeIdentities(dictionary);

	recording.finish();

	RtosRecording playback;

	ASSERT_TRUE(playback.open(file, problem)) << RtosRecording::describe(problem);
	EXPECT_TRUE(playback.hasTraceEvents());
	EXPECT_EQ(1u, playback.roundCount());

	const RtosRecording::TraceContent content = playback.traceContent();

	ASSERT_TRUE(content.present);
	ASSERT_EQ(2u, content.events.size());
	EXPECT_EQ(RtosTraceEventType::TaskSwitchIn, content.events[0].event.type);
	EXPECT_EQ("rtec2", content.source.name);
	EXPECT_TRUE(content.source.capabilities.isrEvents());

	const RtosIdentityStore::Entity* found = content.identities.find(worker.key);

	ASSERT_NE(nullptr, found);
	EXPECT_EQ("worker", found->name);
}

TEST_F(ArchiveFile, AVersionTwoFileSaysItHasNoEvents)
{
	/* Not "the target did nothing": a version 2 file never held the events, and
	   the interface has to tell the two apart. */
	RtosRecording recording;
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	const std::string file = path("legacy.vtrec");

	ASSERT_TRUE(recording.begin(file, problem, RtosRecording::Format::V2)) << RtosRecording::describe(problem);
	recording.write(makeRound(1.0));
	recording.finish();

	RtosRecording playback;

	ASSERT_TRUE(playback.open(file, problem)) << RtosRecording::describe(problem);
	EXPECT_FALSE(playback.hasTraceEvents());
	EXPECT_FALSE(playback.traceContent().present);
}

TEST(RtosTracePipelineTest, ARecordingPutBackDrawsTheSameBandsAsTheLiveSession)
{
	/* The point of writing the events down: the projection built from the file
	   is the one the live session had. Compared through the same reducer the
	   panel uses, so a field lost on the way in shows up as a different band
	   rather than as a test that only counts. */
	std::vector<RtosNormalizedEvent> live;

	RtosNormalizedEvent switchedIn = makeEvent(1000, RtosTraceEventType::TaskSwitchIn, 0x20001100u, 0);
	RtosNormalizedEvent isrIn = makeEvent(1100, RtosTraceEventType::IsrEnter, 0, 0);
	RtosNormalizedEvent isrOut = makeEvent(1200, RtosTraceEventType::IsrExit, 0, 0);

	isrIn.event.payloadBytes = 0;
	isrOut.event.payloadBytes = 0;

	live.push_back(switchedIn);
	live.push_back(isrIn);
	live.push_back(isrOut);

	RtosIdentityStore identities;

	RtosIdentityStore::Entity worker;
	worker.key = RtosEntityKey{7, RtosEntityKind::Task, 0x20001100u, 2};
	worker.name = "worker";

	identities.restore(worker);

	const RtosTrackProjection fromLive = RtosTraceReducer::project(live);

	std::vector<RtosNormalizedEvent> decoded;

	ASSERT_TRUE(RtosTraceArchive::decodeEvents(RtosTraceArchive::encodeEvents(live), decoded));

	RtosTraceSourceRecord source;
	source.name = "rtec2";

	RtosTracePipeline pipeline(nullptr);
	pipeline.restore(decoded, identities, source);

	EXPECT_TRUE(pipeline.attached());
	EXPECT_TRUE(pipeline.isReplay());
	EXPECT_EQ("rtec2", std::string(pipeline.sourceName()));
	EXPECT_EQ(live.size(), pipeline.eventCount());

	const RtosTrackProjection fromFile = RtosTraceReducer::project(pipeline.events());

	ASSERT_EQ(fromLive.isr.size(), fromFile.isr.size());
	ASSERT_EQ(fromLive.task.size(), fromFile.task.size());

	for (size_t index = 0; index < fromLive.isr.size(); index++)
	{
		EXPECT_EQ(fromLive.isr[index].fromSeconds, fromFile.isr[index].fromSeconds);
		EXPECT_EQ(fromLive.isr[index].toSeconds, fromFile.isr[index].toSeconds);
		EXPECT_EQ(fromLive.isr[index].open, fromFile.isr[index].open);
		EXPECT_EQ(fromLive.isr[index].cfsr, fromFile.isr[index].cfsr);
	}

	for (size_t index = 0; index < fromLive.task.size(); index++)
	{
		EXPECT_EQ(fromLive.task[index].fromSeconds, fromFile.task[index].fromSeconds);
		EXPECT_EQ(fromLive.task[index].name, fromFile.task[index].name);
	}

	/* The bands are made from events whose keys carry names; a key that came
	   back without its lifetime would draw an empty label rather than "worker". */
	const RtosIdentityStore::Entity* named = pipeline.identity().find(RtosEntityKey{7, RtosEntityKind::Task, 0x20001100u, 2});

	ASSERT_NE(nullptr, named);
	EXPECT_EQ("worker", named->name);

	/* The live pipeline is empty, so a test that passed by reading the live one
	   would be reading nothing - the count above is the guard. */
	EXPECT_GT(pipeline.eventCount(), 0u);
}
