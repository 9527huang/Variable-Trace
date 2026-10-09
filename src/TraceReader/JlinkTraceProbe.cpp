#include "JlinkTraceProbe.hpp"

#include <cstdio>
#include <cstring>
#include <random>

#include "logging.h"

/* Exported by the J-Link DLL this build links against, but not declared in the
   SDK header it bundles - which is a subset. Picking a probe by its place in
   the list the DLL gave is the one thing the header leaves out. */
extern "C" int32_t JLINKARM_EMU_SelectByIndex(int32_t index);

/* The host interfaces as the connection info describes them: 1 is USB, 2 is a
   probe reached over the network. The header names them in a comment only. */
constexpr int32_t jlinkHostInterfaceIp = 2;

JlinkTraceProbe::JlinkTraceProbe(spdlog::logger* logger) : logger(logger)
{
}

bool JlinkTraceProbe::stopTrace()
{
	if (JLINKARM_SWO_Control(JLINKARM_SWO_CMD_STOP, nullptr) == -1)
	{
		logger->error("Error stopping trace!");
		return false;
	}

	/* The link can be asked to outlive the run: closing it halts the core on
	   some targets, and that halt is what the next run would have to undo. It
	   is handed back when the process ends either way. */
	if (!keepConnection)
	{
		JLINKARM_Close();
		linkOpen = false;
	}

	logger->info("Trace stopped.");
	return true;
}

bool JlinkTraceProbe::isLinkOpen() const
{
	return linkOpen;
}

bool JlinkTraceProbe::startTrace(const TraceProbeSettings& probeSettings, uint32_t coreFrequency, uint32_t tracePrescaler, uint32_t activeChannelMask, bool shouldReset)
{
	int32_t serialNumberInt = std::atoi(probeSettings.serialNumber.c_str());
	lastErrorMsg = "";

	/* Remembered rather than decided here: it is read back when the run ends. */
	keepConnection = probeSettings.keepConnection;

	/* without a device name the J-Link rejects every connect attempt */
	if (probeSettings.device.empty())
	{
		lastErrorMsg = "Target name is empty!";
		logger->error(lastErrorMsg);
		logger->error("Open Options -> Acquisition settings while the TRACE VIEWER tab is active and fill 'Target name' (e.g. STM32F103C8).");
		return false;
	}

	/* The link can be left open when a run ends. In that case there is nothing
	   to open here - and re-connecting is the thing that halts the core - so the
	   link is opened only when it is not up already, or when a setting that
	   shapes it has moved since it was. What shapes it is written down as one
	   string so that a field added later cannot be forgotten here. */
	const std::string linkWanted = probeSettings.device + "|" + std::to_string(probeSettings.speedkHz) + "|" +
								   (probeSettings.connection == Connection::Network ? probeSettings.ipAddress : "usb") + "|" +
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
		   found when they did not. */
		if (probeSettings.connection == Connection::Network)
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
		   connected, and what the J-Link answers is logged either way - the
		   answer to a script that was not found is easy to miss otherwise. */
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
			logger->error("Check 'Target name' in Options -> Acquisition settings while the TRACE VIEWER tab is active. Use the base J-Link device name without the package suffix, e.g. STM32F103C8.");
			JLINKARM_Close();
			linkOpen = false;
			return false;
		}
	}

	/* turn on relative timestamping */
	JLINKARM_SWO_Config("TSEnable=1");

	/* calculate SWO speed */
	const uint32_t traceFrequency = coreFrequency / (tracePrescaler + 1);
	int32_t result = JLINKARM_SWO_EnableTarget(coreFrequency, traceFrequency, JLINKARM_SWO_IF_UART, activeChannelMask);

	if (result == 0)
	{
		logger->info("Starting Jlink reader thread!");
		linkOpen = true;
		return true;
	}

	lastErrorMsg = "Could not enable SWO on the target! Error code " + std::to_string(result);
	logger->error(lastErrorMsg);
	JLINKARM_Close();
	linkOpen = false;
	return false;
}

int32_t JlinkTraceProbe::readTraceBuffer(uint8_t* buffer, uint32_t size)
{
	/* TODO error handling of these two functions? */
	JLINKARM_SWO_Read(buffer, 0, &size);
	JLINKARM_SWO_Control(JLINKARM_SWO_CMD_FLUSH, &size);
	return size;
}

std::string JlinkTraceProbe::getLastErrorMsg() const
{
	return lastErrorMsg;
}

std::string JlinkTraceProbe::getTargetName()
{
	JLINKARM_DEVICE_SELECT_INFO info;
	info.SizeOfStruct = sizeof(JLINKARM_DEVICE_SELECT_INFO);
	int32_t index = JLINKARM_DEVICE_SelectDialog(NULL, 0, &info);

	JLINKARM_DEVICE_INFO devInfo{};
	devInfo.SizeOfStruct = sizeof(JLINKARM_DEVICE_INFO);
	JLINKARM_DEVICE_GetInfo(index, &devInfo);

	return devInfo.sName ? std::string(devInfo.sName) : std::string();
}

std::vector<std::string> JlinkTraceProbe::getConnectedDevices()
{
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