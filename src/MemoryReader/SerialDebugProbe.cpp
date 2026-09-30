#include "SerialDebugProbe.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

SerialDebugProbe::SerialDebugProbe(spdlog::logger* logger) : logger(logger), transport(serial::createSystemTransport())
{
}

SerialDebugProbe::SerialDebugProbe(spdlog::logger* logger, std::unique_ptr<serial::ITransport> transport) : logger(logger), transport(std::move(transport))
{
}

uint32_t SerialDebugProbe::replyTimeoutMs(size_t expectedBytes) const
{
	const uint32_t baudrate = std::max<uint32_t>(activeBaudrate, 1200);

	/* Ten bits on the line per byte, so the transfer itself is predictable and
	   only the reaction of the target is unknown. */
	const uint32_t transferMs = static_cast<uint32_t>((expectedBytes * 10 * 1000) / baudrate);
	return replyGraceMs + transferMs;
}

bool SerialDebugProbe::exchange(const std::vector<uint8_t>& request, serial::ResponseReader& reply)
{
	if (!transport->isOpen())
	{
		lastErrorMsg = "The serial port is not open.";
		isRunning = false;
		return false;
	}

	/* Anything left over from an earlier frame would be read as the beginning of
	   this answer, and a short frame followed by a long one is exactly how that
	   happens. */
	transport->discardInput();

	if (!transport->write(request.data(), request.size()))
	{
		lastErrorMsg = transport->getLastError();
		isRunning = false;
		return false;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(replyTimeoutMs(reply.getExpectedSize()));

	uint8_t buffer[64] = {};

	while (!reply.isComplete())
	{
		const auto now = std::chrono::steady_clock::now();

		if (now >= deadline)
		{
			lastErrorMsg = "The target did not answer within " + std::to_string(replyTimeoutMs(reply.getExpectedSize())) + " ms. Check the port, the baud rate and that the target firmware serves the serial driver.";
			isRunning = false;
			return false;
		}

		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		const size_t received = transport->read(buffer, sizeof(buffer), static_cast<uint32_t>(std::max<int64_t>(remaining, 1)));

		if (received == 0)
		{
			const std::string transportError = transport->getLastError();

			if (!transportError.empty())
			{
				lastErrorMsg = transportError;
				isRunning = false;
				return false;
			}

			continue;
		}

		for (size_t index = 0; index < received && !reply.isComplete(); index++)
			reply.push(buffer[index]);
	}

	/* Bytes past the end of the answer belong to no request, and the next answer
	   would otherwise be read from them. */
	transport->discardInput();
	return true;
}

bool SerialDebugProbe::readEntries(const std::vector<serial::ReadEntry>& entries, std::vector<uint8_t>& out)
{
	std::vector<uint8_t> request;

	try
	{
		request = serial::buildReadRequest(entries);
	}
	catch (const std::invalid_argument& error)
	{
		lastErrorMsg = error.what();
		return false;
	}

	serial::ResponseReader reply(serial::readResponseSize(entries), true);

	if (!exchange(request, reply))
		return false;

	if (!reply.isChecksumValid())
	{
		lastErrorMsg = "The target answered a read with a wrong checksum, so the value cannot be trusted.";
		isRunning = false;
		return false;
	}

	out = reply.getData();
	return true;
}

bool SerialDebugProbe::fetchSweep()
{
	sweepData.clear();
	sweepValueOffset.assign(sweepList.size(), 0);

	size_t index = 0;

	while (index < sweepList.size())
	{
		/* The driver refuses a frame with more entries than its own limit and
		   refuses one whose sizes do not fit its answer buffer, so the pass is
		   split into as few frames as both limits allow. */
		std::vector<serial::ReadEntry> chunk;
		size_t chunkBytes = 0;

		while (index + chunk.size() < sweepList.size() && chunk.size() < serial::defaultMaxVariables)
		{
			const SweepEntry& entry = sweepList[index + chunk.size()];

			if (!chunk.empty() && chunkBytes + entry.size > serial::maxEntrySize * serial::defaultMaxVariables)
				break;

			chunk.push_back({entry.address, entry.size});
			chunkBytes += entry.size;
		}

		std::vector<uint8_t> chunkData;

		if (!readEntries(chunk, chunkData))
			return false;

		if (chunkData.size() != chunkBytes)
		{
			lastErrorMsg = "The target answered a bulk read with " + std::to_string(chunkData.size()) + " bytes instead of " + std::to_string(chunkBytes) + ".";
			isRunning = false;
			return false;
		}

		size_t chunkOffset = 0;

		for (size_t entry = 0; entry < chunk.size(); entry++)
		{
			sweepValueOffset[index + entry] = sweepData.size();
			sweepData.insert(sweepData.end(), chunkData.begin() + chunkOffset, chunkData.begin() + chunkOffset + chunk[entry].size);
			chunkOffset += chunk[entry].size;
		}

		index += chunk.size();
	}

	return true;
}

bool SerialDebugProbe::startAcqusition(const DebugProbeSettings& probeSettings, std::vector<std::pair<uint32_t, uint8_t>>& addressSizeVector, uint32_t samplingFreqency)
{
	std::lock_guard<std::mutex> lock(mtx);
	isRunning = false;

	(void)samplingFreqency;

	if (probeSettings.serialNumber.empty())
	{
		lastErrorMsg = "No serial port was selected. Press the refresh button next to the probe name to list the ports the system reports.";
		return false;
	}

	if (addressSizeVector.empty())
	{
		lastErrorMsg = "The active group has no visible variable, so there is nothing to read.";
		return false;
	}

	for (const auto& [address, size] : addressSizeVector)
	{
		if (size == 0 || size > serial::maxEntrySize)
		{
			lastErrorMsg = "Variable at address " + std::to_string(address) + " has a size of " + std::to_string(size) + " bytes, the serial driver carries one to " + std::to_string(serial::maxEntrySize) + ".";
			return false;
		}
	}

	activeBaudrate = probeSettings.baudrate == 0 ? 115200 : probeSettings.baudrate;

	if (!transport->open(probeSettings.serialNumber, activeBaudrate))
	{
		lastErrorMsg = transport->getLastError();
		return false;
	}

	sweepList.clear();
	sweepList.reserve(addressSizeVector.size());

	for (const auto& [address, size] : addressSizeVector)
		sweepList.push_back({address, size});

	sweepData.clear();
	sweepValueOffset.assign(sweepList.size(), 0);
	sweepServed = 0;
	sweepDataValid = false;

	isRunning = true;
	lastErrorMsg.clear();

	logger->info("Serial debug probe opened {} at {} baud, {} variables per pass", probeSettings.serialNumber, activeBaudrate, sweepList.size());

	return true;
}

bool SerialDebugProbe::stopAcqusition()
{
	std::lock_guard<std::mutex> lock(mtx);
	isRunning = false;
	transport->close();
	sweepDataValid = false;
	sweepServed = 0;
	return true;
}

bool SerialDebugProbe::isValid() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return isRunning;
}

std::optional<IDebugProbe::varEntryType> SerialDebugProbe::readSingleEntry()
{
	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning || sweepList.empty())
		return std::nullopt;

	if (!fetchSweep())
		return std::nullopt;

	std::unordered_map<uint32_t, double> values;

	for (size_t index = 0; index < sweepList.size(); index++)
	{
		uint32_t value = 0;
		std::memcpy(&value, sweepData.data() + sweepValueOffset[index], sweepList[index].size);
		values[sweepList[index].address] = value;
	}

	const double timestamp = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now().time_since_epoch()).count();
	return std::make_pair(timestamp, values);
}

bool SerialDebugProbe::readMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return false;

	if (size == 0 || size > serial::maxEntrySize)
	{
		lastErrorMsg = "A single read carries one to four bytes, got " + std::to_string(size) + ".";
		return false;
	}

	/* Continuing the pass that is already in hand costs nothing, so the whole
	   pass is fetched on its first entry and the rest are served from it. */
	const bool continuesThePass = sweepServed < sweepList.size() && sweepList[sweepServed].address == address && sweepList[sweepServed].size == size;

	if (continuesThePass)
	{
		if (!sweepDataValid)
		{
			if (!fetchSweep())
			{
				sweepServed = 0;
				sweepDataValid = false;
				return false;
			}

			sweepDataValid = true;
		}

		std::memcpy(buf, sweepData.data() + sweepValueOffset[sweepServed], size);
		sweepServed++;

		if (sweepServed >= sweepList.size())
		{
			/* The pass is over, so the next request starts a new one and has to
			   reach the target again. */
			sweepServed = 0;
			sweepDataValid = false;
		}

		return true;
	}

	/* Something other than the next entry of the pass, which is what a write in
	   between two reads or a caller that walks the list in another order does. */
	sweepServed = 0;
	sweepDataValid = false;

	std::vector<uint8_t> value;

	if (!readEntries({{address, static_cast<uint8_t>(size)}}, value))
		return false;

	if (value.size() < size)
	{
		lastErrorMsg = "The target answered a read with fewer bytes than were asked for.";
		return false;
	}

	std::memcpy(buf, value.data(), size);
	return true;
}

bool SerialDebugProbe::writeMemory(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return false;

	/* The target memory just changed, so a pass that was fetched before this is
	   no longer what the target holds. */
	sweepServed = 0;
	sweepDataValid = false;

	std::vector<uint8_t> request;

	try
	{
		request = serial::buildWriteRequest(address, buf, size);
	}
	catch (const std::invalid_argument& error)
	{
		lastErrorMsg = error.what();
		return false;
	}

	serial::ResponseReader reply(1, false);

	if (!exchange(request, reply))
		return false;

	if (reply.getData().empty() || reply.getData()[0] != serial::responseOk)
	{
		lastErrorMsg = "The target refused the write to address " + std::to_string(address) + ".";
		return false;
	}

	return true;
}

bool SerialDebugProbe::readBlock(uint32_t address, uint8_t* buf, uint32_t size)
{
	std::lock_guard<std::mutex> lock(mtx);

	if (!isRunning)
		return false;

	if (size == 0)
		return true;

	/* Nothing is cached here on purpose: a block read is used to pull the
	   recorder buffer, which only ever moves forward. */
	sweepServed = 0;
	sweepDataValid = false;

	uint32_t transferred = 0;

	while (transferred < size)
	{
		const uint32_t chunk = std::min<uint32_t>(serial::maxPayloadSize, size - transferred);
		std::vector<uint8_t> request;

		try
		{
			request = serial::buildBufferReadRequest(address + transferred, static_cast<uint8_t>(chunk));
		}
		catch (const std::invalid_argument& error)
		{
			lastErrorMsg = error.what();
			return false;
		}

		/* A buffer read answer carries no checksum, so a truncated one cannot be
		   told from a complete one and the length is the only check there is. */
		serial::ResponseReader reply(chunk, false);

		if (!exchange(request, reply))
			return false;

		if (reply.getData().size() != chunk)
		{
			lastErrorMsg = "The target answered a block read at " + std::to_string(address + transferred) + " with " + std::to_string(reply.getData().size()) + " bytes instead of " + std::to_string(chunk) + ".";
			isRunning = false;
			return false;
		}

		std::memcpy(buf + transferred, reply.getData().data(), chunk);
		transferred += chunk;
	}

	return true;
}

std::string SerialDebugProbe::getLastErrorMsg() const
{
	return lastErrorMsg;
}

std::vector<std::string> SerialDebugProbe::getConnectedDevices()
{
	return serial::enumeratePorts();
}
