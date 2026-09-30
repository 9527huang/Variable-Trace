#include "IElfParser.hpp"

std::string IElfParser::nameOf(Type type)
{
	switch (type)
	{
		case Type::Gdb:
			return "gdb";

		case Type::LibDwarf:
			return "libdwarf";

		case Type::TiOfd:
			return "c2000";
	}

	return "gdb";
}

std::string IElfParser::descriptionOf(Type type)
{
	switch (type)
	{
		case Type::Gdb:
			return "GDB: reliable, default.";

		case Type::LibDwarf:
			return "DWARF: faster variable address updates (beta).";

		case Type::TiOfd:
			return "C2000: for TI C2000 targets, uses ofd2000.";
	}

	return "";
}

bool IElfParser::typeFromName(const std::string& name, Type& type)
{
	if (name == "gdb")
	{
		type = Type::Gdb;
		return true;
	}

	if (name == "libdwarf")
	{
		type = Type::LibDwarf;
		return true;
	}

	if (name == "c2000")
	{
		type = Type::TiOfd;
		return true;
	}

	return false;
}

/* The DWARF base type encodings that describe a number the viewer can read.
   The values are the ones the standard assigns, spelled out rather than taken
   from the library header, so that this file stays free of the dependency. */
namespace
{
	constexpr uint64_t dwAteBoolean = 0x02;
	constexpr uint64_t dwAteFloat = 0x04;
	constexpr uint64_t dwAteSigned = 0x05;
	constexpr uint64_t dwAteSignedChar = 0x06;
	constexpr uint64_t dwAteUnsigned = 0x07;
	constexpr uint64_t dwAteUnsignedChar = 0x08;
}  // namespace

Variable::Type IElfParser::typeFromEncoding(uint64_t encoding, uint64_t byteSize)
{
	switch (encoding)
	{
		case dwAteBoolean:
		case dwAteUnsigned:
		case dwAteUnsignedChar:
			switch (byteSize)
			{
				case 1:
					return Variable::Type::U8;

				case 2:
					return Variable::Type::U16;

				case 4:
					return Variable::Type::U32;

				default:
					return Variable::Type::UNKNOWN;
			}

		case dwAteSigned:
		case dwAteSignedChar:
			switch (byteSize)
			{
				case 1:
					return Variable::Type::I8;

				case 2:
					return Variable::Type::I16;

				case 4:
					return Variable::Type::I32;

				default:
					return Variable::Type::UNKNOWN;
			}

		case dwAteFloat:
			/* The viewer reads a 32 bit float and nothing wider. */
			return byteSize == 4 ? Variable::Type::F32 : Variable::Type::UNKNOWN;

		default:
			return Variable::Type::UNKNOWN;
	}
}
