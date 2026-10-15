#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "IDebugProbe.hpp"

/* A controllable probe for deterministic acquisition and performance tests.
   Wall-clock assertions are deliberately left to opt-in benchmarks; ordinary
   tests use its counters to verify transactions and transferred bytes. */
class PerformanceDebugProbe : public IDebugProbe
{
   public:
	struct Timing
	{
		std::chrono::microseconds fixedPerCall{0};
		std::chrono::microseconds perByte{0};
	};

	bool startAcqusition(const DebugProbeSettings&, std::vector<std::pair<uint32_t, uint8_t>>& addresses, uint32_t) override
	{
		startedWith = addresses;
		attachments.fetch_add(1, std::memory_order_relaxed);
		isRunning = true;
		return true;
	}

	bool stopAcqusition() override
	{
		detachments.fetch_add(1, std::memory_order_relaxed);
		isRunning = false;
		return true;
	}

	bool isValid() const override { return isRunning; }
	std::string getTargetName() override { return "performance-stub"; }
	std::optional<varEntryType> readSingleEntry() override { return std::nullopt; }

	bool readMemory(uint32_t address, uint8_t* buf, uint32_t size) override
	{
		return performRead(address, buf, size, memoryCalls);
	}

	bool readBlock(uint32_t address, uint8_t* buf, uint32_t size) override
	{
		if (size > maxBlockSize)
		{
			blockCalls.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		return performRead(address, buf, size, blockCalls);
	}

	bool writeMemory(uint32_t, uint8_t*, uint32_t) override
	{
		writeCalls.fetch_add(1, std::memory_order_relaxed);
		return isRunning;
	}

	std::string getLastErrorMsg() const override { return lastErrorMsg; }
	std::vector<std::string> getConnectedDevices() override { return {}; }

	Timing timing{};
	uint32_t maxBlockSize = std::numeric_limits<uint32_t>::max();
	std::optional<uint32_t> failingAddress;
	std::vector<std::pair<uint32_t, uint8_t>> startedWith;
	std::atomic<uint64_t> attachments{0};
	std::atomic<uint64_t> detachments{0};
	std::atomic<uint64_t> memoryCalls{0};
	std::atomic<uint64_t> blockCalls{0};
	std::atomic<uint64_t> writeCalls{0};
	std::atomic<uint64_t> bytesRead{0};

   private:
	bool performRead(uint32_t address, uint8_t* buf, uint32_t size, std::atomic<uint64_t>& counter)
	{
		counter.fetch_add(1, std::memory_order_relaxed);
		std::this_thread::sleep_for(timing.fixedPerCall + timing.perByte * size);

		if (!isRunning || (failingAddress.has_value() && *failingAddress == address))
		{
			lastErrorMsg = "configured read failure";
			return false;
		}

		const uint64_t sample = sequence.fetch_add(1, std::memory_order_relaxed);
		for (uint32_t index = 0; index < size; index++)
			buf[index] = static_cast<uint8_t>((address + index + sample) & 0xff);
		bytesRead.fetch_add(size, std::memory_order_relaxed);
		return true;
	}

	std::atomic<uint64_t> sequence{0};
};
