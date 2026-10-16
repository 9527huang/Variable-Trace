#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "FreeRtosProfile.hpp"
#include "RtosSampler.hpp"

namespace
{
	/* The layout the FreeRTOS fallback table describes. The fixture builds its
	   chains with these numbers rather than with the profile's, so a change to
	   the table shows up as a failing round rather than as a test that quietly
	   follows it. */
	namespace layout
	{
		constexpr uint32_t listCount = 0;
		constexpr uint32_t listEnd = 8;
		constexpr uint32_t miniNext = 12;
		constexpr uint32_t miniPrevious = 16;

		constexpr uint32_t itemValue = 0;
		constexpr uint32_t itemNext = 4;
		constexpr uint32_t itemPrevious = 8;
		constexpr uint32_t itemOwner = 12;
		constexpr uint32_t itemContainer = 16;
		constexpr uint32_t itemSize = 20;

		constexpr uint32_t listSize = 20;
		constexpr uint32_t tcbSize = 96;
		constexpr uint32_t tcbName = 52;
		constexpr uint32_t tcbPriority = 44;
		constexpr uint32_t tcbBasePriority = 80;
		constexpr uint32_t tcbStack = 48;
		constexpr uint32_t tcbStackTop = 68;
		constexpr uint32_t tcbNumber = 72;
		constexpr uint32_t tcbEventItem = 24;

		/* Where the run time counter sits on a build with run time statistics
		   on, read off the fixture firmware's own debug information. */
		constexpr uint32_t tcbRunTimeCounter = 88;

		/* Queue_t, as the fallback layout describes it. */
		constexpr uint32_t queueSize = 72;
		constexpr uint32_t queueHead = 0;
		constexpr uint32_t queueSend = 16;
		constexpr uint32_t queueReceive = 36;
		constexpr uint32_t queueItems = 56;
		constexpr uint32_t queueLength = 60;
	}  // namespace layout

	/* Kernel globals, at the addresses the FreeRTOS build put them at. */
	constexpr uint32_t addressCurrentTcb = 0x200000A0u;
	constexpr uint32_t addressTickCount = 0x200000A4u;
	constexpr uint32_t addressSuspendedList = 0x200000ACu;
	constexpr uint32_t addressTerminationList = 0x200000C4u;
	constexpr uint32_t addressPendingReadyList = 0x200000D8u;
	constexpr uint32_t addressOverflowDelayed = 0x200000ECu;
	constexpr uint32_t addressDelayed = 0x200000F0u;
	constexpr uint32_t addressDelayedList2 = 0x200000F4u;
	constexpr uint32_t addressDelayedList1 = 0x20000108u;
	constexpr uint32_t addressReadyLists = 0x2000011Cu;

	constexpr uint32_t addressTcbA = 0x20010000u;
	constexpr uint32_t addressTcbB = 0x20010100u;

	/* A queue in the kernel's registry, and the string its name points at. */
	constexpr uint32_t addressQueueRegistry = 0x20000200u;
	constexpr uint32_t registryEntrySize = 8;
	constexpr uint32_t addressQueueA = 0x20002000u;
	constexpr uint32_t addressQueueNameA = 0x20002100u;

	class MemoryImage : public IRtosMemory
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

		void writeWord(uint32_t address, uint32_t value)
		{
			for (uint32_t i = 0; i < sizeof(uint32_t); i++)
				bytes[address + i] = static_cast<uint8_t>((value >> (8u * i)) & 0xFFu);
		}

		void writeName(uint32_t address, const std::string& name)
		{
			for (size_t i = 0; i < name.size(); i++)
				bytes[address + static_cast<uint32_t>(i)] = static_cast<uint8_t>(name[i]);
		}

		void mapBytes(uint32_t address, uint32_t size)
		{
			for (uint32_t i = 0; i < size; i++)
				bytes[address + i] = 0;
		}

		uint32_t reads = 0;
		std::map<uint32_t, uint8_t> bytes;
	};

	/* Builds an empty circular list: the sentinel points at itself. */
	void makeEmptyList(MemoryImage& memory, uint32_t listAddress)
	{
		const uint32_t end = listAddress + layout::listEnd;

		memory.writeWord(listAddress + layout::listCount, 0);
		memory.writeWord(end + 0, 0xFFFFFFFFu);
		memory.writeWord(end + 4, end);
		memory.writeWord(end + 8, end);
	}

	/* Appends one item, keeping the chain intact in both directions. */
	void appendItem(MemoryImage& memory, uint32_t listAddress, uint32_t itemAddress, uint32_t value, uint32_t owner, uint32_t nextField, uint32_t previousItem)
	{
		const uint32_t end = listAddress + layout::listEnd;

		memory.writeWord(nextField, itemAddress);
		memory.writeWord(itemAddress + layout::itemValue, value);
		memory.writeWord(itemAddress + layout::itemNext, end);
		memory.writeWord(itemAddress + layout::itemPrevious, previousItem);
		memory.writeWord(itemAddress + layout::itemOwner, owner);
		memory.writeWord(itemAddress + layout::itemContainer, listAddress);
		memory.writeWord(end + 8, itemAddress);
	}

	void writeTaskControlBlock(MemoryImage& memory, uint32_t tcbAddress, const std::string& name, uint32_t priority, uint32_t basePriority, uint32_t stackBase, uint32_t stackTop, uint32_t number)
	{
		memory.writeWord(tcbAddress + layout::tcbPriority, priority);
		memory.writeWord(tcbAddress + layout::tcbBasePriority, basePriority);
		memory.writeWord(tcbAddress + layout::tcbStack, stackBase);
		memory.writeWord(tcbAddress + layout::tcbStackTop, stackTop);
		memory.writeWord(tcbAddress + layout::tcbNumber, number);
		memory.writeName(tcbAddress + layout::tcbName, name);
	}

	IElfParser::SymbolMap kernelSymbols()
	{
		IElfParser::SymbolMap symbols;

		auto add = [&symbols](const std::string& name, uint32_t address)
		{
			IElfParser::Symbol symbol;
			symbol.address = address;
			symbol.type = Variable::Type::U32;
			symbols[name] = symbol;
		};

		add("pxCurrentTCB", addressCurrentTcb);
		add("pxReadyTasksLists", addressReadyLists);
		add("uxTopReadyPriority", 0x2000009Cu);
		add("pxDelayedTaskList", addressDelayed);
		add("pxOverflowDelayedTaskList", addressOverflowDelayed);
		add("xDelayedTaskList1", addressDelayedList1);
		add("xDelayedTaskList2", addressDelayedList2);
		add("xPendingReadyList", addressPendingReadyList);
		add("xSuspendedTaskList", addressSuspendedList);
		add("xTasksWaitingTermination", addressTerminationList);
		add("xTickCount", addressTickCount);
		add("uxCurrentNumberOfTasks", 0x200000A8u);
		add("xSchedulerRunning", 0x20000090u);
		add("uxSchedulerSuspended", 0x20000094u);

		return symbols;
	}

	/* One symbol, for a test that needs one the others do not. */
	void addSymbol(IElfParser::SymbolMap& symbols, const std::string& name, uint32_t address)
	{
		IElfParser::Symbol symbol;
		symbol.address = address;
		symbol.type = Variable::Type::U32;
		symbols[name] = symbol;
	}

	/* A target with one running task at priority 3 and one suspended task. */
	struct FakeTarget
	{
		FakeTarget()
		{
			/* Both control blocks are mapped in full before anything is written
			   into them. A probe handed a partly mapped range gets a failure,
			   so a block with only its interesting fields mapped would fail
			   every round for a reason the real target never gives. The state
			   item lives inside the block, so the block has to exist before the
			   chain that runs through it. */
			memory.mapBytes(addressTcbA, layout::tcbSize);
			memory.mapBytes(addressTcbB, layout::tcbSize);

			/* The ready array: five lists, all empty to start with. */
			for (uint32_t i = 0; i < 5; i++)
				makeEmptyList(memory, addressReadyLists + i * layout::listSize);

			makeEmptyList(memory, addressSuspendedList);
			makeEmptyList(memory, addressPendingReadyList);
			makeEmptyList(memory, addressTerminationList);
			makeEmptyList(memory, addressDelayedList1);
			makeEmptyList(memory, addressDelayedList2);

			/* The task at priority 3 runs, so it is on that ready list and the
			   kernel's current task pointer names it. */
			const uint32_t readyList = addressReadyLists + 3 * layout::listSize;
			appendItem(memory, readyList, addressTcbA + 4, 3, addressTcbA, readyList + layout::miniNext, readyList + layout::listEnd);
			memory.writeWord(readyList + layout::listCount, 1);
			memory.writeWord(addressCurrentTcb, addressTcbA);

			writeTaskControlBlock(memory, addressTcbA, "prod", 3, 3, 0x20020000, 0x20020200, 11);

			/* The suspended task is on the suspended list and on no other. */
			appendItem(memory, addressSuspendedList, addressTcbB + 4, 0, addressTcbB, addressSuspendedList + layout::miniNext, addressSuspendedList + layout::listEnd);
			memory.writeWord(addressSuspendedList + layout::listCount, 1);

			writeTaskControlBlock(memory, addressTcbB, "susp", 1, 1, 0x20021000, 0x20021180, 12);

			memory.writeWord(addressTickCount, 1000);
			memory.writeWord(addressDelayed, addressDelayedList1);
			memory.writeWord(addressOverflowDelayed, addressDelayedList2);
		}

		/*
		 * A queue in the registry, and one task whose event item is on its
		 * receive wait list.
		 *
		 * This is the shape a task blocked with no timeout has on a real
		 * target: the kernel keeps its state item on the suspended list, so a
		 * walk of the kernel's lists finds it and calls it suspended, and only
		 * the event item on the queue's wait list says what it is really
		 * doing. Every byte the round asks for is mapped, because a partly
		 * mapped range fails the read and the failure would be the probe's
		 * rather than the target's.
		 */
		void addQueuedWaiter(uint32_t tcbAddress)
		{
			memory.mapBytes(addressQueueA, layout::queueSize);
			memory.mapBytes(addressQueueRegistry, 8 * registryEntrySize);
			memory.mapBytes(addressQueueNameA, 32);

			memory.writeName(addressQueueNameA, "q_sensor");

			makeEmptyList(memory, addressQueueA + layout::queueSend);
			makeEmptyList(memory, addressQueueA + layout::queueReceive);

			/* Not NULL and not the object's own address: a queue, which is
			   how the kernel's own macros read that field. */
			memory.writeWord(addressQueueA + layout::queueHead, addressQueueA + 0x40);
			memory.writeWord(addressQueueA + layout::queueItems, 0);
			memory.writeWord(addressQueueA + layout::queueLength, 4);

			memory.writeWord(addressQueueRegistry + 0, addressQueueNameA);
			memory.writeWord(addressQueueRegistry + 4, addressQueueA);

			const uint32_t receiveList = addressQueueA + layout::queueReceive;

			appendItem(memory, receiveList, tcbAddress + layout::tcbEventItem, 4, tcbAddress, receiveList + layout::miniNext,
			           receiveList + layout::listEnd);
			memory.writeWord(receiveList + layout::listCount, 1);
		}

		RtosSampler::Settings settings() const
		{
			RtosSampler::Settings settings;
			settings.readyListCapacity = 5;
			settings.readControlBlocks = true;

			return settings;
		}

		MemoryImage memory;
	};

	std::shared_ptr<RtosSampler> preparedSamplerWith(const IElfParser::SymbolMap& symbols, const RtosLayoutTable& layout)
	{
		auto profile = std::make_shared<FreeRtosProfile>();
		auto sampler = std::make_shared<RtosSampler>(profile);

		sampler->prepare(symbols, layout, RtosSampler::Settings{});

		return sampler;
	}

	std::shared_ptr<RtosSampler> preparedSampler(const IElfParser::SymbolMap& symbols)
	{
		auto profile = std::make_shared<FreeRtosProfile>();

		/* The layout is handed in, so the test states which one it means to
		   exercise. Nothing measured this target, so the profile's own table
		   answers and every offset it supplies is a fallback. */
		return preparedSamplerWith(symbols, profile->getFallbackLayout());
	}
}  // namespace

TEST(RtosSampler, RefusesToPrepareWithoutTheSymbolsItNeeds)
{
	auto profile = std::make_shared<FreeRtosProfile>();
	auto sampler = std::make_shared<RtosSampler>(profile);

	IElfParser::SymbolMap symbols = kernelSymbols();
	symbols.erase("pxReadyTasksLists");

	EXPECT_FALSE(sampler->prepare(symbols, profile->getFallbackLayout(), RtosSampler::Settings{}));
	EXPECT_FALSE(sampler->isPrepared());
	EXPECT_NE(sampler->lastError().find("pxReadyTasksLists"), std::string::npos);
}

TEST(RtosSampler, BuildsOneSourcePerReadyListAndOnePerFixedList)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	ASSERT_TRUE(sampler->isPrepared());

	const auto& sources = sampler->listSources();

	/* Five ready lists plus the suspended list. The pending and termination
	   lists are empty but present, so they are in as well. The two delay lists
	   are not: those are read every round. */
	EXPECT_EQ(sources.size(), 8u);

	EXPECT_EQ(sources[0].key, "ready.0");
	EXPECT_EQ(sources[0].address, addressReadyLists);
	EXPECT_TRUE(sources[0].carriesPriority);

	EXPECT_EQ(sources[3].key, "ready.3");
	EXPECT_EQ(sources[3].address, addressReadyLists + 3 * layout::listSize);
	EXPECT_EQ(sources[3].state, RtosTaskState::Ready);
}

TEST(RtosSampler, ReadsNamesStatesAndPrioritiesOffTheTarget)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	ASSERT_TRUE(round.usable()) << (round.problems.empty() ? "" : round.problems.front());
	EXPECT_EQ(round.snapshot->taskCount(), 2u);
	/* A span computed from two times is not exactly the difference of the
	   decimal numbers written here, so it is compared with a tolerance rather
	   than to the last bit. */
	/* The capture times are stamped by the sampling loop, which owns the clock
	   they are measured on, so this test does not look at the span. */
	EXPECT_EQ(round.snapshot->consistency, RtosConsistency::BestEffort);
	EXPECT_EQ(round.snapshot->sessionId, "s1");

	const RtosTask* running = round.snapshot->find(addressTcbA);
	ASSERT_NE(running, nullptr);
	EXPECT_EQ(running->state, RtosTaskState::Running);
	EXPECT_EQ(running->name, "prod");
	EXPECT_EQ(running->rawPriority, 3u);
	EXPECT_EQ(running->basePriority, 3u);
	EXPECT_EQ(running->stackBase, 0x20020000u);
	EXPECT_EQ(running->stackTop, 0x20020200u);
	EXPECT_EQ(running->kernelTaskNumber, 11u);

	const RtosTask* suspended = round.snapshot->find(addressTcbB);
	ASSERT_NE(suspended, nullptr);
	EXPECT_EQ(suspended->state, RtosTaskState::Suspended);
	EXPECT_EQ(suspended->name, "susp");
}

TEST(RtosSampler, APriorityInheritanceIsVisibleInTheTwoPriorities)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	/* The task was created at priority 1 and is running at 3 because it holds
	   a mutex a higher priority task wants. The two numbers disagreeing is the
	   diagnostic, so both have to survive the round. */
	writeTaskControlBlock(target.memory, addressTcbB, "inv_low", 3, 1, 0x20021000, 0x20021180, 12);

	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	ASSERT_TRUE(round.usable());

	const RtosTask* task = round.snapshot->find(addressTcbB);
	ASSERT_NE(task, nullptr);
	EXPECT_EQ(task->rawPriority, 3u);
	EXPECT_EQ(task->basePriority, 1u);
	EXPECT_NE(task->rawPriority, task->basePriority);
}

TEST(RtosSampler, ACheapRoundHasNoNamesAndStillHasStates)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	RtosSampler::Settings settings = target.settings();
	settings.readControlBlocks = false;

	const RtosSampler::Round round = sampler->collect(target.memory, "s1", settings);

	ASSERT_TRUE(round.usable());
	ASSERT_EQ(round.snapshot->taskCount(), 2u);

	const RtosTask* running = round.snapshot->find(addressTcbA);

	ASSERT_NE(running, nullptr);
	EXPECT_EQ(running->state, RtosTaskState::Running);
	EXPECT_TRUE(running->name.empty());

	/* The ready list carries the priority, so that one survives even without
	   the control block. */
	EXPECT_TRUE(running->hasPriority);
	EXPECT_EQ(running->rawPriority, 3u);

	/* Nothing read the suspended task's control block, so it has no priority
	   at all rather than a zero that reads as the lowest one. */
	EXPECT_FALSE(round.snapshot->find(addressTcbB)->hasPriority);
}

TEST(RtosSampler, ARoundThatCannotReadTheGlobalsIsNotUsable)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	/* Nothing is mapped where the current task pointer lives. */
	target.memory.bytes.erase(addressCurrentTcb);

	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	EXPECT_FALSE(round.usable());
	EXPECT_FALSE(round.problems.empty());
}

TEST(RtosSampler, WritesDownWhichOffsetsWereWrittenDown)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	/* Nothing measured this target's layout, so every offset came from the
	   profile's own table. The resolution has to say so: a table built from
	   numbers that were written down is not the same as one read off the
	   target, and the interface shows the difference. */
	const RtosProfileBase::Resolution resolution = sampler->resolution();

	EXPECT_TRUE(resolution.anyFallbackOffset());
	EXPECT_FALSE(resolution.fallbackFields.empty());
	EXPECT_TRUE(resolution.complete());
}

TEST(RtosSampler, CountsTheReadsARoundSpent)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	target.memory.reads = 0;

	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	ASSERT_TRUE(round.usable());

	/* Three globals, a count and a head pointer for every list, one read per
	   item, and one per control block. The band is wide enough not to depend
	   on how many empty lists the fixture happens to carry, and narrow enough
	   that a round which started chasing pointers in a loop could not pass. */
	EXPECT_GE(target.memory.reads, 20u);
	EXPECT_LE(target.memory.reads, 40u);
}

TEST(RtosSampler, ATaskThatBlocksWithNoTimeoutIsNotCalledSuspended)
{
	FakeTarget target;

	target.addQueuedWaiter(addressTcbB);

	IElfParser::SymbolMap symbols = kernelSymbols();

	addSymbol(symbols, "xQueueRegistry", addressQueueRegistry);

	const std::shared_ptr<RtosSampler> sampler = preparedSampler(symbols);
	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	ASSERT_TRUE(round.usable()) << (round.problems.empty() ? "" : round.problems.front());
	ASSERT_EQ(round.snapshot->syncObjects.size(), 1u);
	EXPECT_EQ(round.snapshot->syncObjects[0].name, "q_sensor");

	const RtosTask* blocked = round.snapshot->find(addressTcbB);

	ASSERT_NE(blocked, nullptr);

	/* Its state item is on the suspended list, which is where the kernel puts a
	   task that blocks with no timeout at all, so the list on its own would
	   call this task suspended. The event item, on the queue's receive wait
	   list, is the only thing that says otherwise - and reading the two
	   together is what the kernel's own task state query does. */
	EXPECT_EQ(blocked->state, RtosTaskState::Blocked);
	EXPECT_TRUE(blocked->hasEventList);
	EXPECT_EQ(blocked->eventList, addressQueueA + layout::queueReceive);

	/* The state item's container is the kernel's own list, and can never be
	   the answer to what the task waits for. */
	EXPECT_EQ(blocked->waitingList, addressSuspendedList);

	/* Named from the event item's container, not from the state item's. */
	EXPECT_EQ(blocked->waitingOn, "q_sensor");

	/* A task with no event item on any list keeps the state its list gave it,
	   and nothing is named for it: an empty column there is the answer rather
	   than a field that went unread. */
	const RtosTask* running = round.snapshot->find(addressTcbA);

	ASSERT_NE(running, nullptr);
	EXPECT_TRUE(running->hasEventList);
	EXPECT_EQ(running->eventList, 0u);
	EXPECT_EQ(running->state, RtosTaskState::Running);
	EXPECT_TRUE(running->waitingOn.empty());
}

TEST(RtosSampler, ATargetBuiltWithoutRunTimeStatisticsSaysSo)
{
	FakeTarget target;
	const std::shared_ptr<RtosSampler> sampler = preparedSampler(kernelSymbols());

	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	ASSERT_TRUE(round.usable());

	/* This firmware has no run time statistics, so no counter is in the control
	   block and there is nothing to read. That is a build option rather than a
	   fault, and the round says which it is instead of reading the word beside
	   where the field would have sat. */
	EXPECT_FALSE(round.snapshot->cpuLoad.supported);

	const RtosTask* task = round.snapshot->find(addressTcbA);

	ASSERT_NE(task, nullptr);
	EXPECT_FALSE(task->hasRunTimeCounter);
}

TEST(RtosSampler, ReadsTheRunTimeCounterWhenTheBuildKeepsOne)
{
	FakeTarget target;

	/* A build with run time statistics: the control block is one word longer,
	   and the debug information describes the field. Describing it is what
	   makes the offset trustworthy, and the profile deliberately offers no
	   written down one. */
	/* The counter sits inside the control block the fixture already mapped, so
	   nothing has to be mapped again - and mapping it again would clear the
	   state item's chain, which is written into the same block. */
	target.memory.writeWord(addressTcbA + layout::tcbRunTimeCounter, 12345);
	target.memory.writeWord(addressTcbB + layout::tcbRunTimeCounter, 42);

	auto profile = std::make_shared<FreeRtosProfile>();

	RtosLayoutTable measured = profile->getFallbackLayout();
	measured.addStruct("TCB_t", layout::tcbSize, RtosLayoutTable::Origin::Measured);
	measured.addField("TCB_t", "ulRunTimeCounter", layout::tcbRunTimeCounter, RtosLayoutTable::Origin::Measured);

	const std::shared_ptr<RtosSampler> sampler = preparedSamplerWith(kernelSymbols(), measured);
	const RtosSampler::Round round = sampler->collect(target.memory, "s1", target.settings());

	ASSERT_TRUE(round.usable()) << (round.problems.empty() ? "" : round.problems.front());
	EXPECT_TRUE(round.snapshot->cpuLoad.supported);

	const RtosTask* task = round.snapshot->find(addressTcbA);

	ASSERT_NE(task, nullptr);
	ASSERT_TRUE(task->hasRunTimeCounter);
	EXPECT_EQ(task->runTimeCounter, 12345u);
}
