#include <gtest/gtest.h>

#include <cstdint>
#include <map>

#include "RtosStackScanner.hpp"

namespace
{
	constexpr uint32_t stackBase = 0x20001000u;
	constexpr uint32_t stackBytes = 512u;

	class FakeMemory : public IRtosMemory
	{
	   public:
		bool read(uint32_t address, uint8_t* buf, uint32_t size) override
		{
			reads++;
			lastSize = size;

			/* A probe handed a range it cannot serve fails the whole read
			   rather than returning the part that was there, so the fixture
			   does the same. */
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

		uint32_t reads = 0;
		uint32_t lastSize = 0;
		std::map<uint32_t, uint8_t> bytes;
	};

	RtosStackScanSettings defaultSettings()
	{
		RtosStackScanSettings settings;
		settings.chunkBytes = 64;

		return settings;
	}
}  // namespace

TEST(RtosStackScanner, StopsAtTheFirstByteTheTaskWrote)
{
	FakeMemory memory;

	/* 400 bytes still holding the pattern, then the stack the task has used. */
	memory.fill(stackBase, 400, 0xA5);
	memory.fill(stackBase + 400, stackBytes - 400, 0x11);

	const RtosStackScan scan = RtosStackScanner::scan(memory, stackBase, stackBytes, defaultSettings());

	EXPECT_EQ(scan.result, RtosStackScan::Result::Ok);
	EXPECT_TRUE(scan.fillSeen);
	EXPECT_FALSE(scan.reachedCap);
	EXPECT_EQ(scan.freeBytes, 400u);
	EXPECT_EQ(scan.usedBytes(stackBytes), 112u);

	/* The read that found the boundary asked for a chunk, not for the whole
	   stack: the bytes past the boundary are bytes the round paid for on the
	   link, and the chunk size is where that cost is traded against round
	   trips. */
	EXPECT_LE(memory.lastSize, defaultSettings().chunkBytes);
}

TEST(RtosStackScanner, AStackWrittenFromItsFirstByteHasNothingFree)
{
	FakeMemory memory;

	memory.fill(stackBase, stackBytes, 0x11);

	const RtosStackScan scan = RtosStackScanner::scan(memory, stackBase, stackBytes, defaultSettings());

	EXPECT_EQ(scan.result, RtosStackScan::Result::Ok);
	EXPECT_EQ(scan.freeBytes, 0u);

	/* No pattern was seen at all, so this could be a stack that is full or a
	   kernel that fills nothing. The scanner reports what it saw and leaves
	   the two readings to be told apart by whoever has more than one stack to
	   look at. */
	EXPECT_FALSE(scan.fillSeen);
}

TEST(RtosStackScanner, ABoundaryOnAChunkEdgeIsFoundByTheNextChunk)
{
	FakeMemory memory;

	/* The free part ends exactly where the first chunk does. Nothing in the
	   first read says so, and the second read starts on the byte the task
	   wrote: the pattern is then behind the scan rather than in front of it. */
	memory.fill(stackBase, 64, 0xA5);
	memory.fill(stackBase + 64, stackBytes - 64, 0x11);

	const RtosStackScan scan = RtosStackScanner::scan(memory, stackBase, stackBytes, defaultSettings());

	EXPECT_EQ(scan.freeBytes, 64u);
	EXPECT_TRUE(scan.fillSeen);
	EXPECT_EQ(memory.reads, 2u);
}

TEST(RtosStackScanner, StopsAtTheCapAndSaysTheAnswerIsALowerBound)
{
	FakeMemory memory;

	memory.fill(stackBase, stackBytes, 0xA5);

	RtosStackScanSettings settings = defaultSettings();
	settings.maxBytes = 128;

	const RtosStackScan scan = RtosStackScanner::scan(memory, stackBase, stackBytes, settings);

	EXPECT_EQ(scan.result, RtosStackScan::Result::Ok);
	EXPECT_TRUE(scan.reachedCap);
	EXPECT_EQ(scan.freeBytes, 128u);
	EXPECT_EQ(scan.scannedBytes, 128u);

	/* The cap is not the answer: the stack is 512 bytes and only the first
	   128 were looked at, so the used part is at most what this leaves. */
	EXPECT_EQ(scan.usedBytes(stackBytes), 384u);
}

TEST(RtosStackScanner, AStackThatWasNeverUsedReportsAllOfItFree)
{
	FakeMemory memory;

	memory.fill(stackBase, stackBytes, 0xA5);

	RtosStackScanSettings settings = defaultSettings();
	settings.maxBytes = stackBytes;

	const RtosStackScan scan = RtosStackScanner::scan(memory, stackBase, stackBytes, settings);

	EXPECT_EQ(scan.freeBytes, stackBytes);
	EXPECT_TRUE(scan.fillSeen);
	EXPECT_FALSE(scan.reachedCap);
	EXPECT_EQ(scan.usedBytes(stackBytes), 0u);
}

TEST(RtosStackScanner, AFailedReadKeepsThePartThatWasScanned)
{
	FakeMemory memory;

	/* The first chunk is there, the rest of the stack is not. */
	memory.fill(stackBase, 64, 0xA5);

	const RtosStackScan scan = RtosStackScanner::scan(memory, stackBase, stackBytes, defaultSettings());

	EXPECT_EQ(scan.result, RtosStackScan::Result::ReadingFailed);
	EXPECT_EQ(scan.freeBytes, 64u);
	EXPECT_TRUE(scan.fillSeen);
	EXPECT_EQ(memory.reads, 2u);
}

TEST(RtosStackScanner, RefusesAStackWithNoRange)
{
	FakeMemory memory;

	memory.fill(stackBase, stackBytes, 0xA5);

	EXPECT_EQ(RtosStackScanner::scan(memory, 0u, stackBytes, defaultSettings()).result, RtosStackScan::Result::NoStack);
	EXPECT_EQ(RtosStackScanner::scan(memory, stackBase, 0u, defaultSettings()).result, RtosStackScan::Result::NoStack);
	EXPECT_EQ(memory.reads, 0u);
}

TEST(RtosStackScanner, NeverAsksForMoreThanOneChunkAtATime)
{
	FakeMemory memory;

	memory.fill(stackBase, stackBytes, 0xA5);

	RtosStackScanSettings settings = defaultSettings();
	settings.chunkBytes = 4096;
	settings.maxBytes = stackBytes;

	RtosStackScanner::scan(memory, stackBase, stackBytes, settings);

	/* Clamped to what the buffer on the sampling thread can hold, so that a
	   setting cannot ask a probe for a read no probe can serve. */
	EXPECT_LE(memory.lastSize, RtosStackScanner::maxChunkBytes);
	EXPECT_GE(memory.reads, 2u);
}
