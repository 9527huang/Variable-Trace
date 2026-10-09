#include "JlinkTraceProbe.hpp"

#include <cstring>
#include <random>

#include "logging.h"

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
	   shapes it has moved since it was. */
	const bool linkUsable = linkOpen && openedDevice == probeSettings.device && openedSpeedkHz == probeSettings.speedkHz;

	if (linkUsable)
	{
		logger->info("J-Link link kept from the last run, nothing to open.");
	}
	else
	{
		if (linkOpen)
			JLINKARM_Close();

		/* Selecting by serial number is for picking one probe out of several.
		   With none given there is nothing to pick, and opening the connection
		   takes the first probe found; asking the selector for the probe
		   numbered 0 is a different question, and one that has no answer on a
		   probe carrying any other number. */
		if (serialNumberInt != 0 && JLINKARM_EMU_SelectByUSBSN(serialNumberInt) < 0)
		{
			lastErrorMsg = "Could not connect to the selected probe";
			linkOpen = false;
			return false;
		}

		const char* error = JLINKARM_Open();

		if (error != 0)
			logger->error(error);

		linkOpen = true;
		openedDevice = probeSettings.device;
		openedSpeedkHz = probeSettings.speedkHz;

		JLINKARM_SetSpeed(probeSettings.speedkHz > maxSpeedkHz ? maxSpeedkHz : probeSettings.speedkHz);
		logger->info("J-Link speed set to: {}", JLINKARM_GetSpeed());

		/* select interface - SWD only for now */
		JLINKARM_TIF_Select(JLINKARM_TIF_SWD);

		/* set the desired target */
		char acOut[256];
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