#include "JlinkDebugProbe.hpp"

#include <cstdio>

/* Exported by the J-Link DLL this build links against, but not declared in the
   SDK header it bundles - which is a subset. Picking a probe by its place in
   the list the DLL gave is the one thing the header leaves out. */
extern "C" int32_t JLINKARM_EMU_SelectByIndex(int32_t index);

/* The host interfaces as the connection info describes them: 1 is USB, 2 is a
   probe reached over the network. The header names them in a comment only. */
constexpr int32_t jlinkHostInterfaceIp = 2;

#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

JlinkDebugProbe::JlinkDebugProbe(spdlog::logger* logger) : logger(logger)
{
}

bool JlinkDebugProbe::startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>& addressSizeVector, uint32_t samplingFreqency)
{
	std::lock_guard<std::mutex> lock(mtx);
	int serialNumberInt = std::atoi(probeSettings.serialNumber.c_str());
	lastErrorMsg = "";
	isRunning = false;
	emptyMessageErrorCnt = 0;

	/* Remembered rather than decided here: it is read back when the run ends. */
	keepConnection = probeSettings.keepConnection;

	/* without a device name the J-Link rejects every connect attempt */
	if (probeSettings.device.empty())
	{
		lastErrorMsg = "Target name is empty!";
		logger->error(lastErrorMsg);
		logger->error("Open Options -> Acquisition settings while the VARIABLE VIEWER tab is active and fill 'Target name' (e.g. STM32F103C8).");
		return false;
	}

	/* The link can be left open when a run ends. In that case there is nothing
	   to open here - and re-connecting is the thing that halts the core - so the
	   link is opened only when it is not up already, or when a setting that
	   shapes it has moved since it was. What shapes it is written down as one
	   string so that a field added later cannot be forgotten here. */
	const std::string linkWanted = probeSettings.device + "|" + std::to_string(probeSettings.speedkHz) + "|" +
								   (probeSettings.connection == IDebugProbe::Connection::Network ? probeSettings.ipAddress : "usb") + "|" +
								   (probeSettings.scriptEnabled ? probeSettings.scriptPath : "");
	const bool linkUsable = linkOpen && openedLink == linkWanted;

	if (linkUsable)
	{
		logger->info("J-Link link kept from the last run, nothing to open.");
	}
	else
	{
		if (linkOpen)
			JLINKARM_Close();

		/* A probe reached over the network is looked for among the ones that
		   answer, and picked by its place in that list. Naming the address
		   straight to the selector is the other way the SDK offers and it took
		   this build down on an address that was not there; enumerating first
		   can also say how many probes did answer. A probe on USB is picked by
		   serial number when the reader gave one, and by opening the first one
		   found when they did not - asking the USB selector for the probe
		   numbered 0 is a different question, and one that has no answer on a
		   probe carrying any other number. */
		if (probeSettings.connection == IDebugProbe::Connection::Network)
		{
			JLINKARM_EMU_CONNECT_INFO reachable[maxDevices]{};
			const int32_t count = JLINKARM_EMU_GetList(jlinkHostInterfaceIp, reachable, static_cast<int32_t>(maxDevices));
			int32_t index = -1;

			for (int32_t i = 0; i < count && i < static_cast<int32_t>(maxDevices); i++)
			{
				char address[20]{};
				std::snprintf(address, sizeof(address), "%u.%u.%u.%u",
							  reachable[i].aIPAddr[0], reachable[i].aIPAddr[1], reachable[i].aIPAddr[2], reachable[i].aIPAddr[3]);

				if (probeSettings.ipAddress == address)
				{
					index = i;
					break;
				}
			}

			if (index < 0 || JLINKARM_EMU_SelectByIndex(index) < 0)
			{
				lastErrorMsg = "No J-Link at '" + probeSettings.ipAddress + "' answered (" + std::to_string(count) + " found on the network)";
				logger->error(lastErrorMsg);
				linkOpen = false;
				return false;
			}
		}
		else if (serialNumberInt != 0 && JLINKARM_EMU_SelectByUSBSN(serialNumberInt) < 0)
		{
			lastErrorMsg = "Could not connect to the selected probe";
			linkOpen = false;
			return false;
		}

		const char* error = JLINKARM_Open();

		if (error != 0)
			logger->error(error);

		linkOpen = true;
		openedLink = linkWanted;

		JLINKARM_SetSpeed(probeSettings.speedkHz > maxSpeedkHz ? maxSpeedkHz : probeSettings.speedkHz);
		logger->info("J-Link speed set to: {}", JLINKARM_GetSpeed());

		/* select interface - SWD only for now */
		JLINKARM_TIF_Select(JLINKARM_TIF_SWD);

		/* A script of the reader's own goes in before the target is named and
		   connected: it is what stands between a target that stops its watchdog
		   on debug entry and one that does not. What the J-Link answers is
		   logged whether or not it is a complaint, since the answer to a script
		   that was not found is easy to miss otherwise. */
		char acOut[256];
		acOut[0] = 0;

		if (probeSettings.scriptEnabled && !probeSettings.scriptPath.empty())
		{
			const std::string scriptCmd = "ScriptFile = " + probeSettings.scriptPath;
			JLINKARM_ExecCommand(scriptCmd.c_str(), acOut, sizeof(acOut));
			logger->info("J-Link script '{}' requested, answer: '{}'", probeSettings.scriptPath, acOut);
			acOut[0] = 0;
		}

		/* set the desired target */
		logger->info("J-Link target device string: '{}'", probeSettings.device);
		auto deviceCmd = "Device = " + probeSettings.device;
		JLINKARM_ExecCommand(deviceCmd.c_str(), acOut, sizeof(acOut));

		if (acOut[0] != 0)
			logger->error(acOut);

		/* try to connect to target */
		if (JLINKARM_Connect() < 0)
		{
			lastErrorMsg = "Could not connect to the target! Check the Target name.";
			logger->error(lastErrorMsg);
			logger->error("Check 'Target name' in Options -> Acquisition settings while the VARIABLE VIEWER tab is active. Use the base J-Link device name without the package suffix, e.g. STM32F103C8.");
			JLINKARM_Close();
			linkOpen = false;
			return isRunning;
		}
	}

	if (probeSettings.mode == IDebugProbe::Mode::NORMAL)
	{
		isRunning = true;
		return true;
	}

	if (varTable.size() > 0)
		varTable.clear();

	trackedVarsCount = 0;

	/* account for timestamp in size */
	trackedVarsTotalSize = sizeof(uint32_t);
	for (auto [address, size] : addressSizeVector)
	{
		auto& desc = variableDesc[trackedVarsCount++];
		desc.Addr = address;
		desc.NumBytes = size;
		desc.Flags = 0;
		desc.Dummy = 0;
		addressSizeMap[address] = size;
		trackedVarsTotalSize += size;
	}

	if (samplingFreqency < 1)
		samplingFreqency = 1;

	uint32_t samplePeriodUs = 1.0 / (samplingFreqency * timestampResolution);
	int32_t result = JLINK_HSS_Start(variableDesc, trackedVarsCount, samplePeriodUs, JLINK_HSS_FLAG_TIMESTAMP_US);
	/* 1M is arbitraty value that works across all sampling frequencies */
	emptyMessageErrorThreshold = 1.0 / (timestampResolution * samplingFreqency) * 1000000;

	if (result >= 0)
		isRunning = true;
	else
	{
		JLINKARM_Close();
		isRunning = false;
	}

	if (result == -1)
		logger->error("Unspecified J-Link error!");
	else if (result == -2)
		logger->error("Failed to allocate memory for one or more buffers on the debug probe side!");
	else if (result == -3)
	{
		lastErrorMsg = "Too many memory variables specified!";
		logger->error(lastErrorMsg);
	}
	else if (result == -4)
	{
		lastErrorMsg = "Target does not support HSS mode!";
		logger->error(lastErrorMsg);
	}

	return isRunning;
}

bool JlinkDebugProbe::stopAcqusition()
{
	std::lock_guard<std::mutex> lock(mtx);

	/* The link can be asked to outlive the run. Closing it halts the core on
	   some targets, and that halt is what the next run would have to undo;
	   keeping it open costs nothing else, since the connection is handed back
	   when the process ends. What the probe answers to afterwards is what
	   isValid reports, so the flag follows the link rather than the run. */
	if (!keepConnection)
	{
		JLINKARM_Close();
		isRunning = false;
		linkOpen = false;
	}

	return true;
}

bool JlinkDebugProbe::isValid() const
{
	return isRunning;
}

std::string JlinkDebugProbe::getTargetName()
{
	std::lock_guard<std::mutex> lock(mtx);
	JLINKARM_DEVICE_SELECT_INFO info;
	info.SizeOfStruct = sizeof(JLINKARM_DEVICE_SELECT_INFO);
	int32_t index = JLINKARM_DEVICE_SelectDialog(NULL, 0, &info);

	JLINKARM_DEVICE_INFO devInfo{};
	devInfo.SizeOfStruct = sizeof(JLINKARM_DEVICE_INFO);
	JLINKARM_DEVICE_GetInfo(index, &devInfo);

	return devInfo.sName ? std::string(devInfo.sName) : std::string();
}

std::optional<IDebugProbe::varEntryType> JlinkDebugProbe::readSingleEntry()
{
	std::lock_guard<std::mutex> lock(mtx);
	uint8_t rawBuffer[bufferSizeHSS]{};

	int32_t readSize = JLINK_HSS_Read(rawBuffer, sizeof(rawBuffer));

	if (readSize <= 0)
	{
		emptyMessageErrorCnt++;
		if (emptyMessageErrorCnt > emptyMessageErrorThreshold)
		{
			lastErrorMsg = "Error reading HSS data!";
			logger->error(lastErrorMsg);
			isRunning = false;
		}
		return varTable.pop();
	}

	emptyMessageErrorCnt = 0;

	for (int32_t i = 0; i < readSize; i += trackedVarsTotalSize)
	{
		varEntryType entry{};

		/* timestamp */
		entry.first = (*(uint32_t*)&rawBuffer[i]) * timestampResolution;

		int32_t k = i + 4;
		for (size_t j = 0; j < trackedVarsCount; j++)
		{
			uint32_t address = variableDesc[j].Addr;
			entry.second[address] = *(uint32_t*)&rawBuffer[k];
			k += addressSizeMap[address];
		}

		if (!varTable.push(entry))
		{
			lastErrorMsg = "HSS FIFO overflow!";
			logger->error(lastErrorMsg);
			isRunning = false;
		}
	}

	return varTable.pop();
}

bool JlinkDebugProbe::readMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);
	return (isRunning && JLINKARM_ReadMemEx(address, size, buf, 0) >= 0);
}

bool JlinkDebugProbe::writeMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);
	return (isRunning && JLINKARM_WriteMemEx(address, size, buf, 0) >= 0);
}

bool JlinkDebugProbe::readBlock(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);
	/* The J-Link moves the whole range in one call, which is what makes the
	   recorder buffer download bearable: a word at a time would be some eight
	   thousand round trips for a full buffer. */
	return (isRunning && size > 0 && JLINKARM_ReadMemEx(address, size, buf, 0) >= 0);
}

std::string JlinkDebugProbe::getLastErrorMsg() const
{
	return lastErrorMsg;
}

std::vector<std::string> JlinkDebugProbe::getConnectedDevices()
{
	std::lock_guard<std::mutex> lock(mtx);
	std::vector<std::string> deviceIDs{};

	JLINKARM_EMU_CONNECT_INFO connectInfo[maxDevices]{};
	int32_t result = JLINKARM_EMU_GetList(1, (JLINKARM_EMU_CONNECT_INFO*)connectInfo, maxDevices);

	if (result < 0)
	{
		logger->error("Error reading JLink devices list. Error code {}", result);
		return std::vector<std::string>{};
	}

	for (size_t i = 0; i < maxDevices; i++)
	{
		auto serialNumber = connectInfo[i].SerialNumber;

		if (serialNumber != 0)
		{
			logger->info("JLink serial number {}", serialNumber);
			deviceIDs.push_back(std::to_string(serialNumber));
		}
	}

	return deviceIDs;
}