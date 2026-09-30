#include "SerialProtocol.hpp"

#include <stdexcept>
#include <string>

namespace
{
	/* ____crc16NibbleTable from serialDriver.c. */
	constexpr uint16_t nibbleTable[16] = {
		0x0000, 0xC0C1, 0xC181, 0x0140,
		0xC301, 0x03C0, 0x0280, 0xC241,
		0xC601, 0x06C0, 0x0780, 0xC741,
		0x0500, 0xC5C1, 0xC481, 0x0440};

	void appendLittleEndian(std::vector<uint8_t>& out, uint32_t value, size_t bytes)
	{
		for (size_t index = 0; index < bytes; index++)
			out.push_back(static_cast<uint8_t>((value >> (8 * index)) & 0xFF));
	}

	void appendChecksum(std::vector<uint8_t>& out)
	{
		appendLittleEndian(out, serial::crc16(out), serial::crcLength);
	}

	void appendHeader(std::vector<uint8_t>& out, serial::Command command, uint8_t length)
	{
		out.push_back(serial::startOfFrame1);
		out.push_back(serial::startOfFrame2);
		out.push_back(static_cast<uint8_t>(command));
		out.push_back(length);
	}

	std::string describe(serial::Command command)
	{
		switch (command)
		{
			case serial::Command::Read:
				return "read";
			case serial::Command::Write:
				return "write";
			case serial::Command::BufferRead:
				return "buffer read";
		}

		return "unknown";
	}
}  // namespace

namespace serial
{
	uint16_t crc16Update(uint16_t crc, uint8_t byte)
	{
		crc = (crc >> 4) ^ nibbleTable[(crc & 0x0F) ^ (byte & 0x0F)];
		crc = (crc >> 4) ^ nibbleTable[(crc & 0x0F) ^ (byte >> 4)];
		return crc;
	}

	uint16_t crc16(const uint8_t* data, size_t length)
	{
		uint16_t crc = 0xFFFF;

		for (size_t index = 0; index < length; index++)
			crc = crc16Update(crc, data[index]);

		return crc;
	}

	uint16_t crc16(const std::vector<uint8_t>& data)
	{
		return crc16(data.data(), data.size());
	}

	size_t readResponseSize(const std::vector<ReadEntry>& entries)
	{
		size_t total = 0;

		for (const ReadEntry& entry : entries)
			total += entry.size;

		return total;
	}

	std::vector<uint8_t> buildReadRequest(const std::vector<ReadEntry>& entries)
	{
		if (entries.empty())
			throw std::invalid_argument("a bulk read needs at least one entry");

		if (entries.size() > defaultMaxVariables)
			throw std::invalid_argument("a bulk read carries at most " + std::to_string(defaultMaxVariables) + " entries, got " + std::to_string(entries.size()));

		size_t total = 0;

		for (const ReadEntry& entry : entries)
		{
			if (entry.size == 0 || entry.size > maxEntrySize)
				throw std::invalid_argument("entry at address " + std::to_string(entry.address) + " asks for " + std::to_string(entry.size) + " bytes, the driver carries 1 to " + std::to_string(maxEntrySize));

			total += entry.size;
		}

		/* The driver builds its reply in a buffer of four bytes per entry plus
		   the checksum, so a request that asks for more would overrun it. */
		if (total > maxEntrySize * defaultMaxVariables)
			throw std::invalid_argument("a bulk read asks for " + std::to_string(total) + " bytes in total, the driver answers at most " + std::to_string(maxEntrySize * defaultMaxVariables));

		std::vector<uint8_t> frame;
		frame.reserve(4 + entries.size() * (addressLength + 1) + crcLength);

		appendHeader(frame, Command::Read, static_cast<uint8_t>(entries.size()));

		for (const ReadEntry& entry : entries)
		{
			appendLittleEndian(frame, entry.address, addressLength);
			frame.push_back(entry.size);
		}

		appendChecksum(frame);
		return frame;
	}

	std::vector<uint8_t> buildWriteRequest(uint32_t address, const uint8_t* data, size_t size)
	{
		if (data == nullptr)
			throw std::invalid_argument("a write needs data");

		/* The driver accumulates the value into one register and shifts by eight
		   bits per byte, so more than four bytes is undefined on the target. */
		if (size == 0 || size > maxEntrySize)
			throw std::invalid_argument("a write carries 1 to " + std::to_string(maxEntrySize) + " bytes, got " + std::to_string(size));

		std::vector<uint8_t> frame;
		frame.reserve(4 + addressLength + size + crcLength);

		appendHeader(frame, Command::Write, static_cast<uint8_t>(size));
		appendLittleEndian(frame, address, addressLength);
		frame.insert(frame.end(), data, data + size);
		appendChecksum(frame);

		return frame;
	}

	std::vector<uint8_t> buildBufferReadRequest(uint32_t address, uint8_t size)
	{
		if (size == 0)
			throw std::invalid_argument("a buffer read needs a size of at least one byte");

		std::vector<uint8_t> frame;
		frame.reserve(4 + addressLength + crcLength);

		appendHeader(frame, Command::BufferRead, size);
		appendLittleEndian(frame, address, addressLength);
		appendChecksum(frame);

		return frame;
	}

	RequestParser::RequestParser(size_t maxVariables) : maxVariables(maxVariables)
	{
	}

	void RequestParser::reset()
	{
		state = State::WaitSof1;
		calculatedCrc = 0xFFFF;
		receivedCrc = 0;
		current = Request{};
		length = 0;
		index = 0;
		accum = 0;
	}

	std::optional<Request> RequestParser::push(uint8_t byte)
	{
		switch (state)
		{
			case State::WaitSof1:
				if (byte == startOfFrame1)
				{
					calculatedCrc = 0xFFFF;
					calculatedCrc = crc16Update(calculatedCrc, byte);
					state = State::WaitSof2;
				}
				break;

			case State::WaitSof2:
				if (byte == startOfFrame2)
				{
					calculatedCrc = crc16Update(calculatedCrc, byte);
					state = State::Command;
				}
				else
				{
					/* The driver does not treat the mismatching byte as a new start
					   of frame, so a run of 0xAA bytes ends on the first one and the
					   others are lost. Kept as it is, because the target is what the
					   host has to match. */
					state = State::WaitSof1;
				}
				break;

			case State::Command:
				if (byte != static_cast<uint8_t>(Command::Read) && byte != static_cast<uint8_t>(Command::Write) && byte != static_cast<uint8_t>(Command::BufferRead))
				{
					droppedFrames++;
					reset();
					break;
				}

				current.command = static_cast<Command>(byte);
				calculatedCrc = crc16Update(calculatedCrc, byte);
				state = State::Length;
				break;

			case State::Length:
				length = byte;

				if (current.command == Command::Read)
				{
					/* The driver refuses a count of zero and one above its limit
					   without checking the rest of the frame. */
					if (length == 0 || length > maxVariables)
					{
						droppedFrames++;
						reset();
						break;
					}
				}
				else if (current.command == Command::Write)
				{
					if (length == 0 || length > maxEntrySize)
					{
						droppedFrames++;
						reset();
						break;
					}

					current.payload.assign(length, 0);
				}

				calculatedCrc = crc16Update(calculatedCrc, byte);
				index = 0;
				accum = 0;

				if (current.command == Command::Read)
					current.entries.assign(1, ReadEntry{});

				state = State::Address;
				break;

			case State::Address:
				calculatedCrc = crc16Update(calculatedCrc, byte);

				/* A bulk read carries one address per entry, so the bytes
				   accumulate into the entry being filled rather than into a
				   single address shared by the whole frame. */
				if (current.command == Command::Read)
				{
					ReadEntry& entry = current.entries.back();
					entry.address |= static_cast<uint32_t>(byte) << (8 * index);
					index++;

					if (index >= addressLength)
					{
						index = 0;
						state = State::Size;
					}

					break;
				}

				accum |= static_cast<uint32_t>(byte) << (8 * index);
				index++;

				if (index >= addressLength)
				{
					index = 0;

					if (current.command == Command::BufferRead)
					{
						current.address = accum;
						current.bufferReadSize = length;
						state = State::Crc1;
					}
					else
					{
						current.address = accum;
						state = State::Data;
					}

					accum = 0;
				}
				break;

			case State::Size:
				calculatedCrc = crc16Update(calculatedCrc, byte);
				current.entries.back().size = byte;

				if (current.entries.size() >= length)
					state = State::Crc1;
				else
				{
					current.entries.push_back(ReadEntry{});
					index = 0;
					state = State::Address;
				}
				break;

			case State::Data:
				calculatedCrc = crc16Update(calculatedCrc, byte);
				accum |= static_cast<uint32_t>(byte) << (8 * index);
				index++;

				if (index >= length)
					state = State::Crc1;
				break;

			case State::Crc1:
				receivedCrc = byte;
				state = State::Crc2;
				break;

			case State::Crc2:
			{
				receivedCrc |= static_cast<uint16_t>(byte) << 8;

				std::optional<Request> result;

				if (calculatedCrc == receivedCrc)
				{
					if (current.command == Command::Write)
					{
						for (size_t byteIndex = 0; byteIndex < current.payload.size(); byteIndex++)
							current.payload[byteIndex] = static_cast<uint8_t>((accum >> (8 * byteIndex)) & 0xFF);
					}

					result = current;
				}
				else
					crcErrors++;

				reset();
				return result;
			}
		}

		return std::nullopt;
	}

	ResponseReader::ResponseReader(size_t expectedSize, bool hasCrc) : expected(expectedSize), hasCrc(hasCrc)
	{
		data.reserve(expectedSize + (hasCrc ? crcLength : 0));
	}

	void ResponseReader::reset(size_t expectedSize, bool hasCrc)
	{
		expected = expectedSize;
		this->hasCrc = hasCrc;
		received = 0;
		complete = false;
		checksumValid = false;
		data.clear();
		data.reserve(expectedSize + (hasCrc ? crcLength : 0));
	}

	bool ResponseReader::push(uint8_t byte)
	{
		if (complete)
			return true;

		data.push_back(byte);
		received++;

		const size_t payloadSize = expected;
		const size_t expectedTotal = payloadSize + (hasCrc ? crcLength : 0);

		if (received < expectedTotal)
			return false;

		complete = true;

		if (!hasCrc)
		{
			checksumValid = true;
			return true;
		}

		const uint16_t calculated = crc16(data.data(), payloadSize);
		const uint16_t receivedChecksum = static_cast<uint16_t>(data[payloadSize]) | (static_cast<uint16_t>(data[payloadSize + 1]) << 8);
		checksumValid = calculated == receivedChecksum;

		/* The checksum bytes are not part of the answer. */
		data.resize(payloadSize);
		return true;
	}
}  // namespace serial
