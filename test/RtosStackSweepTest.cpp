#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#include "RtosStackSweep.hpp"

namespace
{
	constexpr uint32_t firstStack = 0x20001000u;
	constexpr uint32_t stackStride = 0x100u;
	constexpr uint32_t stackSize = 128u;

	class FakeMemory : public IRtosMemory
	{
	   public:
		bool read(uint32_t address, uint8_t* buf, uint32_t size) override
		{
			reads++;

			for (uint32_t i = 0; i < size; i++)
			{
				const auto it = bytes.find(address + i);

				if (it == bytes.end())
					return false;

				buf[i] = it->second;
			}

			return true;
		}

		void fill(uint32_t from, uint32_t count, uint8_t value)
		{
			for (uint32_t i = 0; i < count; i++)
				bytes[from + i] = value;
		}

		void unmap(uint32_t from, uint32_t count)
		{
			for (uint32_t i = 0; i < count; i++)
				bytes.erase(from + i);
		}

		uint32_t reads = 0;
		std::map<uint32_t, uint8_t> bytes;
	};

	/* A control block as a round hands it over: an address and a stack range. */
	RtosTask makeTask(uint32_t index)
	{
		RtosTask task;
		task.tcbAddress = 0x20010000u + index * 0x100u;
		task.hasStack = true;
		task.stackBase = firstStack + index * stackStride;
		task.stackTop = task.stackBase + stackSize;

		return task;
	}

	/* The pattern below the used part, and something else above it. */
	void layOutStack(FakeMemory& memory, const RtosTask& task, uint32_t freeBytes, uint8_t used = 0x11)
	{
		memory.fill(task.stackBase, freeBytes, 0xA5);
		memory.fill(task.stackBase + freeBytes, stackSize - freeBytes, used);
	}

	RtosStackSweep::Settings settingsFor(uint32_t tasksPerRound)
	{
		RtosStackSweep::Settings settings;
		settings.tasksPerRound = tasksPerRound;
		settings.scan.chunkBytes = 64;
		settings.scan.maxBytes = 256;

		return settings;
	}

	RtosSnapshot snapshotOf(const std::vector<RtosTask>& tasks)
	{
		RtosSnapshot snapshot;
		snapshot.tasks = tasks;

		return snapshot;
	}
}  // namespace

TEST(RtosStackSweep, GoesRoundEveryTaskWithinEnoughRounds)
{
	FakeMemory memory;

	std::vector<RtosTask> tasks;

	for (uint32_t index = 0; index < 5; index++)
	{
		tasks.push_back(makeTask(index));
		layOutStack(memory, tasks.back(), 96u);
	}

	RtosStackSweep sweep;
	const RtosStackSweep::Settings settings = settingsFor(2);

	sweep.step(memory, tasks, settings, 0.0);

	RtosSnapshot afterOne = snapshotOf(tasks);
	sweep.applyTo(afterOne, 0.0);

	/* Two tasks a round: the sweep is deliberately slower than the round, and
	   the count is what says how far it has got. */
	EXPECT_EQ(afterOne.stackScan.tasksScanned, 2u);
	EXPECT_TRUE(afterOne.stackScan.enabled);
	EXPECT_TRUE(afterOne.stackScan.fillSeen);

	sweep.step(memory, tasks, settings, 0.5);
	sweep.step(memory, tasks, settings, 1.0);

	RtosSnapshot afterThree = snapshotOf(tasks);
	sweep.applyTo(afterThree, 1.0);

	EXPECT_EQ(afterThree.stackScan.tasksScanned, 5u);

	for (const RtosTask& task : afterThree.tasks)
	{
		EXPECT_TRUE(task.hasStackWatermark) << task.tcbAddress;
		EXPECT_EQ(task.stackFreeBytes, 96u);
		EXPECT_EQ(task.stackWatermarkStackBytes, stackSize);
	}
}

TEST(RtosStackSweep, AReadingCarriesTheMomentItWasTakenAt)
{
	FakeMemory memory;

	const std::vector<RtosTask> tasks{makeTask(0)};
	layOutStack(memory, tasks.front(), 96u);

	RtosStackSweep sweep;
	sweep.step(memory, tasks, settingsFor(1), 10.0);

	RtosSnapshot snapshot = snapshotOf(tasks);
	sweep.applyTo(snapshot, 13.5);

	ASSERT_TRUE(snapshot.tasks.front().hasStackWatermark);

	/* The age travels with the reading rather than being assumed to be one
	   round: a task that has been waiting its turn carries an older number
	   than one scanned this round, and the panel says so. */
	EXPECT_DOUBLE_EQ(snapshot.tasks.front().stackWatermarkAgeSeconds, 3.5);
	EXPECT_DOUBLE_EQ(snapshot.stackScan.oldestReadingAgeSeconds, 3.5);
}

TEST(RtosStackSweep, ClaimsNothingUntilThePatternHasBeenSeen)
{
	FakeMemory memory;

	const std::vector<RtosTask> tasks{makeTask(0), makeTask(1)};

	/* A kernel that fills nothing: every byte of every stack was written. */
	for (const RtosTask& task : tasks)
		memory.fill(task.stackBase, stackSize, 0x11);

	RtosStackSweep sweep;
	const RtosStackSweep::Settings settings = settingsFor(2);

	sweep.step(memory, tasks, settings, 0.0);

	RtosSnapshot empty = snapshotOf(tasks);
	sweep.applyTo(empty, 0.0);

	/* Readings were taken and the summary says so, but zero free bytes cannot
	   be told from a stack that is full, so nothing is claimed. */
	EXPECT_TRUE(empty.stackScan.enabled);
	EXPECT_FALSE(empty.stackScan.fillSeen);
	EXPECT_EQ(empty.stackScan.tasksScanned, 2u);

	for (const RtosTask& task : empty.tasks)
		EXPECT_FALSE(task.hasStackWatermark);

	/* The moment the pattern turns up anywhere, the numbers become claims. */
	layOutStack(memory, tasks.front(), 96u);
	sweep.step(memory, tasks, settings, 0.5);

	RtosSnapshot later = snapshotOf(tasks);
	sweep.applyTo(later, 0.5);

	EXPECT_TRUE(later.stackScan.fillSeen);
	EXPECT_TRUE(later.tasks.front().hasStackWatermark);
}

TEST(RtosStackSweep, AReadingSurvivesARoundThatDidNotListTheTask)
{
	FakeMemory memory;

	const RtosTask a = makeTask(0);
	const RtosTask b = makeTask(1);
	const RtosTask c = makeTask(2);

	layOutStack(memory, a, 96u);
	layOutStack(memory, c, 96u);

	RtosStackSweep sweep;

	sweep.step(memory, {a, b}, settingsFor(1), 0.0);

	/* A round that could not walk one of the kernel's lists leaves out every
	   task that was on it. Dropping the readings with it would empty the
	   column for a reason that has nothing to do with the stacks. */
	sweep.step(memory, {b, c}, settingsFor(1), 4.0);

	RtosSnapshot snapshot = snapshotOf({a, b, c});
	sweep.applyTo(snapshot, 4.0);

	EXPECT_EQ(snapshot.stackScan.tasksScanned, 2u);
	EXPECT_TRUE(snapshot.tasks[0].hasStackWatermark);
	EXPECT_TRUE(snapshot.tasks[2].hasStackWatermark);

	/* Kept, and old: the age is what says so. */
	EXPECT_DOUBLE_EQ(snapshot.tasks[0].stackWatermarkAgeSeconds, 4.0);
}

TEST(RtosStackSweep, AReadingIsDroppedOnceTheTaskHasBeenGoneLongEnough)
{
	FakeMemory memory;

	const RtosTask a = makeTask(0);
	const RtosTask b = makeTask(1);
	const RtosTask c = makeTask(2);

	layOutStack(memory, c, 96u);

	RtosStackSweep sweep;

	sweep.step(memory, {a, b}, settingsFor(1), 0.0);

	/* Well past the lifetime, and never listed again: that is a task that was
	   deleted rather than one a single round happened to miss. */
	sweep.step(memory, {b, c}, settingsFor(1), 61.0);

	RtosSnapshot snapshot = snapshotOf({a, b, c});
	sweep.applyTo(snapshot, 61.0);

	EXPECT_FALSE(snapshot.tasks[0].hasStackWatermark);
	EXPECT_TRUE(snapshot.tasks[2].hasStackWatermark);
}

TEST(RtosStackSweep, AControlBlockHandedOutAgainDoesNotInheritTheReading)
{
	FakeMemory memory;

	RtosTask first = makeTask(0);
	first.hasKernelTaskNumber = true;
	first.kernelTaskNumber = 11;

	layOutStack(memory, first, 96u);

	RtosStackSweep sweep;

	sweep.step(memory, {first}, settingsFor(1), 0.0);

	/* The kernel deleted that task and gave its control block to another. The
	   address cannot tell the two apart; the kernel's own task number can. */
	RtosTask second = first;
	second.kernelTaskNumber = 12;

	RtosSnapshot snapshot = snapshotOf({second});
	sweep.applyTo(snapshot, 1.0);

	EXPECT_EQ(snapshot.stackScan.tasksScanned, 0u);
	EXPECT_FALSE(snapshot.tasks[0].hasStackWatermark);
}

TEST(RtosStackSweep, PassesOverATaskWhoseStackRangeWasNotRead)
{
	FakeMemory memory;

	const std::vector<RtosTask> tasks{makeTask(0), makeTask(1), makeTask(2)};

	for (const RtosTask& task : tasks)
		layOutStack(memory, task, 96u);

	/* The middle one came from a round that read no control block, so its
	   stack range is not known and it cannot be scanned. The turn is spent on
	   a task the sweep can answer for. */
	std::vector<RtosTask> sparse = tasks;
	sparse[1].hasStack = false;

	RtosStackSweep sweep;

	sweep.step(memory, sparse, settingsFor(2), 0.0);

	RtosSnapshot snapshot = snapshotOf(sparse);
	sweep.applyTo(snapshot, 0.0);

	EXPECT_EQ(snapshot.stackScan.tasksScanned, 2u);
	EXPECT_TRUE(snapshot.tasks[0].hasStackWatermark);
	EXPECT_FALSE(snapshot.tasks[1].hasStackWatermark);
	EXPECT_TRUE(snapshot.tasks[2].hasStackWatermark);
}

TEST(RtosStackSweep, AFailedScanLeavesTheEarlierReadingInPlace)
{
	FakeMemory memory;

	const std::vector<RtosTask> tasks{makeTask(0)};
	layOutStack(memory, tasks.front(), 96u);

	RtosStackSweep sweep;

	sweep.step(memory, tasks, settingsFor(1), 1.0);

	/* The probe loses the target for a round. The reading from before is more
	   use than a gap, and the age beside it is what says how old it is. */
	memory.unmap(tasks.front().stackBase, stackSize);

	sweep.step(memory, tasks, settingsFor(1), 4.0);

	RtosSnapshot snapshot = snapshotOf(tasks);
	sweep.applyTo(snapshot, 5.0);

	ASSERT_TRUE(snapshot.tasks.front().hasStackWatermark);
	EXPECT_EQ(snapshot.tasks.front().stackFreeBytes, 96u);
	EXPECT_DOUBLE_EQ(snapshot.tasks.front().stackWatermarkAgeSeconds, 4.0);
}
