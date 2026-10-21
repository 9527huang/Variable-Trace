#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "RtosModel.hpp"

namespace
{
	RtosTask makeTask(uint32_t tcbAddress, RtosTaskState state, uint32_t priority)
	{
		RtosTask task;
		task.tcbAddress = tcbAddress;
		task.stateListItem = tcbAddress + 4;
		task.state = state;
		task.rawPriority = priority;
		task.hasPriority = true;
		task.priorityRank = static_cast<int64_t>(priority);
		task.sourceList = "ready";

		return task;
	}

	RtosTaskEnumeration enumerationOf(uint32_t count)
	{
		RtosTaskEnumeration enumeration;

		for (uint32_t i = 0; i < count; i++)
			enumeration.tasks.push_back(makeTask(0x20008000u + i * 0x60u, RtosTaskState::Ready, i));

		return enumeration;
	}
}  // namespace

TEST(RtosModel, IsEmptyBeforeTheFirstPublish)
{
	RtosModel model;

	EXPECT_EQ(model.current(), nullptr);
	EXPECT_EQ(model.stats().published, 0u);
	EXPECT_EQ(model.stats().rejected, 0u);
}

TEST(RtosModel, PublishesAndReadsBack)
{
	RtosModel model;

	model.publish(RtosModel::fromEnumeration(enumerationOf(3), "s1", 10.0, 10.5, RtosConsistency::BestEffort));

	ASSERT_NE(model.current(), nullptr);
	EXPECT_EQ(model.current()->taskCount(), 3u);
	EXPECT_EQ(model.current()->sessionId, "s1");
	EXPECT_DOUBLE_EQ(model.current()->captureSpanSeconds, 0.5);
	EXPECT_EQ(model.current()->consistency, RtosConsistency::BestEffort);
	EXPECT_EQ(model.stats().published, 1u);
}

TEST(RtosModel, RefusesToPublishNothing)
{
	RtosModel model;

	model.publish(RtosModel::fromEnumeration(enumerationOf(2), "s1", 0.0, 1.0, RtosConsistency::BestEffort));

	const std::shared_ptr<const RtosSnapshot> before = model.current();

	model.publish(nullptr);

	/* What was there is still there and nothing was counted. Storing the empty
	   pointer would have shown a task table with no tasks, which reads as a
	   target that has none rather than as a round that was lost. */
	EXPECT_EQ(model.current(), before);
	EXPECT_EQ(model.stats().published, 1u);
}

TEST(RtosModel, SequenceNumbersAdvance)
{
	RtosModel model;

	model.publish(RtosModel::fromEnumeration(enumerationOf(1), "s1", 0.0, 1.0, RtosConsistency::BestEffort));
	const uint64_t first = model.current()->sequence;

	model.publish(RtosModel::fromEnumeration(enumerationOf(1), "s1", 1.0, 2.0, RtosConsistency::BestEffort));
	const uint64_t second = model.current()->sequence;

	EXPECT_LT(first, second);
	EXPECT_EQ(model.stats().published, 2u);
}

TEST(RtosModel, StampsTheSessionIntoASnapshotThatHasNone)
{
	RtosModel model;
	model.beginSession("session-42");

	/* A caller that left the session empty gets the one the model is on, so a
	   snapshot cannot travel without an identity. */
	model.publish(RtosModel::fromEnumeration(enumerationOf(1), "", 0.0, 1.0, RtosConsistency::BestEffort));

	EXPECT_EQ(model.current()->sessionId, "session-42");
	EXPECT_EQ(model.sessionId(), "session-42");
}

TEST(RtosModel, RejectedRoundsLeaveThePreviousSnapshotAlone)
{
	RtosModel model;

	model.publish(RtosModel::fromEnumeration(enumerationOf(2), "s1", 0.0, 1.0, RtosConsistency::BestEffort));

	const std::shared_ptr<const RtosSnapshot> before = model.current();

	model.recordRejectedRound("the busy list declares 5 items and 3 were found", 2.0);

	/* The picture stays a reading of one moment; what is added is the note that
	   a round went missing. */
	EXPECT_EQ(model.current(), before);
	EXPECT_EQ(model.stats().published, 1u);
	EXPECT_EQ(model.stats().rejected, 1u);
	EXPECT_EQ(model.stats().lastRejectionReason, "the busy list declares 5 items and 3 were found");
	EXPECT_DOUBLE_EQ(model.stats().lastRejectedAt, 2.0);
}

TEST(RtosModel, CountsWhatWasPublishedAndWhatWasSkipped)
{
	RtosModel model;

	for (int i = 0; i < 3; i++)
		model.publish(RtosModel::fromEnumeration(enumerationOf(1), "s1", 0.0, 1.0, RtosConsistency::BestEffort));

	model.recordRejectedRound("first reason", 1.0);
	model.recordRejectedRound("second reason", 2.0);

	const RtosModel::Stats stats = model.stats();

	EXPECT_EQ(stats.published, 3u);
	EXPECT_EQ(stats.rejected, 2u);
	EXPECT_EQ(stats.lastRejectionReason, "second reason");
}

TEST(RtosModel, BeginningASessionRestartsTheSequence)
{
	RtosModel model;

	model.beginSession("s1");
	model.publish(RtosModel::fromEnumeration(enumerationOf(1), "", 0.0, 1.0, RtosConsistency::BestEffort));
	model.publish(RtosModel::fromEnumeration(enumerationOf(1), "", 1.0, 2.0, RtosConsistency::BestEffort));

	const uint64_t beforeNewSession = model.current()->sequence;

	model.beginSession("s2");
	model.publish(RtosModel::fromEnumeration(enumerationOf(1), "", 3.0, 4.0, RtosConsistency::BestEffort));

	/* A sequence number identifies a round within a session, so the identity
	   that has to be quoted is the pair and a new session counts from one. */
	EXPECT_EQ(model.current()->sequence, 1u);
	EXPECT_EQ(model.current()->sessionId, "s2");
	EXPECT_GT(beforeNewSession, model.current()->sequence);
}

TEST(RtosModel, EveryReadSeesAWholeSnapshot)
{
	RtosModel model;

	constexpr uint32_t taskCount = 7;
	constexpr int rounds = 400;

	model.beginSession("s1");

	std::atomic<bool> stop{false};
	std::atomic<int> reads{0};
	std::atomic<int> wrong{0};

	std::thread reader([&]
	{
		while (!stop.load())
		{
			const std::shared_ptr<const RtosSnapshot> snapshot = model.current();

			if (snapshot == nullptr)
				continue;

			reads++;

			/* A reader that could see a half built round would find a task
			   count other than the one every round publishes. */
			if (snapshot->taskCount() != taskCount)
				wrong++;

			if (snapshot->sessionId != "s1")
				wrong++;
		}
	});

	for (int i = 0; i < rounds; i++)
		model.publish(RtosModel::fromEnumeration(enumerationOf(taskCount), "s1", i * 1.0, i * 1.0 + 0.5, RtosConsistency::BestEffort));

	stop.store(true);
	reader.join();

	EXPECT_GT(reads.load(), 0);
	EXPECT_EQ(wrong.load(), 0);
	EXPECT_EQ(model.stats().published, static_cast<uint64_t>(rounds));
}

TEST(RtosModel, FromEnumerationCarriesTheFieldsThrough)
{
	RtosTaskEnumeration enumeration = enumerationOf(2);
	enumeration.problems.push_back("busy list: the list declares 5 items and 3 were found");

	const std::shared_ptr<RtosSnapshot> snapshot = RtosModel::fromEnumeration(enumeration, "s9", 100.0, 100.25, RtosConsistency::Consistent);

	ASSERT_NE(snapshot, nullptr);
	EXPECT_EQ(snapshot->taskCount(), 2u);
	EXPECT_EQ(snapshot->problems.size(), 1u);
	EXPECT_EQ(snapshot->consistency, RtosConsistency::Consistent);
	EXPECT_DOUBLE_EQ(snapshot->captureStartedAt, 100.0);
	EXPECT_DOUBLE_EQ(snapshot->captureFinishedAt, 100.25);
	EXPECT_DOUBLE_EQ(snapshot->captureSpanSeconds, 0.25);
	EXPECT_EQ(snapshot->sessionId, "s9");
}

TEST(RtosSnapshot, AnswersWhetherEveryTaskWasOnAList)
{
	auto snapshot = std::make_shared<RtosSnapshot>();
	snapshot->tasks.push_back(makeTask(0x1000, RtosTaskState::Ready, 1));
	snapshot->tasks.push_back(makeTask(0x2000, RtosTaskState::Blocked, 1));

	EXPECT_TRUE(snapshot->everyTaskWasOnAList());
	EXPECT_NE(snapshot->find(0x1000), nullptr);
	EXPECT_EQ(snapshot->find(0x3000), nullptr);

	/* A task on no list is reported as unknown rather than dropped, and one of
	   those means the round did not account for the whole kernel. */
	snapshot->tasks.push_back(makeTask(0x3000, RtosTaskState::Unknown, 0));

	EXPECT_FALSE(snapshot->everyTaskWasOnAList());
}

TEST(RtosSnapshot, NamesEveryConsistencyItCanReport)
{
	EXPECT_STREQ(toString(RtosConsistency::Consistent), "consistent");
	EXPECT_STREQ(toString(RtosConsistency::BestEffort), "read while running");
}

TEST(RtosSnapshot, WarnsOnlyWhenTheRoundMovedInMorePlacesThanItCanExpect)
{
	RtosSnapshot snapshot;

	/* A round that walks eighteen lists can expect a few of them to have moved
	   while it was reading the others: that is what a running target looks
	   like, and saying so in a colour every round would leave the warning on
	   permanently, which is the same as having none. */
	snapshot.listsWalked = 18;

	EXPECT_EQ(snapshot.inconsistentPlaceLimit(), 4u);

	for (uint32_t places = 0; places <= 4; places++)
	{
		snapshot.inconsistencies.assign(places, "a place that moved");

		EXPECT_FALSE(snapshot.consistencyIsShaky()) << places;
	}

	snapshot.inconsistencies.assign(5, "a place that moved");

	EXPECT_TRUE(snapshot.consistencyIsShaky());
}

TEST(RtosSnapshot, ASmallRoundStillWarnsAboveItsFloor)
{
	RtosSnapshot snapshot;

	/* Five lists walked: the share is one, and one place moving would be an
	   ordinary round on a bigger kernel. The floor is what keeps a small round
	   from being quiet about something worth looking at. */
	snapshot.listsWalked = 5;

	EXPECT_EQ(snapshot.inconsistentPlaceLimit(), 3u);

	snapshot.inconsistencies.assign(3, "a place that moved");
	EXPECT_FALSE(snapshot.consistencyIsShaky());

	snapshot.inconsistencies.assign(4, "a place that moved");
	EXPECT_TRUE(snapshot.consistencyIsShaky());
}

TEST(RtosSnapshot, ARoundThatWalkedNothingStillHasALimit)
{
	RtosSnapshot snapshot;

	/* A round that walked no lists cannot have caught anything moving, and it
	   must not divide by a share of zero either. */
	snapshot.listsWalked = 0;

	EXPECT_EQ(snapshot.inconsistentPlaceLimit(), 3u);
	EXPECT_FALSE(snapshot.consistencyIsShaky());
}

TEST(RtosModel, EveryPublishedRoundReachesTheTimeline)
{
	RtosModel model;
	const std::string session = "session-1";

	model.beginSession(session);

	EXPECT_EQ(model.history().size(), 0u);

	RtosSnapshot first;
	first.captureStartedAt = 1.0;
	first.tasks = enumerationOf(2).tasks;

	model.publish(std::make_shared<RtosSnapshot>(first));

	RtosSnapshot second;
	second.captureStartedAt = 1.5;
	second.tasks = enumerationOf(3).tasks;

	model.publish(std::make_shared<RtosSnapshot>(second));

	/* The timeline is what was published, in order, and only that: a round the
	   model refused never reaches the interface and so is not in it. */
	EXPECT_EQ(model.history().size(), 2u);
	EXPECT_DOUBLE_EQ(model.history().oldestSeconds(), 1.0);
	EXPECT_DOUBLE_EQ(model.history().newestSeconds(), 1.5);

	std::vector<RtosTimeline::Sample> samples;
	model.history().copyRange(0.0, 10.0, samples);

	ASSERT_EQ(samples.size(), 2u);
	EXPECT_EQ(samples[0].entries.size(), 2u);
	EXPECT_EQ(samples[1].entries.size(), 3u);

	model.publish(nullptr);
	EXPECT_EQ(model.history().size(), 2u);

	/* A new session is a new history. */
	model.beginSession("session-2");
	EXPECT_EQ(model.history().size(), 0u);
}

namespace
{
	/* A recording needs a file, and a test needs a place to put one. */
	class RtosModelRecordingTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			directory = std::filesystem::temp_directory_path() / "variable-trace-model-recording-test";
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

	/* The rounds a session publishes, the way the sampler does it. */
	void publishRound(RtosModel& model, double atSeconds, uint32_t tasks)
	{
		auto snapshot = std::make_shared<RtosSnapshot>();
		snapshot->captureStartedAt = atSeconds;
		snapshot->tasks = enumerationOf(tasks).tasks;

		model.publish(std::move(snapshot));
	}
}  // namespace

TEST_F(RtosModelRecordingTest, WhatWasRecordedIsWhatIsPlayedBack)
{
	RtosModel model;
	const std::string file = path("session.vtrec");
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	model.beginSession("session-1");

	ASSERT_TRUE(model.startRecording(file, problem)) << RtosRecording::describe(problem);
	EXPECT_TRUE(model.isRecording());

	publishRound(model, 1.0, 2);
	publishRound(model, 1.5, 3);
	publishRound(model, 2.0, 3);

	model.stopRecording();

	EXPECT_FALSE(model.isRecording());

	const RtosRecording::Info written = model.recordingStats();
	EXPECT_EQ(written.rounds, 3u);

	/* Playing it back is a different model: the live history is gone, and what
	   is in its place is the file. */
	RtosModel replay;

	ASSERT_TRUE(replay.openRecording(file, problem)) << RtosRecording::describe(problem);
	EXPECT_TRUE(replay.isPlayback());

	EXPECT_EQ(replay.history().size(), 3u);
	EXPECT_DOUBLE_EQ(replay.history().oldestSeconds(), 1.0);
	EXPECT_DOUBLE_EQ(replay.history().newestSeconds(), 2.0);

	const RtosModel::PlaybackInfo info = replay.playbackInfo();

	EXPECT_EQ(info.rounds, 3u);
	EXPECT_EQ(info.current, 2u);
	EXPECT_DOUBLE_EQ(info.firstSecond, 1.0);
	EXPECT_DOUBLE_EQ(info.lastSecond, 2.0);

	ASSERT_NE(replay.current(), nullptr);
	EXPECT_DOUBLE_EQ(replay.current()->captureStartedAt, 2.0);
	EXPECT_EQ(replay.current()->tasks.size(), 3u);
}

TEST_F(RtosModelRecordingTest, ShowingARoundMovesWhatTheViewHas)
{
	RtosModel model;
	const std::string file = path("scrub.vtrec");
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	model.beginSession("session-1");

	ASSERT_TRUE(model.startRecording(file, problem)) << RtosRecording::describe(problem);

	publishRound(model, 1.0, 2);
	publishRound(model, 1.5, 3);
	publishRound(model, 2.0, 4);

	model.stopRecording();

	RtosModel replay;

	ASSERT_TRUE(replay.openRecording(file, problem)) << RtosRecording::describe(problem);

	replay.showPlaybackRound(0);

	ASSERT_NE(replay.current(), nullptr);
	EXPECT_DOUBLE_EQ(replay.current()->captureStartedAt, 1.0);
	EXPECT_EQ(replay.current()->tasks.size(), 2u);

	/* The history does not move with the round being looked at: the lanes are
	   the whole recording, and the round on screen is one point inside it. */
	EXPECT_EQ(replay.history().size(), 3u);

	/* A click on the strip asks for the nearest round, not for an exact one. */
	EXPECT_EQ(replay.nearestPlaybackRound(1.62), 1u);

	replay.showPlaybackRound(1);
	EXPECT_EQ(replay.current()->tasks.size(), 3u);

	/* Out of range is ignored rather than clamped: the view keeps showing what
	   it was showing. */
	replay.showPlaybackRound(99);
	EXPECT_EQ(replay.current()->tasks.size(), 3u);
}

TEST_F(RtosModelRecordingTest, StartingASessionEndsThePlayback)
{
	RtosModel model;
	const std::string file = path("session2.vtrec");
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	model.beginSession("session-1");

	ASSERT_TRUE(model.startRecording(file, problem)) << RtosRecording::describe(problem);
	publishRound(model, 1.0, 2);
	model.stopRecording();

	RtosModel replay;

	ASSERT_TRUE(replay.openRecording(file, problem)) << RtosRecording::describe(problem);
	EXPECT_TRUE(replay.isPlayback());

	/* Starting an acquisition is what this is: a new session, and the file has
	   no place in it. */
	replay.beginSession("session-2");

	EXPECT_FALSE(replay.isPlayback());
	EXPECT_EQ(replay.current(), nullptr);
	EXPECT_EQ(replay.history().size(), 0u);
}

TEST_F(RtosModelRecordingTest, ARoundThatArrivesWhilePlayingBackIsDropped)
{
	RtosModel model;
	const std::string file = path("guarded.vtrec");
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	model.beginSession("session-1");

	ASSERT_TRUE(model.startRecording(file, problem)) << RtosRecording::describe(problem);
	publishRound(model, 1.0, 2);
	model.stopRecording();

	RtosModel replay;

	ASSERT_TRUE(replay.openRecording(file, problem)) << RtosRecording::describe(problem);

	/* Two targets in one history is a picture of neither: a live round reaching
	   a view that is showing a file is refused. */
	publishRound(replay, 5.0, 3);

	EXPECT_EQ(replay.history().size(), 1u);
	EXPECT_DOUBLE_EQ(replay.current()->captureStartedAt, 1.0);
}

TEST_F(RtosModelRecordingTest, RecordingCannotStartWhileAPlaybackIsOpen)
{
	RtosModel model;
	const std::string file = path("both.vtrec");
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	model.beginSession("session-1");

	ASSERT_TRUE(model.startRecording(file, problem)) << RtosRecording::describe(problem);
	publishRound(model, 1.0, 2);
	model.stopRecording();

	RtosModel replay;

	ASSERT_TRUE(replay.openRecording(file, problem)) << RtosRecording::describe(problem);

	EXPECT_FALSE(replay.startRecording(path("other.vtrec"), problem));
	EXPECT_NE(problem, RtosRecording::Problem::None);
}

TEST_F(RtosModelRecordingTest, ANewSessionEndsTheRecording)
{
	RtosModel model;
	const std::string file = path("session3.vtrec");
	RtosRecording::Problem problem = RtosRecording::Problem::None;

	model.beginSession("session-1");

	ASSERT_TRUE(model.startRecording(file, problem)) << RtosRecording::describe(problem);
	publishRound(model, 1.0, 2);

	/* The clock starts again with a session, so rounds from both sides of one
	   would be two runs on one axis - in the file as well as on the screen. */
	model.beginSession("session-2");

	EXPECT_FALSE(model.isRecording());
	EXPECT_EQ(model.recordingStats().rounds, 1u);
}

TEST(RtosModel, TheRosterGrowsWithTheRunAndNotWithTheRound)
{
	RtosModel model;

	model.beginSession("session-1");

	/* Three rounds, the middle one short: a list walked while the kernel was
	   moving under it. */
	publishRound(model, 1.0, 3);
	publishRound(model, 1.5, 2);
	publishRound(model, 2.0, 3);

	/* Three tasks were seen, so three rows are available to the table, although
	   no single round held all three. */
	EXPECT_EQ(model.rosterSize(), 3u);

	const std::vector<RtosTaskRoster::Entry> entries = model.rosterEntries();

	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0].tcbAddress, 0x20008000u);
	EXPECT_EQ(entries[2].tcbAddress, 0x20008000u + 2 * 0x60u);

	/* A new session is a new set of addresses as far as the view is concerned. */
	model.beginSession("session-2");

	EXPECT_EQ(model.rosterSize(), 0u);
}
