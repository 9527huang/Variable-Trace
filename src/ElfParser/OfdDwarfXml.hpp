#ifndef _OFDDWARFXML_HPP
#define _OFDDWARFXML_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Variable.hpp"
#include "spdlog/spdlog.h"

/*
 * Reading the debug information TI's `ofd2000` prints for a C2000 object file.
 *
 * The tool has two output formats. The text one is a list of lines where a DIE
 * is a line carrying an offset and an indentation level, and its attributes are
 * the lines that follow, with no indication of where one DIE ends and the next
 * begins other than the levels. The XML one is a document: every DIE is an
 * element naming its own offset and tag, and every attribute is a child element
 * naming the attribute and carrying its value. This code reads the XML one,
 * because the structure it needs - "the DIE at this offset" and "the children
 * of this DIE" - is the structure the document already has.
 *
 * What comes out is an index rather than a tree: every DIE is stored once, in
 * document order, and a DIE that refers to another does so by offset. Type
 * resolution is a chain of such references - a variable points at a typedef,
 * which points at the real type - and following offsets is what makes a type
 * that contains a pointer to itself terminate instead of expanding forever.
 *
 * Two details of the document are handled leniently, because neither is
 * published and the tool is not present on every machine to check against:
 *
 *   - an offset is read as hexadecimal, which is how debug information is
 *     always printed, but a value that could also be a decimal number is
 *     indexed under both readings, so a reference finds its DIE either way;
 *   - an attribute value is taken from whichever place carries one: a `value`
 *     attribute, a `ref` attribute, or the text of a named child element.
 *
 * Neither looseness can invent a variable: a DIE still has to name a global,
 * carry an address and resolve to a type the viewer can read, or it is dropped.
 */

namespace ofd
{
	/* One debugging information entry, with the attributes the viewer uses. */
	struct Die
	{
		uint64_t offset = 0;
		std::string tag;

		/* The two names a TI object file can carry. The C name is the one the
		   user writes; the symbol name is the one the linker emits, with a
		   leading underscore, and is kept only to fall back on. */
		std::string name;
		std::string symbolName;

		/* Raw text of DW_AT_location, which is an operation list. */
		std::string location;

		uint64_t typeOffset = 0;
		uint64_t byteSize = 0;
		uint64_t encoding = 0;
		uint64_t dataMemberOffset = 0;
		uint64_t upperBound = 0;
		uint64_t count = 0;

		bool hasType = false;
		bool hasByteSize = false;
		bool hasEncoding = false;
		bool hasDataMemberOffset = false;
		bool hasUpperBound = false;
		bool hasCount = false;
		bool isDeclaration = false;
		bool isExternal = false;

		/* DIEs nested directly inside this one, in document order. */
		std::vector<uint64_t> children;
	};

	class DwarfIndex
	{
	   public:
		/* Builds the index from the document. Returns false, with a reason,
		   when the document cannot be read as XML at all. An index with no
		   DIEs in it is not an error: the file can simply have no debugging
		   information. `sourceName` is what the messages call the document,
		   which is the path of the file it was dumped to. */
		bool load(const std::string& document, const std::string& sourceName, spdlog::logger* logger, std::string& error);

		const Die* find(uint64_t offset) const;

		const std::vector<Die>& all() const { return dies; }

		size_t size() const { return dies.size(); }

	   private:
		std::vector<Die> dies;
		std::map<uint64_t, size_t> byOffset;
	};

	/* Whether a tag names one of the aggregates whose members are reported one
	   by one rather than as a single entry. An enumeration is not one of them:
	   its members name values, they are not fields to read. */
	bool isAggregateTag(const std::string& tag);

	/* Whether a tag is one of the qualifiers and wrappers that hide the real
	   type of a variable: a typedef, a `const`, a `volatile`, and TI's own
	   `far` type. */
	bool isTransparentTag(const std::string& tag);
}  // namespace ofd

#endif
