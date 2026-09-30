#ifndef _LIBDWARFPARSER_HPP
#define _LIBDWARFPARSER_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "IElfParser.hpp"
#include "Variable.hpp"
#include "VariableHandler.hpp"
#include "spdlog/spdlog.h"

/*
 * Reading the symbol file in process, straight out of the DWARF sections.
 *
 * A GDB child process answers one name per round trip and has to be started for
 * every file, which is the cost that shows up when the addresses of a few
 * hundred variables are refreshed. This parser reads the same information
 * without leaving the process, so a refresh is bounded by the size of the file.
 *
 * What it reads is only what the viewer can use:
 *
 *   - a global variable is a `DW_TAG_variable` reached from the compilation unit
 *     or from a namespace, never from inside a subprogram, because a local has
 *     no address until the program runs;
 *   - its address comes from `DW_AT_location`, and only the two operations that
 *     carry an address directly are accepted. A variable whose location is a
 *     register or a frame offset has no fixed address;
 *   - its type comes from `DW_AT_type`, followed through typedefs and qualifiers
 *     to a base type, which gives a size and a signedness.
 *
 * Two deliberate limits keep the result the same shape as the GDB parser, so
 * that switching parsers does not silently change which variables exist: a
 * pointer is not reported, and an aggregate is reported as one entry per field
 * rather than as one entry.
 *
 * The two pieces of the work that are pure arithmetic - decoding a location
 * expression and mapping an encoding onto a type - live in their own
 * translation unit and are covered by tests. The DIE walk needs a real file and
 * is exercised through the parser.
 */
class LibDwarfParser : public IElfParser
{
   public:
	LibDwarfParser(VariableHandler* variableHandler, spdlog::logger* logger);

	Type getType() const override;
	std::string getName() const override;
	std::string getDescription() const override;
	bool checkAvailability(std::string& reason) override;

	bool parse(const std::string& elfPath) override;
	bool updateVariableMap(const std::string& elfPath) override;
	SymbolMap getParsedData() const override;
	std::string getLastErrorMsg() const override;

	/* Whether this build links the library. When it does not, every call fails
	   with a reason instead of the build failing. */
	static bool isCompiledIn();

	/* What a location expression turned out to be. */
	enum class LocationForm : uint8_t
	{
		/* Nothing to read: no attribute, or an empty expression. */
		Empty = 0,
		/* DW_OP_addr: the address is the operand. */
		Address = 1,
		/* DW_OP_addrx: the operand indexes .debug_addr, which needs the DIE. */
		AddressIndex = 2,
		/* Any other operation, none of which yields a fixed address. */
		Unsupported = 3,
	};

	struct Location
	{
		LocationForm form = LocationForm::Empty;
		uint64_t value = 0;
	};

	/* Reads the leading operation of a DWARF location expression. The two forms
	   that carry an address directly are the only ones answered with one;
	   everything else is reported as unsupported, which the caller turns into a
	   skipped variable.

	   The operand of the address operation is read least significant byte
	   first, which is the order every target this reader is used with states
	   its addresses in. A big-endian object would have to have its order passed
	   in; reading one with this function produces a byte reversed address, which
	   is valid memory holding the wrong value. */
	static Location decodeLocation(const std::vector<uint8_t>& expression, uint32_t addressSize);

	/* Maps a `DW_AT_encoding` and a `DW_AT_byte_size` onto the type the viewer
	   can read. Sizes the viewer has no representation for come back UNKNOWN,
	   which the caller treats as "not a variable worth reporting". */
	static Variable::Type typeFromEncoding(uint64_t encoding, uint64_t byteSize);

	/* How deep a structure is followed before its members stop being reported.
	   Without a limit a type that contains itself would not terminate. */
	static constexpr uint32_t maximumNesting = 4;

   private:
	void setError(const std::string& message);

	/* Looks a name up in the map this parser filled. Takes the lock, so it is
	   not called while one is held. */
	bool lookup(const std::string& name, Symbol& symbol);

   private:
	VariableHandler* variableHandler;
	spdlog::logger* logger;
	mutable std::mutex mtx;
	SymbolMap parsedData;
	std::string lastErrorMsg;

	/* The file the map above was read from. Remembered so that a refresh of the
	   same file does not read it again, and a different file is read at once. */
	std::string parsedPath;
};

#endif
