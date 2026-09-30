#ifndef _BorrowedTransport_HPP
#define _BorrowedTransport_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "SerialTransport.hpp"

/*
 * Hands a transport to an owner that expects to hold one, while the object that
 * really talks stays with the test.
 *
 * The probe takes its transport by unique_ptr, and a test that gives up the
 * simulator would have no way of looking at the memory afterwards. This passes
 * the calls through and keeps the reference.
 */
class BorrowedTransport : public serial::ITransport
{
   public:
	explicit BorrowedTransport(serial::ITransport& borrowed) : borrowed(borrowed) {}

	bool open(const std::string& portName, uint32_t baudrate) override { return borrowed.open(portName, baudrate); }
	void close() override { borrowed.close(); }
	bool isOpen() const override { return borrowed.isOpen(); }
	bool write(const uint8_t* data, size_t size) override { return borrowed.write(data, size); }
	size_t read(uint8_t* data, size_t size, uint32_t timeoutMs) override { return borrowed.read(data, size, timeoutMs); }
	void discardInput() override { borrowed.discardInput(); }
	std::string getLastError() const override { return borrowed.getLastError(); }

   private:
	serial::ITransport& borrowed;
};

#endif
