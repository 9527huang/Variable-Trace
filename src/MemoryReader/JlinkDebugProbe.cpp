#include "JlinkDebugProbe.hpp"

#include <cstdio>

/* Exported by the J-Link DLL this build links against, but not declared in the
   SDK header it bundles - which is a subset. Picking a probe by its place in
   the list the DLL gave is the one thing the header leaves out. */
extern "C" int32_t JLINKARM_EMU_SelectByIndex(int32_t index);

/*
 * The DLL would rather show a message box than return an error.
 *
 * "CPU is halted", "CPU is not halted", "Could not connect" - each arrives as a
 * modal window owned by the DLL, and a modal window owns the thread that opened
 * it. That thread is this application's interface thread, so the whole program
 * stops: the panels stop drawing, the API stops answering, and the only way out
 * is for someone to click OK on a window that says nothing about what the
 * program was doing. A probe that is busy, or a target that is running when the
 * DLL wants it stopped, is a normal thing to meet; a dialog is not the way to
 * report it.
 *
 * The hook below is the documented way to take the messages over. It is not in
 * the SDK header this build bundles, which is a subset, so it is declared here
 * next to the one above and for the same reason - and under the name the DLL
 * actually exports, which `objdump -p` on the DLL confirms.
 */
extern "C" void JLINKARM_SetErrorOutHandler(void (*errorOut)(const char* message));

/*
 * RTT, as the DLL spells it. Not in the SDK header this build bundles, so it is
 * declared here next to the other two, under the names `objdump -p` shows.
 *
 * The command numbers are SEGGER's: 1 starts the terminal and looks for the
 * control block, 2 stops it.
 */
extern "C" int32_t JLINK_RTTERMINAL_Control(uint32_t command, void* configuration);
extern "C" uint32_t JLINK_RTTERMINAL_Read(uint32_t channel, uint8_t* buffer, uint32_t size);

constexpr uint32_t jlinkRttStart = 1;

/* What the SWO start command expects. The bundled header names the command and
   points at this structure without declaring it. */
struct JlinkSwoStart
{
	uint32_t sizeOfStruct;
	uint32_t interface;
	uint32_t speed;
};

/* The four SWO calls are declared in the SDK header; the register write is not,
   so it is declared here. */
extern "C" int32_t JLINKARM_WriteU32(uint32_t address, uint32_t value);

constexpr uint32_t jlinkSwoStart = 0;    /* JLINKARM_SWO_CMD_START */
constexpr uint32_t jlinkSwoFlush = 2;    /* JLINKARM_SWO_CMD_FLUSH */
constexpr uint32_t jlinkSwoInterfaceUart = 1;

/* What the start command takes: the address of the control block, and an
   identifier of zero for "the one the address points at". A zero address makes
   the DLL search the target's RAM, which is what it did before - and found the
   block without being able to read it. */
struct JlinkRttStart
{
	uint32_t controlBlockAddress;
	uint32_t identifier;
};

namespace
{
	/* The DLL calls these from whichever thread it is running on, so the logger
	   has to be reached without an object: the probe that installed it may be
	   gone by the time a late message arrives. */
	spdlog::logger* dllLogger = nullptr;

	void errorFromDll(const char* message)
	{
		if (dllLogger == nullptr || message == nullptr)
			return;

		dllLogger->warn("J-Link refused: {}", message);
	}
}  // namespace

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

		/* Before anything is asked of the target: a failure from here on goes to
		   the log rather than to a window nobody asked for. */
		dllLogger = logger;

		JLINKARM_SetErrorOutHandler(&errorFromDll);

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

		/* Connecting can leave the core halted before firmware initialization.
		   Acquisition observes a running target, so resume it before either the
		   Normal reader or HSS starts consuming memory. */
		JLINKARM_Go();
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
	if (!isRunning)
		return false;

	if (JLINKARM_ReadMemEx(address, size, buf, 0) >= 0)
		return true;

	invalidateLink("reading target memory");
	return false;
}

bool JlinkDebugProbe::writeMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (!isRunning)
		return false;

	if (JLINKARM_WriteMemEx(address, size, buf, 0) >= 0)
		return true;

	invalidateLink("writing target memory");
	return false;
}

bool JlinkDebugProbe::startRtt(uint32_t controlBlock)
{
	if (rttStarted && rttControlBlock == controlBlock)
		return true;

	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return false;

	/* Finding the control block means searching the target's RAM, which is why
	   the answer is remembered: the search costs milliseconds and gives the same
	   answer every round. A refusal is not remembered, because the target may
	   set the block up after this first try. */
	JlinkRttStart configuration{controlBlock, 0};
	const int32_t result = JLINK_RTTERMINAL_Control(jlinkRttStart, controlBlock == 0 ? nullptr : &configuration);

	if (result < 0)
		return false;

	rttStarted = true;
	rttControlBlock = controlBlock;

	logger->info("The target keeps an RTT buffer, and the probe is reading it.");

	return true;
}

uint32_t JlinkDebugProbe::readRtt(uint8_t* buffer, uint32_t size)
{
	if (!rttStarted)
		return 0;

	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return 0;

	/* What the target wrote, and nothing waiting for it: an empty read is the
	   usual answer between two switches. */
	return JLINK_RTTERMINAL_Read(0, buffer, size);
}

/*
 * The trace unit, configured the way the trace view configures it.
 *
 * The two are the same hardware: the same ITM, the same clock, the same pin. The
 * sequence here is the trace view's, and it has to be, because a J-Link connect
 * clears the target's ITM configuration - the link then looks healthy, the clock
 * is programmed, and nothing ever arrives until the registers are written back.
 */
bool JlinkDebugProbe::startSwo(uint32_t coreFrequency, uint32_t traceFrequency, uint32_t stimulusMask)
{
	if (swoStarted)
		return true;

	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return false;

	JLINKARM_SWO_Config("TSEnable=1");

	const int32_t enabled = JLINKARM_SWO_EnableTarget(coreFrequency, traceFrequency, jlinkSwoInterfaceUart, stimulusMask);

	if (enabled != 0)
	{
		logger->warn("The trace unit refused to enable: {} (core {} Hz, trace {} Hz, mask 0x{:X}).", enabled, coreFrequency, traceFrequency,
		             stimulusMask);

		return false;
	}

	JLINKARM_WriteU32(0xE0000FB0u, 0xC5ACCE55u);         /* ITM->LAR: unlock */
	JLINKARM_WriteU32(0xE0000E80u, 0x00010003u);         /* ITM->TCR: TraceBusID 1, timestamps, enabled */
	JLINKARM_WriteU32(0xE0000E00u, stimulusMask);        /* ITM->TER: the ports */
	JLINKARM_WriteU32(0xE0000E40u, 0x0000000Fu);         /* ITM->TPR: every privilege level */

	JlinkSwoStart start{};
	start.sizeOfStruct = sizeof(start);
	start.interface = jlinkSwoInterfaceUart;
	start.speed = traceFrequency;

	const int32_t started = JLINKARM_SWO_Control(jlinkSwoStart, &start);

	if (started < 0)
	{
		logger->warn("The trace unit refused to start: {}.", started);

		return false;
	}

	swoStarted = true;

	logger->info("The trace unit is carrying the target's switch records.");

	return true;
}

uint32_t JlinkDebugProbe::readSwo(uint8_t* buffer, uint32_t size)
{
	if (!swoStarted)
		return 0;

	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return 0;

	uint32_t available = size;

	JLINKARM_SWO_Read(buffer, 0, &available);
	JLINKARM_SWO_Control(jlinkSwoFlush, &available);

	return available;
}

bool JlinkDebugProbe::readBlock(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);
	/* The J-Link moves the whole range in one call, which is what makes the
	   recorder buffer download bearable: a word at a time would be some eight
	   thousand round trips for a full buffer. */
	if (!isRunning || size == 0)
		return false;

	if (JLINKARM_ReadMemEx(address, size, buf, 0) >= 0)
		return true;

	invalidateLink("reading a target memory block");
	return false;
}

void JlinkDebugProbe::invalidateLink(const char* operation)
{
	lastErrorMsg = std::string("J-Link connection lost while ") + operation + ". Reconnect the probe and start acquisition again.";
	logger->error(lastErrorMsg);

	/* A USB disconnect invalidates the DLL's session even when keep-connection
	   was requested. Closing that session here makes linkOpen describe reality,
	   so the next Start opens the re-enumerated probe instead of reusing a dead
	   handle. */
	JLINKARM_Close();
	isRunning = false;
	linkOpen = false;
	openedLink.clear();
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
