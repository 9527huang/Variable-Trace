#ifndef _IELFPARSER_HPP
#define _IELFPARSER_HPP

#include <cstdint>
#include <map>
#include <string>

#include "Variable.hpp"

/*
 * Reading a symbol file.
 *
 * The application never needs the whole object file. It needs one thing: for a
 * given name, the address the target keeps it at, and how to read it. Everything
 * a parser does is in service of that answer, and which program does the reading
 * is a detail the rest of the application should not carry.
 *
 * Three implementations exist because no single one covers every target:
 *
 *   - `gdb` reads the file through a GDB child process. It is the default and
 *     the most forgiving, because GDB understands every dialect it was built
 *     for and its output is plain text.
 *   - `libdwarf` reads the DWARF sections in process. It starts much faster on
 *     a large file, which is what makes it worth having when the addresses have
 *     to be refreshed often.
 *   - `c2000` reads the file through TI's `ofd2000`, and converts the addresses
 *     it reports. The C2000 family addresses memory in 16 bit words while the
 *     probe addresses it in bytes, so an address taken from a C2000 object file
 *     is half of the byte address the probe needs.
 *
 * A parser that cannot run - the library it is built on is missing, or the
 * external tool is not installed - says so through `isAvailable` rather than
 * failing later with a confusing message.
 */
class IElfParser
{
   public:
	enum class Type : uint8_t
	{
		Gdb = 0,
		LibDwarf = 1,
		TiOfd = 2,
	};

	/* One name the symbol file carries, with what the viewer needs to turn it
	   into a variable. The address is always a byte address: a parser whose
	   target counts in words has already converted it. */
	struct Symbol
	{
		uint32_t address = 0;
		Variable::Type type = Variable::Type::UNKNOWN;
	};

	using SymbolMap = std::map<std::string, Symbol>;

	virtual ~IElfParser() = default;

	virtual Type getType() const = 0;

	/* The value the setting stores and the API accepts: "gdb", "libdwarf",
	   "c2000". */
	virtual std::string getName() const = 0;

	/* One line naming the program behind the parser, for the settings window. */
	virtual std::string getDescription() const = 0;

	/* Whether this parser can run here and now. When it cannot, reason carries
	   a sentence that says what is missing and what to do about it. Every call
	   that reads a file has to be preceded by this one, because a parser that
	   is unavailable has no other way to report why.
	   Answering it may run a program - the GDB check does - which is why the
	   method is not const. */
	virtual bool checkAvailability(std::string& reason) = 0;

	/* Collects every name in the file. Returns false only when the file could
	   not be read at all; a file that holds no variables is a success with an
	   empty result. */
	virtual bool parse(const std::string& elfPath) = 0;

	/* Re-reads the addresses and types of the variables the viewer already
	   holds, leaving the names it does not know alone. */
	virtual bool updateVariableMap(const std::string& elfPath) = 0;

	virtual SymbolMap getParsedData() const = 0;

	virtual std::string getLastErrorMsg() const = 0;

	static std::string nameOf(Type type);
	static std::string descriptionOf(Type type);

	/* Returns false for a name no parser answers to. */
	static bool typeFromName(const std::string& name, Type& type);

	/* Maps the encoding and the size of a base type onto the type the viewer
	   can read. Both DWARF readers ask this one, so the two cannot end up
	   disagreeing about what a two byte signed integer is. A size the viewer
	   has no representation for comes back UNKNOWN. */
	static Variable::Type typeFromEncoding(uint64_t encoding, uint64_t byteSize);
};

#endif
