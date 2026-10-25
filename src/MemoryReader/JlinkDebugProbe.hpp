#ifndef _JlinkDebugProbe_HPP
#define _JlinkDebugProbe_HPP

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "IDebugProbe.hpp"
#include "jlink.h"
#include "spdlog/spdlog.h"

class JlinkDebugProbe : public IDebugProbe
{
   public:
	JlinkDebugProbe(spdlog::logger* logger);
	bool startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>& addressSizeVector, uint32_t samplingFreqency) override;
	bool stopAcqusition() override;
	bool isValid() const override;

	bool startRtt(uint32_t controlBlock) override;


	uint32_t readRtt(uint8_t* buffer, uint32_t size) override;

	/* SWO on the connection the acquisition already holds.
	 *
	 * The alternative - a connection of its own - never works: a J-Link serves
	 * one at a time, and the acquisition asked first. Reading SWO and reading
	 * memory are different requests on the same link, which is why this belongs
	 * here and not beside the trace view's reader. */
	void requestSwo(uint32_t clockAddress, uint32_t fallbackCoreHz, uint32_t prescaler) override;
	bool isSwoRunning() const override
	{
		return swoStarted;
	}

	bool startSwo(uint32_t coreFrequency, uint32_t traceFrequency, uint32_t stimulusMask) override;
	uint32_t readSwo(uint8_t* buffer, uint32_t size) override;
	std::string getTargetName() override;

	std::optional<IDebugProbe::varEntryType> readSingleEntry() override;

	bool readMemory(uint32_t address, uint8_t* buf, uint32_t size) override;
	bool writeMemory(uint32_t address, uint8_t* buf, uint32_t size) override;
	bool readBlock(uint32_t address, uint8_t* buf, uint32_t size) override;

	std::string getLastErrorMsg() const override;
	std::vector<std::string> getConnectedDevices() override;

   private:
	void invalidateLink(const char* operation);

	static constexpr size_t maxDevices = 10;
	static constexpr size_t maxVariables = 100;
	static constexpr size_t fifoSize = 2000;
	static constexpr uint32_t maxSpeedkHz = 50000;
	static constexpr double timestampResolution = 1e-6;
	static constexpr uint32_t bufferSizeHSS = 16384;

	JLINK_HSS_MEM_BLOCK_DESC variableDesc[maxVariables]{};
	size_t trackedVarsCount = 0;
	size_t trackedVarsTotalSize = 0;

	size_t emptyMessageErrorThreshold = 100000;
	size_t emptyMessageErrorCnt = 0;

	/* Taken from the settings when a run starts, and read when it ends: whether
	   the link outlives the run is the reader's choice, not the probe's. */
	bool keepConnection = false;

	/* Whether the trace unit is capturing on this connection, so that a read
	   does not ask a unit that was never started. */
	bool swoStarted = false;

	/* Asked for by the caller, started at the one moment it can be: after the
	   connect, before the memory stream. */
	bool swoWanted = false;
	uint32_t swoClockAddress = 0;
	uint32_t swoFallbackCoreHz = 0;
	uint32_t swoPrescaler = 0;

	/* Whether the link to the probe is open, and what it was opened with - the
	   settings that shape it, written down as one string. A run that keeps the
	   connection leaves it open for the next one, which then has nothing to
	   open as long as that string still matches. */
	bool linkOpen = false;

	/* Set once the target's control block has been found. A refusal is not
	   remembered: the target may configure the block after the first try. */
	bool rttStarted = false;

	/* Where the block was, so that a second attempt with a different address
	   is not answered from the first one's success. */
	uint32_t rttControlBlock = 0;


	std::string openedLink;

	std::unordered_map<uint32_t, uint8_t> addressSizeMap;
	RingBuffer<varEntryType, fifoSize> varTable;

	spdlog::logger* logger;
};

#endif
