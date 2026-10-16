#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "FreeRtosProfile.hpp"
#include "RtosSyncEnumerator.hpp"

namespace
{
	/* The same shape the task enumeration tests use: the reader only ever calls
	   read, so a map of bytes is a whole kernel as far as it is concerned. */
	class ImageMemory : public IRtosMemory
	{
	   public:
		void writeWord(uint32_t address, uint32_t value)
		{
			for (int index = 0; index < 4; ++index)
				cells[address + index] = static_cast<uint8_t>((value >> (8 * index)) & 0xFF);
		}

		void writeText(uint32_t address, const std::string& text)
		{
			for (size_t index = 0; index < text.size(); ++index)
				cells[address + index] = static_cast<uint8_t>(text[index]);

			cells[address + text.size()] = 0;
		}

		bool read(uint32_t address, uint8_t* buffer, uint32_t size) override
		{
			if (unreadable.count(address) != 0)
				return false;

			for (uint32_t index = 0; index < size; ++index)
			{
				const auto it = cells.find(address + index);

				buffer[index] = (it == cells.end()) ? 0 : it->second;
			}

			return true;
		}

		std::map<uint32_t, uint8_t> cells;
		std::map<uint32_t, int> unreadable;
	};

	constexpr uint32_t registryAddress = 0x20001000;
	constexpr uint32_t nameAddress = 0x20002000;

	/* Where the profile's own table says things are on this build. See
	   FreeRtosProfile::getFallbackLayout. */
	constexpr uint32_t queueHead = 0;
	constexpr uint32_t queueWaitingToSend = 16;
	constexpr uint32_t queueWaitingToReceive = 36;
	constexpr uint32_t queueItems = 56;
	constexpr uint32_t queueLength = 60;
	constexpr uint32_t mutexHolder = 8;
	constexpr uint32_t recursiveCount = 12;
	constexpr uint32_t listItemCount = 0;

	constexpr uint32_t slotBytes = 8;
	constexpr uint32_t registrySlots = 4;

	FreeRtosProfile& profile()
	{
		static FreeRtosProfile instance;

		return instance;
	}

	RtosProfileBase::Resolution layout()
	{
		return profile().resolve({}, profile().getFallbackLayout());
	}

	/* Files one object into a registry slot, the way the kernel's registry
	   does: a pointer to its name and a pointer to the object. */
	void registerSlot(ImageMemory& memory, uint32_t slot, uint32_t namePointer, uint32_t handle)
	{
		memory.writeWord(registryAddress + slot * slotBytes + 0, namePointer);
		memory.writeWord(registryAddress + slot * slotBytes + 4, handle);
	}

	void makeQueue(ImageMemory& memory, uint32_t address, uint32_t depth, uint32_t capacity)
	{
		memory.writeWord(address + queueHead, 0x20003000);
		memory.writeWord(address + queueItems, depth);
		memory.writeWord(address + queueLength, capacity);
	}

	void makeSemaphore(ImageMemory& memory, uint32_t address, uint32_t count)
	{
		/* A semaphore is marked by the head pointer holding the object's own
		   address. */
		memory.writeWord(address + queueHead, address);
		memory.writeWord(address + queueItems, count);
		memory.writeWord(address + queueLength, count);
	}

	void makeMutex(ImageMemory& memory, uint32_t address, uint32_t holder)
	{
		/* A mutex is marked by a null head pointer. */
		memory.writeWord(address + queueHead, 0);
		memory.writeWord(address + queueItems, holder == 0 ? 0 : 1);
		memory.writeWord(address + queueLength, 1);
		memory.writeWord(address + mutexHolder, holder);
	}
}  // namespace

TEST(RtosSyncEnumerator, TellsAQueueFromASemaphoreAndAMutex)
{
	ImageMemory memory;

	makeQueue(memory, 0x20004000, 3, 8);
	makeSemaphore(memory, 0x20004100, 1);
	makeMutex(memory, 0x20004200, 0);

	registerSlot(memory, 0, nameAddress, 0x20004000);
	registerSlot(memory, 1, nameAddress, 0x20004100);
	registerSlot(memory, 2, nameAddress, 0x20004200);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	ASSERT_EQ(found.objects.size(), 3u);

	EXPECT_EQ(found.objects[0].kind, RtosSyncKind::Queue);
	EXPECT_EQ(found.objects[0].capacity, 8u);
	EXPECT_EQ(found.objects[0].items, 3u);

	EXPECT_EQ(found.objects[1].kind, RtosSyncKind::Semaphore);
	EXPECT_EQ(found.objects[1].items, 1u);

	EXPECT_EQ(found.objects[2].kind, RtosSyncKind::Mutex);
	EXPECT_EQ(found.objects[2].capacity, 1u);

	EXPECT_TRUE(found.complete);
}

TEST(RtosSyncEnumerator, ReadsTheNameAnObjectWasRegisteredUnder)
{
	ImageMemory memory;

	makeQueue(memory, 0x20004000, 0, 4);

	memory.writeText(nameAddress, "q_sensor");
	registerSlot(memory, 0, nameAddress, 0x20004000);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	ASSERT_EQ(found.objects.size(), 1u);
	EXPECT_EQ(found.objects[0].name, "q_sensor");
}

TEST(RtosSyncEnumerator, LeavesAnUnusedSlotAlone)
{
	ImageMemory memory;

	makeQueue(memory, 0x20004000, 1, 4);
	registerSlot(memory, 0, nameAddress, 0x20004000);
	registerSlot(memory, 2, nameAddress, 0x20004200);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	/* Two objects, and the empty slots are not reported as problems: a slot
	   with no handle was never used. */
	ASSERT_EQ(found.objects.size(), 2u);
	EXPECT_TRUE(found.problems.empty());
}

TEST(RtosSyncEnumerator, CountsTheWaitersOnEachSide)
{
	ImageMemory memory;

	makeQueue(memory, 0x20004000, 4, 4);

	/* The wait lists are full and empty respectively, which is what a queue
	   with two blocked senders and no blocked receiver looks like. */
	memory.writeWord(0x20004000 + queueWaitingToSend + listItemCount, 2);
	memory.writeWord(0x20004000 + queueWaitingToReceive + listItemCount, 0);

	registerSlot(memory, 0, nameAddress, 0x20004000);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	ASSERT_EQ(found.objects.size(), 1u);
	EXPECT_TRUE(found.objects[0].hasWaiters);
	EXPECT_EQ(found.objects[0].waitingToSend, 2u);
	EXPECT_EQ(found.objects[0].waitingToReceive, 0u);
}

TEST(RtosSyncEnumerator, ReportsWhoHoldsAMutexAndHowDeeply)
{
	ImageMemory memory;

	makeMutex(memory, 0x20004200, 0x20000B48);
	memory.writeWord(0x20004200 + recursiveCount, 3);

	registerSlot(memory, 0, nameAddress, 0x20004200);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	ASSERT_EQ(found.objects.size(), 1u);
	EXPECT_TRUE(found.objects[0].hasHolder);
	EXPECT_EQ(found.objects[0].holder, 0x20000B48u);
	EXPECT_EQ(found.objects[0].recursiveCount, 3u);
}

TEST(RtosSyncEnumerator, AnUnheldMutexIsNotTheSameAsOneWithoutAHolder)
{
	ImageMemory memory;

	makeMutex(memory, 0x20004200, 0);
	registerSlot(memory, 0, nameAddress, 0x20004200);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	ASSERT_EQ(found.objects.size(), 1u);

	/* Held by nobody, which is a reading, and not an unread field. */
	EXPECT_TRUE(found.objects[0].hasHolder);
	EXPECT_EQ(found.objects[0].holder, 0u);
	EXPECT_EQ(found.objects[0].items, 0u);
}

TEST(RtosSyncEnumerator, AQueueWithNoRegistrySaysSoRatherThanAnsweringNothing)
{
	ImageMemory memory;

	const RtosSyncEnumeration found = RtosSyncEnumerator::enumerate(memory, layout(), {0, 0});

	EXPECT_FALSE(found.complete);
	EXPECT_TRUE(found.objects.empty());
	ASSERT_EQ(found.problems.size(), 1u);
	EXPECT_NE(found.problems[0].find("registry"), std::string::npos);
}

TEST(RtosSyncEnumerator, ASlotThatCouldNotBeReadMakesTheRoundIncomplete)
{
	ImageMemory memory;

	makeQueue(memory, 0x20004000, 1, 4);
	registerSlot(memory, 0, nameAddress, 0x20004000);

	memory.unreadable[registryAddress + 2 * slotBytes] = 1;

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	/* The object that could be read is still reported, and the round says it
	   is not the whole picture rather than presenting it as one. */
	EXPECT_EQ(found.objects.size(), 1u);
	EXPECT_FALSE(found.complete);
	EXPECT_EQ(found.problems.size(), 1u);
}

TEST(RtosSyncEnumerator, DoesNotPutGarbageInTheNameColumn)
{
	ImageMemory memory;

	makeQueue(memory, 0x20004000, 0, 4);

	/* A pointer that is not a string at all, which is what a stale registry
	   entry looks like. */
	memory.writeWord(nameAddress, 0xDEADBEEF);
	registerSlot(memory, 0, nameAddress, 0x20004000);

	const RtosSyncEnumeration found =
	    RtosSyncEnumerator::enumerate(memory, layout(), {registryAddress, registrySlots});

	ASSERT_EQ(found.objects.size(), 1u);
	EXPECT_TRUE(found.objects[0].name.empty());
}
