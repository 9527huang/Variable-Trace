#ifndef _TIOFDPARSER_HPP
#define _TIOFDPARSER_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "IElfParser.hpp"
#include "OfdDwarfXml.hpp"
#include "Variable.hpp"
#include "VariableHandler.hpp"
#include "spdlog/spdlog.h"

/*
 * Reading a TI C2000 symbol file through the TI object file display tool.
 *
 * A C2000 object file does not need this parser because of its DWARF, which is
 * ordinary, but because of how the family counts memory. A C2000 counts in 16
 * bit address units; the host and the debug probe count in 8 bit bytes. Every
 * quantity the object file states - an address, the size of a type, where a
 * member sits inside a structure, how far apart two elements of an array are -
 * is therefore in address units and has to be doubled before the viewer or the
 * probe uses it. A parser that passes them through unchanged reads valid memory
 * that holds the wrong value, and reads half of a 32 bit number as if it were
 * the whole of it.
 *
 * The tool is asked for its XML form rather than its text form. The text form
 * reports a DIE as a line with an offset and a nesting level and its attributes
 * as the lines under it, with nothing marking where the next entry begins; the
 * XML form states the offset and the tag of every entry and states the offsets
 * an entry refers to, which is what a chain of types needs in order to be
 * followed.
 *
 * The chain is what the walk is built around. A variable points at a typedef,
 * which points at a `const`, which points at the real type; a structure points
 * at its members; an array points at its element type. Following offsets rather
 * than nesting by position is what keeps a type that contains a pointer to
 * itself from expanding forever.
 *
 * The parts that are arithmetic rather than parsing - the unit conversion, the
 * location operation, the encoding to type mapping - are static and covered by
 * tests. The walk needs a dump to run on, and a dump needs the tool, which is
 * not installed on every machine, so producing the dump is a separate step from
 * walking it: `parseDump` turns a document into symbols and is tested against a
 * hand written one, and `parse` is the two steps together.
 */
class TiOfdParser : public IElfParser
{
   public:
	TiOfdParser(VariableHandler* variableHandler, spdlog::logger* logger);

	Type getType() const override;
	std::string getName() const override;
	std::string getDescription() const override;
	bool checkAvailability(std::string& reason) override;

	bool parse(const std::string& elfPath) override;
	bool updateVariableMap(const std::string& elfPath) override;
	SymbolMap getParsedData() const override;
	std::string getLastErrorMsg() const override;

	/* The program that reads the object file. It belongs to the TI toolchain
	   and is not shipped with the application, so its location is a setting and
	   the parser refuses to run without it. */
	void setToolCommand(const std::string& command);
	std::string getToolCommand() const;

	/* True when the command names a program that exists: a path that resolves,
	   or a name the operating system finds. Nothing is executed, because the
	   tool has no documented way to be asked whether it is the tool. */
	static bool toolExists(const std::string& command);

	/* Runs the tool over a file and hands back what it printed. Separate from
	   the walk so that both halves can be exercised on their own. */
	bool runDump(const std::string& elfPath, std::string& xml, std::string& error) const;

	/* Turns a dump into symbols, without touching the file system. The result
	   replaces what the parser holds, so a caller that has a dump and no tool
	   still ends up with a parser that answers. */
	void parseDump(const std::string& xml, const std::string& sourceName);

	/* One address unit is one 16 bit word. The probe addresses bytes, so a
	   quantity taken from the object file is multiplied by this. */
	static constexpr uint32_t bytesPerAddressUnit = 2;

	static uint32_t toByteAddress(uint64_t address);

	/* Reads the operation list of a location into the address it carries, in
	   address units. Only the operation that states an address directly is
	   accepted; a location in a register or at a frame offset has no address
	   that can be read before the program runs. */
	static bool decodeLocation(const std::string& expression, uint64_t& address);

	/* What is appended to the object file path to ask for the dump. The words
	   are the ones the reference build passes. */
	static constexpr const char* dumpArguments = " --dwarf --xml";

	/* How deep a structure is followed before its members stop being reported.
	   A structure that contains itself must not expand forever. */
	static constexpr uint32_t maximumNesting = 4;

	/* How many elements of an array are named. An array is a run of the same
	   type, so its elements are found by arithmetic rather than by listing
	   them: only the first element goes into the table, and the rest are
	   answered when a variable asks for one by index. */
	static constexpr uint32_t arrayBaseIndex = 0;

   private:
	/* What is known about an array after the walk, which is everything needed
	   to answer for one of its elements. */
	struct ArrayInfo
	{
		uint32_t firstElementAddress = 0;
		uint32_t elementSize = 0;
		uint32_t count = 0;
		Variable::Type type = Variable::Type::UNKNOWN;
	};

	void setError(const std::string& message);

	/* Adds one variable. An aggregate contributes its members instead of
	   itself, because a structure has no single value to read. */
	void emitVariable(const ofd::DwarfIndex& index, const ofd::Die& die, SymbolMap& out,
					  std::map<std::string, ArrayInfo>& arrays) const;

	void emitMembers(const ofd::DwarfIndex& index, const ofd::Die& aggregate, const std::string& prefix,
					 uint32_t byteAddress, uint32_t depth, SymbolMap& out,
					 std::map<std::string, ArrayInfo>& arrays) const;

	void emitArray(const ofd::DwarfIndex& index, const ofd::Die& array, const std::string& name,
				   uint32_t byteAddress, uint32_t depth, SymbolMap& out,
				   std::map<std::string, ArrayInfo>& arrays) const;

	/* Whether a tag describes something the viewer can read as one number. */
	enum class Kind : uint8_t
	{
		Unknown = 0,
		Scalar = 1,
		Aggregate = 2,
		Array = 3,
		Pointer = 4,
	};

	/* Follows DW_AT_type through typedefs and qualifiers down to the entry that
	   describes the value, and says what that entry is. */
	Kind resolveType(const ofd::DwarfIndex& index, uint64_t offset, Variable::Type& type,
					 uint64_t& byteSizeInUnits, const ofd::Die** resolved) const;

	/* The answer to "what is this name", with the array case expanded. */
	bool resolveName(const std::string& name, Symbol& symbol) const;

   private:
	std::string toolCommand = "ofd2000";

	VariableHandler* variableHandler;
	spdlog::logger* logger;
	mutable std::mutex mtx;
	SymbolMap parsedData;
	mutable std::map<std::string, ArrayInfo> arrays;
	mutable std::string lastErrorMsg;
};

#endif
