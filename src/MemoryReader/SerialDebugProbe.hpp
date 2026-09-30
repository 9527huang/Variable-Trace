#ifndef _SerialDebugProbe_HPP
#define _SerialDebugProbe_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "IDebugProbe.hpp"
#include "SerialProtocol.hpp"
#include "SerialTransport.hpp"

#include "spdlog/spdlog.h"

/*
 * Reads the target memory over a plain serial line.
 *
 * There is no debug access at all here: the target firmware runs a small driver
 * that watches the line and serves read, write and buffer copy requests. That
 * makes this the only probe that works when the debug port is occupied, and the
 * only one fast enough to pull the recorder buffer out of the target.
 *
 * Two things about the design are worth knowing before reading the code.
 *
 * A bulk read answers many addresses in one transaction, and the driver carries
 * at most ten entries per frame. The viewer walks its sample list in the same
 * order on every pass, so the first request of a pass fetches the whole pass and
 * the rest are served from that answer. A request that does not continue the
 * pass is read on its own, which costs a round trip but keeps the probe correct
 * when the caller does something unexpected.
 *
 * The recorder buffer is copied with the buffer read command, which does not go
 * through the sample list at all.
 */
class SerialDebugProbe : public IDebugProbe
{
   public:
	explicit SerialDebugProbe(spdlog::logger* logger);
	/* Lets a test stand in for the port and, through it, for the target. */
	SerialDebugProbe(spdlog::logger* logger, std::unique_ptr<serial::ITransport> transport);

	bool startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>& addressSizeVector, uint32_t samplingFreqency) override;
	bool stopAcqusition() override;
	bool isValid() const override;
	std::string getTargetName() override { return std::string(); }

	std::optional<IDebugProbe::varEntryType> readSingleEntry() override;

	bool readMemory(uint32_t address, uint8_t* buf, uint32_t size) override;
	bool writeMemory(uint32_t address, uint8_t* buf, uint32_t size) override;
	bool readBlock(uint32_t address, uint8_t* buf, uint32_t size) override;

	std::string getLastErrorMsg() const override;
	std::vector<std::string> getConnectedDevices() override;

	/* Milliseconds to wait for a reply before giving up on the target. It is
	   derived from the baud rate and the length of the expected answer, with
	   this much added on top for the target to react. */
	static constexpr uint32_t replyGraceMs = 500;

   private:
	/* One entry of the sample list of the current run. */
	struct SweepEntry
	{
		uint32_t address = 0;
		uint8_t size = 0;
	};

	/* Sends a request and collects the reply it implies. Assumes the lock. */
	bool exchange(const std::vector<uint8_t>& request, serial::ResponseReader& reply);

	/* Reads a number of entries in one frame and returns the values in order.
	   Assumes the lock. */
	bool readEntries(const std::vector<serial::ReadEntry>& entries, std::vector<uint8_t>& out);

	/* Fetches the whole sample list, in as few frames as the driver allows.
	   Assumes the lock. */
	bool fetchSweep();

	uint32_t replyTimeoutMs(size_t expectedBytes) const;

	spdlog::logger* logger;
	std::unique_ptr<serial::ITransport> transport;

	uint32_t activeBaudrate = 115200;

	std::vector<SweepEntry> sweepList;
	std::vector<uint8_t> sweepData;
	std::vector<size_t> sweepValueOffset;
	size_t sweepServed = 0;
	bool sweepDataValid = false;
};

#endif
