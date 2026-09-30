#ifndef _SerialProtocol_HPP
#define _SerialProtocol_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/*
 * Wire format of the target side serial driver (serialDriver.h v1.2).
 *
 * A request is framed:
 *
 *   0xAA 0x55 | command | length | address (4, little endian) | data | crc16 (2, little endian)
 *
 * A reply is not framed at all. The target writes plain bytes back:
 *
 *   read        -> the requested values, then crc16 of those values
 *   write       -> one status byte, 0x00 on success, no crc
 *   buffer read -> the requested bytes, no crc
 *
 * So the length of a reply follows from the request and the reply is collected
 * as a fixed number of bytes. Looking for a start marker in the reply, which is
 * the obvious first guess, would never find one.
 *
 * The CRC is the CRC-16 variant the driver computes over a sixteen entry nibble
 * table, seeded with 0xFFFF. It covers the whole request frame including both
 * start bytes and excludes the two CRC bytes themselves.
 */

namespace serial
{
	enum class Command : uint8_t
	{
		Read = 0x01,
		Write = 0x02,
		BufferRead = 0x03,
	};

	constexpr uint8_t startOfFrame1 = 0xAA;
	constexpr uint8_t startOfFrame2 = 0x55;
	constexpr uint8_t responseOk = 0x00;

	constexpr size_t addressLength = 4;
	constexpr size_t crcLength = 2;

	/* ____SERIAL_DRIVER_MAX_VAR_LEN. A variable is at most a 32 bit value, so a
	   single bulk entry never carries more than four bytes. */
	constexpr size_t maxEntrySize = 4;

	/* ____SERIAL_DRIVER_MAXVARS defaults to 10 in serialDriverDefines.h and is
	   the number of entries a single bulk read may carry. The driver refuses a
	   count of zero or more than this and drops the frame. */
	constexpr size_t defaultMaxVariables = 10;

	/* The length field is one byte, which is what limits a buffer read chunk. */
	constexpr size_t maxPayloadSize = 0xFF;

	/* ----------------------------------------------------------------------
	 * Checksum
	 * -------------------------------------------------------------------- */

	uint16_t crc16Update(uint16_t crc, uint8_t byte);
	uint16_t crc16(const uint8_t* data, size_t length);
	uint16_t crc16(const std::vector<uint8_t>& data);

	/* ----------------------------------------------------------------------
	 * Requests
	 * -------------------------------------------------------------------- */

	/* One entry of a bulk read: where to read and how many bytes. The driver
	   carries the size as a single byte even though it is declared 16 bit. */
	struct ReadEntry
	{
		uint32_t address = 0;
		uint8_t size = 0;
	};

	/* Bytes a bulk read reply carries, excluding the trailing CRC. */
	size_t readResponseSize(const std::vector<ReadEntry>& entries);

	/* All three throw std::invalid_argument on a request the driver would drop,
	   rather than sending a frame that is silently ignored. */
	std::vector<uint8_t> buildReadRequest(const std::vector<ReadEntry>& entries);
	std::vector<uint8_t> buildWriteRequest(uint32_t address, const uint8_t* data, size_t size);
	std::vector<uint8_t> buildBufferReadRequest(uint32_t address, uint8_t size);

	/* A request as it arrives at the target, used to verify the builders and to
	   drive a target simulation in the tests. */
	struct Request
	{
		Command command = Command::Read;
		uint32_t address = 0;
		std::vector<ReadEntry> entries;	// read only
		std::vector<uint8_t> payload;	// write only
		uint8_t bufferReadSize = 0;		// buffer read only
	};

	/* Mirrors the receive state machine of the driver, including its habit of
	   dropping a frame whose CRC does not match and one that carries an unknown
	   command or an out of range entry count. */
	class RequestParser
	{
	   public:
		RequestParser(size_t maxVariables = defaultMaxVariables);

		void reset();

		/* Feeds one byte and returns the request when the byte completes one. */
		std::optional<Request> push(uint8_t byte);

		uint32_t getCrcErrorCount() const { return crcErrors; }
		uint32_t getDroppedFrameCount() const { return droppedFrames; }

	   private:
		enum class State
		{
			WaitSof1,
			WaitSof2,
			Command,
			Length,
			Address,
			Size,
			Data,
			Crc1,
			Crc2,
		};

		size_t maxVariables;
		State state = State::WaitSof1;
		uint16_t calculatedCrc = 0xFFFF;
		uint16_t receivedCrc = 0;
		Request current;
		uint8_t length = 0;
		size_t index = 0;
		uint32_t accum = 0;
		uint32_t crcErrors = 0;
		uint32_t droppedFrames = 0;
	};

	/* ----------------------------------------------------------------------
	 * Replies
	 * -------------------------------------------------------------------- */

	/* Collects a fixed number of reply bytes and, when the reply ends with a
	   CRC, checks it. A reply that fails the check is reported through
	   isComplete() together with an invalid checksum rather than being dropped:
	   on a real line a length mismatch is worth logging. */
	class ResponseReader
	{
	   public:
		ResponseReader() = default;
		ResponseReader(size_t expectedSize, bool hasCrc);

		void reset(size_t expectedSize, bool hasCrc);

		/* Feeds one byte and returns true once the expected count is reached. Once
	   the reply is complete the byte is dropped and the call keeps returning
	   true, so the answer never grows past its length. */
	bool push(uint8_t byte);

		bool isComplete() const { return complete; }
		bool isChecksumValid() const { return checksumValid; }

		/* Bytes before the CRC. Only meaningful once the reply is complete. */
		const std::vector<uint8_t>& getData() const { return data; }

		size_t getExpectedSize() const { return expected; }
		size_t getReceivedSize() const { return received; }

	   private:
		size_t expected = 0;
		size_t received = 0;
		bool hasCrc = false;
		bool complete = false;
		bool checksumValid = false;
		std::vector<uint8_t> data;
	};
}  // namespace serial

#endif
