#include "LibDwarfParser.hpp"

/* The two operations below are the ones this file recognises, and their values
   are written out rather than taken from the library header, so that the file
   builds and is tested with or without the library. Nothing in it calls into
   the library either way. */
namespace
{
	constexpr uint8_t dwOpAddr = 0x03;
	constexpr uint8_t dwOpAddrx = 0xa1;
}  // namespace

LibDwarfParser::Location LibDwarfParser::decodeLocation(const std::vector<uint8_t>& expression, uint32_t addressSize)
{
	Location location;

	if (expression.empty())
		return location;

	/* A location expression is a program. Two of its operations begin with an
	   address that is fixed for the lifetime of the program, and those are the
	   only ones a symbol file can answer for; the others describe how to find
	   the value once the program runs, usually in a register or on the stack. */
	const uint8_t operation = expression[0];

	if (operation == dwOpAddr)
	{
		if (expression.size() < 1u + addressSize)
			return location;

		uint64_t address = 0;

		for (uint32_t index = 0; index < addressSize; index++)
			address |= static_cast<uint64_t>(expression[1u + index]) << (8u * index);

		location.form = LocationForm::Address;
		location.value = address;
		return location;
	}

	if (operation == dwOpAddrx)
	{
		/* The operand is a ULEB128 index into .debug_addr, which the caller
		   turns into an address because only it has the DIE to ask. */
		uint64_t index = 0;
		uint32_t shift = 0;
		size_t at = 1;
		bool terminated = false;

		/* The last byte of the number has its continuation bit clear. An
		   operand that runs off the end of the expression, or one that keeps
		   promising another byte past the width of the value, never ends, and a
		   partial number is not an index anyone can use. */
		while (at < expression.size() && shift < 64u)
		{
			const uint8_t byte = expression[at++];
			index |= static_cast<uint64_t>(byte & 0x7fu) << shift;
			shift += 7u;

			if ((byte & 0x80u) == 0)
			{
				terminated = true;
				break;
			}
		}

		if (!terminated)
			return location;

		location.form = LocationForm::AddressIndex;
		location.value = index;
		return location;
	}

	location.form = LocationForm::Unsupported;
	return location;
}

Variable::Type LibDwarfParser::typeFromEncoding(uint64_t encoding, uint64_t byteSize)
{
	/* One implementation of the mapping, shared with the C2000 reader, so that
	   the two parsers cannot disagree about what a size and an encoding mean. */
	return IElfParser::typeFromEncoding(encoding, byteSize);
}
