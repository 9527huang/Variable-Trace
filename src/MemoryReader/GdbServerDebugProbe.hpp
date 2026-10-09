#ifndef _GDBSERVERDEBUGPROBE_HPP
#define _GDBSERVERDEBUGPROBE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "IDebugProbe.hpp"

#include "spdlog/spdlog.h"

/* Reads and writes target memory through a GDB server that somebody else has
   started - a vendor tool, or OpenOCD. The protocol is the one GDB speaks to
   such a server: packets of '$' <body> '#' <checksum>, where a memory read is
   "m<address>,<length>" and the answer is the bytes in hex.

   Nothing here knows anything about the target. The addresses come from the
   symbol file the same way they do for the other probes, which is why this one
   works with a probe this build has never heard of: whatever a GDB server can
   reach, this can read.

   The server halts the target when it attaches unless it is told not to, and
   that is left to whoever started it - the address here is the server's, not
   the target's. */
class GdbServerDebugProbe : public IDebugProbe
{
   public:
	GdbServerDebugProbe(spdlog::logger* logger);
	~GdbServerDebugProbe() override;

	bool startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>& addressSizeVector, uint32_t samplingFreqency) override;
	bool stopAcqusition() override;
	bool isValid() const override;
	std::string getTargetName() override { return std::string(); }

	/* The streaming mode is the J-Link's own; a GDB server is read a range at a
	   time like every other probe here. */
	std::optional<IDebugProbe::varEntryType> readSingleEntry() override { return std::nullopt; }

	bool readMemory(uint32_t address, uint8_t* buf, uint32_t size) override;
	bool writeMemory(uint32_t address, uint8_t* buf, uint32_t size) override;
	bool readBlock(uint32_t address, uint8_t* buf, uint32_t size) override;

	std::string getLastErrorMsg() const override;
	std::vector<std::string> getConnectedDevices() override { return {}; }

   private:
	/* One packet out, with the acknowledgement the server owes it. */
	bool sendPacket(const std::string& body);

	/* One packet in. The acknowledgements the server sends along the way are
	   stepped over rather than read as answers. */
	bool receivePacket(std::string& reply);

	bool writeAll(const char* data, size_t size);
	bool readAll(char* data, size_t size);
	void closeLink();

	/* The platform's socket handle, or -1. Held as an integer so that the
	   header does not have to pull in the socket headers. */
	intptr_t handle = -1;

	/* How much of a range the server takes in one answer, as it said when the
	   link was opened. */
	uint32_t packetSize = 1024;

	spdlog::logger* logger;
};

#endif
