#ifndef _SerialTransport_HPP
#define _SerialTransport_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/*
 * Byte level access to a serial port.
 *
 * The probe talks to this and never to the operating system directly, so a test
 * can stand in for the port and for the target behind it. Everything the probe
 * needs from a port is in here, including the wait, which a loopback of plain
 * file handles would otherwise be missing.
 */

namespace serial
{
	class ITransport
	{
	   public:
		virtual ~ITransport() = default;

		/* Opens the port and applies the line settings. */
		virtual bool open(const std::string& portName, uint32_t baudrate) = 0;
		virtual void close() = 0;
		virtual bool isOpen() const = 0;

		/* Writes the whole buffer or fails. */
		virtual bool write(const uint8_t* data, size_t size) = 0;

		/* Waits up to timeoutMs for at least one byte and returns how many
		   arrived, at most size. Zero means the wait ran out. */
		virtual size_t read(uint8_t* data, size_t size, uint32_t timeoutMs) = 0;

		/* Throws away bytes that arrived but were not consumed, so that the next
		   reply is not read from the leftovers of an earlier one. */
		virtual void discardInput() = 0;

		virtual std::string getLastError() const = 0;
	};

	/* A port offered by the operating system, created on each call so that the
	   probe owns the handle for exactly as long as a run lasts. */
	std::unique_ptr<ITransport> createSystemTransport();

	/* Names of the ports the operating system reports, in the form the probe and
	   the settings field expect: "COM3" on Windows, "/dev/ttyUSB0" elsewhere. */
	std::vector<std::string> enumeratePorts();
}  // namespace serial

#endif
