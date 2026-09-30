#ifndef _IVARIABLEREADER_HPP
#define _IVARIABLEREADER_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RingBuffer.hpp"

class IDebugProbe
{
   public:
	enum Mode
	{
		NORMAL = 0,
		HSS = 1,
	};

	/* Index into DebugProbeSettings::debugProbe, and the order of the probe list
	   in the acquisition settings window. The values are part of the project
	   file, so an existing one keeps meaning what it meant. */
	enum Probe
	{
		Stlink = 0,
		Jlink = 1,
		Serial = 2,
	};

	typedef struct
	{
		uint32_t debugProbe = 0;
		std::string serialNumber = "";
		std::string device = "";
		Mode mode = Mode::NORMAL;
		uint32_t speedkHz = 10000;
		/* Serial probe only. The port name travels in serialNumber, which is the
		   field the settings window and the project file already carry. */
		uint32_t baudrate = 115200;

	} DebugProbeSettings;

	/* timestamp (first) and a map of <address-value> entries (second) only fo HSS mode */
	using varEntryType = std::pair<double, std::unordered_map<uint32_t, double>>;

	virtual ~IDebugProbe() = default;
	virtual bool startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>& addressSizeVector, uint32_t samplingFreqency) = 0;
	virtual bool stopAcqusition() = 0;
	virtual bool isValid() const = 0;
	virtual std::string getTargetName() = 0;

	/* only HSS mode */
	virtual std::optional<varEntryType> readSingleEntry() = 0;

	/* NORMAL mode */
	virtual bool readMemory(uint32_t address, uint8_t* buf, uint32_t size) = 0;
	virtual bool writeMemory(uint32_t address, uint8_t* buf, uint32_t size) = 0;

	/* Reads a range that is longer than the four byte value readMemory carries.
	   The default walks the range word by word, which costs one round trip per
	   word; a probe that can move a block in one transaction overrides it. The
	   recorder buffer download is what needs this. */
	virtual bool readBlock(uint32_t address, uint8_t* buf, uint32_t size)
	{
		uint32_t transferred = 0;

		while (transferred < size)
		{
			const uint32_t chunk = std::min<uint32_t>(4, size - transferred);
			uint32_t value = 0;

			if (!readMemory(address + transferred, reinterpret_cast<uint8_t*>(&value), chunk))
				return false;

			std::memcpy(buf + transferred, &value, chunk);
			transferred += chunk;
		}

		return true;
	}

	virtual std::string getLastErrorMsg() const = 0;

	virtual std::vector<std::string> getConnectedDevices() = 0;

   protected:
	std::atomic<bool> isRunning = false;
	std::string lastErrorMsg = "";
	mutable std::mutex mtx;
};

#endif