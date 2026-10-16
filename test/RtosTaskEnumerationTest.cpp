#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "FreeRtosProfile.hpp"
#include "RtosTaskEnumerator.hpp"

namespace
{
	/* The layout the FreeRTOS fallback table describes. The tests build their
	   chains with these numbers rather than with the profile's, so that a
	   change to the table shows up as a failing walk rather than as a test that
	   quietly follows it. */
	namespace layout
	{
		constexpr uint32_t listCount = 0;
		constexpr uint32_t listEnd = 8;
		constexpr uint32_t miniNext = 12;
		constexpr uint32_t miniValue = 8;
		constexpr uint32_t miniPrevious = 16;

		constexpr uint32_t itemValue = 0;
		constexpr uint32_t itemNext = 4;
		constexpr uint32_t itemPrevious = 8;
		constexpr uint32_t itemOwner = 12;
		constexpr uint32_t itemContainer = 16;
		constexpr uint32_t itemSize = 20;
	}  // namespace layout

	/* A target memory that is only what the test put in it. Reading a byte that
	   was never written fails, which is how a probe reads an address it cannot
	   reach. */
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

		void erase(uint32_t address, uint32_t size)
		{
			for (uint32_t i = 0; i < size; i++)
				bytes.erase(address + i);
		}

		uint32_t reads = 0;
		std::map<uint32_t, uint8_t> bytes;
	};

	/* Builds a FreeRTOS shaped circular list: the sentinel lives in the head,
	   and each item is linked forward and backward. */
	class ListBuilder
	{
	   public:
		ListBuilder(MemoryImage& memory, uint32_t listAddress) : memory(memory), listAddress(listAddress)
		{
			/* Every list gets its own run of memory. Two builders sharing one
			   run would have the second write over the first, and a test that
			   means to check several lists would then be measuring one. */
			itemBase = listAddress + 0x10000u;

			memory.writeWord(listAddress + layout::listCount, 0);
			memory.writeWord(listAddress + layout::listEnd + 0, 0xFFFFFFFFu);
			memory.writeWord(listAddress + layout::listEnd + 4, senderAddress());
			memory.writeWord(listAddress + layout::listEnd + 8, senderAddress());

			nextField = listAddress + layout::miniNext;
			previousItem = senderAddress();
		}

		uint32_t senderAddress() const
		{
			return listAddress + layout::listEnd;
		}

		void append(uint32_t itemAddress, uint32_t value, uint32_t owner)
		{
			memory.writeWord(nextField, itemAddress);

			memory.writeWord(itemAddress + layout::itemValue, value);
			memory.writeWord(itemAddress + layout::itemNext, senderAddress());
			memory.writeWord(itemAddress + layout::itemPrevious, previousItem);
			memory.writeWord(itemAddress + layout::itemOwner, owner);
			memory.writeWord(itemAddress + layout::itemContainer, listAddress);

			/* The sentinel always points back at the last item. */
			memory.writeWord(listAddress + layout::miniPrevious, itemAddress);

			count++;
			memory.writeWord(listAddress + layout::listCount, count);

			nextField = itemAddress + layout::itemNext;
			previousItem = itemAddress;
		}

		/* Writes the count the list declares without adding an item, which is
		   how a walk sees a list that disagrees with itself. */
		void declareCount(uint32_t value)
		{
			memory.writeWord(listAddress + layout::listCount, value);
		}

		uint32_t itemAddressAt(uint32_t index) const
		{
			return itemBase + index * 0x20u;
		}

		uint32_t itemBase = 0;

	   private:
		MemoryImage& memory;
		uint32_t listAddress;
		uint32_t nextField = 0;
		uint32_t previousItem = 0;
		uint32_t count = 0;
	};

	RtosProfileBase::Resolution resolvedLayout()
	{
		FreeRtosProfile profile;

		return profile.resolve(IElfParser::SymbolMap{}, profile.getFallbackLayout());
	}

	constexpr uint32_t listA = 0x20001000u;
	constexpr uint32_t listB = 0x20002000u;
	constexpr uint32_t listC = 0x20003000u;

	RtosProfileBase::Resolution layoutRef()
	{
		static const RtosProfileBase::Resolution layout = resolvedLayout();

		return layout;
	}
}  // namespace

TEST(RtosListWalker, WalksAChainInOrder)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	list.append(list.itemAddressAt(0), 3, 0x20008000u);
	list.append(list.itemAddressAt(1), 3, 0x20008020u);
	list.append(list.itemAddressAt(2), 3, 0x20008040u);

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA);

	ASSERT_TRUE(walk.ok()) << RtosListWalker::describe(walk.result);
	EXPECT_FALSE(walk.countDisagrees);
	ASSERT_EQ(walk.items.size(), 3u);

	EXPECT_EQ(walk.declaredCount, 3u);
	EXPECT_EQ(walk.items[0].address, list.itemAddressAt(0));
	EXPECT_EQ(walk.items[1].address, list.itemAddressAt(1));
	EXPECT_EQ(walk.items[2].address, list.itemAddressAt(2));

	EXPECT_EQ(walk.items[1].owner, 0x20008020u);
	EXPECT_EQ(walk.items[1].value, 3u);
	EXPECT_EQ(walk.items[0].container, listA);

	/* One read for the count, one for the sentinel's forward pointer, and one
	   per item. The item read covers the whole item, so the backward pointer
	   can be checked in the same round trip. */
	EXPECT_EQ(memory.reads, 2u + 3u);
}

TEST(RtosListWalker, AnEmptyListYieldsNothing)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA);

	ASSERT_TRUE(walk.ok()) << RtosListWalker::describe(walk.result);
	EXPECT_TRUE(walk.items.empty());
	EXPECT_EQ(walk.declaredCount, 0u);
	EXPECT_FALSE(walk.countDisagrees);
}

TEST(RtosListWalker, StopsOnALoop)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	list.append(list.itemAddressAt(0), 1, 0x20008000u);
	list.append(list.itemAddressAt(1), 1, 0x20008020u);

	/* Point the last item back at the first one. The sentinel is skipped, so
	   nothing in the chain ever ends it. */
	memory.writeWord(list.itemAddressAt(1) + layout::itemNext, list.itemAddressAt(0));

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA);

	EXPECT_EQ(walk.result, RtosListWalker::Result::Loop);

	/* A loop is found by revisiting a node, not by running out of budget. */
	EXPECT_LT(memory.reads, 20u);
}

TEST(RtosListWalker, StopsWhenTheChainIsLongerThanTheLimit)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	list.append(list.itemAddressAt(0), 1, 0x20008000u);
	list.append(list.itemAddressAt(1), 1, 0x20008020u);
	list.append(list.itemAddressAt(2), 1, 0x20008040u);

	RtosWalkLimits limits;
	limits.maxItems = 2;

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA, limits);

	EXPECT_EQ(walk.result, RtosListWalker::Result::TooManyItems);
}

TEST(RtosListWalker, StopsWhenABackwardPointerLies)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	list.append(list.itemAddressAt(0), 1, 0x20008000u);
	list.append(list.itemAddressAt(1), 1, 0x20008020u);

	/* The second item claims it came from the sentinel rather than from the
	   first item, so what the walk is following is not the chain it began on. */
	memory.writeWord(list.itemAddressAt(1) + layout::itemPrevious, list.senderAddress());

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA);

	EXPECT_EQ(walk.result, RtosListWalker::Result::LinksDisagree);
}

TEST(RtosListWalker, ReportsAReadThatFailed)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	list.append(list.itemAddressAt(0), 1, 0x20008000u);
	list.append(list.itemAddressAt(1), 1, 0x20008020u);

	/* Nothing is mapped at the second item any more, so a probe would report a
	   failed read there. */
	memory.erase(list.itemAddressAt(1), layout::itemSize);

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA);

	EXPECT_EQ(walk.result, RtosListWalker::Result::ReadingFailed);
}

TEST(RtosListWalker, ReportsALayoutThatCannotAnswer)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);
	list.append(list.itemAddressAt(0), 1, 0x20008000u);

	/* A profile that resolved nothing: every offset lookup fails. */
	const RtosProfileBase::Resolution empty;

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, empty, listA);

	EXPECT_EQ(walk.result, RtosListWalker::Result::LayoutIncomplete);
	EXPECT_EQ(memory.reads, 0u);
}

TEST(RtosListWalker, ReportsACountThatDoesNotMatch)
{
	MemoryImage memory;
	ListBuilder list(memory, listA);

	list.append(list.itemAddressAt(0), 1, 0x20008000u);
	list.append(list.itemAddressAt(1), 1, 0x20008020u);

	/* The chain is intact and ends where it should, but the list claims to
	   hold more than it does. */
	list.declareCount(5);

	const RtosListWalker::Walk walk = RtosListWalker::walk(memory, layoutRef(), listA);

	ASSERT_TRUE(walk.ok()) << RtosListWalker::describe(walk.result);
	EXPECT_TRUE(walk.countDisagrees);
	EXPECT_EQ(walk.declaredCount, 5u);
	EXPECT_EQ(walk.items.size(), 2u);
}

TEST(RtosTaskEnumerator, RunningComesFromTheCurrentTaskPointer)
{
	MemoryImage memory;
	ListBuilder ready(memory, listA);

	ready.append(ready.itemAddressAt(0), 2, 0x20008000u);
	ready.append(ready.itemAddressAt(1), 2, 0x20008020u);

	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.2", listA, RtosTaskState::Ready, true, false},
	};

	/* Only the pointer decides which of the two ready tasks is running. */
	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0x20008020u, true);

	ASSERT_EQ(result.tasks.size(), 2u);
	ASSERT_NE(result.find(0x20008000u), nullptr);
	ASSERT_NE(result.find(0x20008020u), nullptr);

	EXPECT_EQ(result.find(0x20008000u)->state, RtosTaskState::Ready);
	EXPECT_EQ(result.find(0x20008020u)->state, RtosTaskState::Running);
}

TEST(RtosTaskEnumerator, EveryListGivesItsOwnState)
{
	MemoryImage memory;
	ListBuilder ready(memory, listA);
	ListBuilder delayed(memory, listB);
	ListBuilder suspended(memory, listC);

	ready.append(ready.itemAddressAt(0), 1, 0x20008000u);
	delayed.append(delayed.itemAddressAt(0), 0x0000FFFFu, 0x20008020u);
	suspended.append(suspended.itemAddressAt(0), 0, 0x20008040u);

	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.1", listA, RtosTaskState::Ready, true, false},
		{"delayed", listB, RtosTaskState::Blocked, false, true},
		{"suspended", listC, RtosTaskState::Suspended, false, false},
	};

	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0u, true);

	ASSERT_TRUE(result.complete);
	ASSERT_EQ(result.tasks.size(), 3u);

	EXPECT_EQ(result.find(0x20008000u)->state, RtosTaskState::Ready);
	EXPECT_EQ(result.find(0x20008020u)->state, RtosTaskState::Blocked);
	EXPECT_EQ(result.find(0x20008040u)->state, RtosTaskState::Suspended);

	EXPECT_EQ(result.find(0x20008000u)->sourceList, "ready.1");
	EXPECT_EQ(result.find(0x20008020u)->sourceList, "delayed");

	EXPECT_EQ(result.listsWalked, 3u);
	EXPECT_EQ(result.listsFailed, 0u);
}

TEST(RtosTaskEnumerator, PriorityComesFromTheReadyListAndNowhereElse)
{
	MemoryImage memory;
	ListBuilder ready(memory, listA);
	ListBuilder blocked(memory, listB);

	ready.append(ready.itemAddressAt(0), 3, 0x20008000u);
	blocked.append(blocked.itemAddressAt(0), 0x1234u, 0x20008020u);

	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.3", listA, RtosTaskState::Ready, true, false},
		{"delayed", listB, RtosTaskState::Blocked, false, true},
	};

	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0u, true);

	const RtosTask* running = result.find(0x20008000u);
	const RtosTask* waiting = result.find(0x20008020u);

	ASSERT_NE(running, nullptr);
	ASSERT_NE(waiting, nullptr);

	EXPECT_TRUE(running->hasPriority);
	EXPECT_EQ(running->rawPriority, 3u);

	/* The delay list carries a wake tick, not a priority. Reporting a zero
	   here would read as "lowest priority", so it is reported as absent. */
	EXPECT_FALSE(waiting->hasPriority);
	EXPECT_TRUE(waiting->hasWaitTime);
	EXPECT_EQ(waiting->waitValue, 0x1234u);
}

TEST(RtosTaskEnumerator, RanksBothDirectionsTheSameWay)
{
	/* Upward numbering: a larger number is higher, so the ranking is the
	   number itself. */
	EXPECT_GT(normalizedPriority(5, true), normalizedPriority(1, true));

	/* Downward numbering: a smaller number is higher, so the ranking is
	   flipped and the comparison still reads the same way. */
	EXPECT_GT(normalizedPriority(1, false), normalizedPriority(5, false));

	/* Zero has to be usable in both directions: an upward numbering puts it at
	   the bottom, a downward one at the top. */
	EXPECT_LT(normalizedPriority(0, true), normalizedPriority(1, true));
	EXPECT_GT(normalizedPriority(0, false), normalizedPriority(1, false));
}

TEST(RtosTaskEnumerator, OneControlBlockOnTwoListsIsReported)
{
	MemoryImage memory;
	ListBuilder ready(memory, listA);
	ListBuilder delayed(memory, listB);

	ready.append(ready.itemAddressAt(0), 2, 0x20008000u);
	delayed.append(delayed.itemAddressAt(0), 100, 0x20008000u);

	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.2", listA, RtosTaskState::Ready, true, false},
		{"delayed", listB, RtosTaskState::Blocked, false, true},
	};

	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0u, true);

	EXPECT_EQ(result.tasks.size(), 1u);
	EXPECT_EQ(result.duplicates, 1u);

	/* A control block on two lists means the round caught the target moving
	   it between them. What came back is real, so the round stays complete
	   and is published; the disagreement is recorded rather than used to
	   withhold it, and the reader decides whether it matters. */
	EXPECT_TRUE(result.complete);
	EXPECT_TRUE(result.problems.empty());
	EXPECT_FALSE(result.inconsistencies.empty());

	/* The first sighting is kept, so which list a task is on does not depend
	   on which list happened to be walked last. */
	EXPECT_EQ(result.tasks.front().sourceList, "ready.2");
}

TEST(RtosTaskEnumerator, AFailedListMakesTheRoundIncomplete)
{
	MemoryImage memory;
	ListBuilder ready(memory, listA);

	ready.append(ready.itemAddressAt(0), 1, 0x20008000u);

	/* A list whose head was never written: every read of it fails. */
	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.1", listA, RtosTaskState::Ready, true, false},
		{"suspended", 0x20009000u, RtosTaskState::Suspended, false, false},
	};

	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0u, true);

	EXPECT_EQ(result.tasks.size(), 1u);
	EXPECT_EQ(result.listsWalked, 2u);
	EXPECT_EQ(result.listsFailed, 1u);
	EXPECT_FALSE(result.complete);

	ASSERT_EQ(result.problems.size(), 1u);
	EXPECT_NE(result.problems.front().find("suspended"), std::string::npos);
}

TEST(RtosTaskEnumerator, AListThatDisagreesWithItselfIsRecordedAndStillUsed)
{
	MemoryImage memory;
	ListBuilder ready(memory, listA);

	ready.append(ready.itemAddressAt(0), 1, 0x20008000u);
	ready.declareCount(9);

	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.1", listA, RtosTaskState::Ready, true, false},
	};

	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0u, true);

	/* The items are handed over and the round stays complete. The chain ended
	   properly; it just declares a different count than it yielded, which is
	   what a change landing mid-walk looks like. The disagreement is recorded
	   so that a reader who needs one instant can tell. */
	EXPECT_EQ(result.tasks.size(), 1u);
	EXPECT_TRUE(result.complete);
	EXPECT_TRUE(result.problems.empty());
	ASSERT_EQ(result.inconsistencies.size(), 1u);
	EXPECT_NE(result.inconsistencies.front().find("declares 9"), std::string::npos);
}

TEST(RtosTaskEnumerator, AnswersWithEveryTaskExactlyOnce)
{
	MemoryImage memory;
	ListBuilder readyA(memory, listA);
	ListBuilder readyB(memory, listB);
	ListBuilder blocked(memory, listC);

	for (uint32_t i = 0; i < 3; i++)
		readyA.append(readyA.itemAddressAt(i), 2, 0x20008000u + i * 0x20u);

	readyB.append(readyB.itemAddressAt(0), 3, 0x20009000u);
	blocked.append(blocked.itemAddressAt(0), 500, 0x2000A000u);

	const std::vector<RtosTaskEnumerator::ListSource> sources = {
		{"ready.2", listA, RtosTaskState::Ready, true, false},
		{"ready.3", listB, RtosTaskState::Ready, true, false},
		{"delayed", listC, RtosTaskState::Blocked, false, true},
	};

	const RtosTaskEnumeration result = RtosTaskEnumerator::enumerate(memory, layoutRef(), sources, 0u, true);

	EXPECT_TRUE(result.complete);
	EXPECT_EQ(result.tasks.size(), 5u);
	EXPECT_EQ(result.duplicates, 0u);
	EXPECT_TRUE(result.problems.empty());

	/* Nothing is reported twice and nothing is dropped. */
	for (size_t i = 0; i < result.tasks.size(); i++)
	{
		for (size_t j = i + 1; j < result.tasks.size(); j++)
			EXPECT_NE(result.tasks[i].tcbAddress, result.tasks[j].tcbAddress);
	}
}

TEST(RtosTaskEnumerator, NamesEveryStateItCanReport)
{
	EXPECT_STREQ(toString(RtosTaskState::Running), "running");
	EXPECT_STREQ(toString(RtosTaskState::Ready), "ready");
	EXPECT_STREQ(toString(RtosTaskState::Blocked), "blocked");
	EXPECT_STREQ(toString(RtosTaskState::Suspended), "suspended");
	EXPECT_STREQ(toString(RtosTaskState::PendingReady), "pending ready");
	EXPECT_STREQ(toString(RtosTaskState::Deleted), "deleted");
	EXPECT_STREQ(toString(RtosTaskState::Unknown), "unknown");
}
