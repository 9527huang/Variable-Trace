#ifndef _JLINKTRACEPROBE_HPP
#define _JLINKTRACEPROBE_HPP

#include <memory>

#include "ITraceProbe.hpp"
#include "jlink.h"
#include "spdlog/spdlog.h"

class JlinkTraceProbe : public ITraceProbe
{
   public:
	explicit JlinkTraceProbe(spdlog::logger* logger);
	bool startTrace(const TraceProbeSettings& probeSettings, uint32_t coreFrequency, uint32_t tracePrescaler, uint32_t activeChannelMask, bool shouldReset) override;
	bool stopTrace() override;
	int32_t readTraceBuffer(uint8_t* buffer, uint32_t size) override;

	std::string getTargetName() override;
	std::vector<std::string> getConnectedDevices() override;
	std::string getLastErrorMsg() const override;
	bool isLinkOpen() const override;

   private:
	static constexpr uint32_t maxSpeedkHz = 50000;
	static constexpr size_t maxDevices = 10;

	/* Taken from the settings when a run starts and read when it ends: whether
	   the link outlives the run is the reader's choice. */
	bool keepConnection = false;

	/* Whether the link is open, and what it was opened with - the settings that
	   shape it, written down as one string. A run that keeps the connection
	   leaves it open for the next one. */
	bool linkOpen = false;
	std::string openedLink;

	spdlog::logger* logger;
};
#endif