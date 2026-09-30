#include "OfdDwarfXml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace
{
	/* ------------------------------------------------------------- XML reader
	 *
	 * A document reader, not a general XML implementation. The documents it is
	 * given are machine generated, hold no namespace declarations, no CDATA and
	 * no processing instructions past the prolog, and are never nested deeply.
	 * What it does have to survive is size: a debug dump of a real program is
	 * tens of megabytes, so the reader walks the text once and copies nothing
	 * that it does not keep.
	 */

	struct Element
	{
		std::string name;
		std::vector<std::pair<std::string, std::string>> attributes;
		std::vector<Element> children;
		std::string text;

		const std::string* attribute(const std::string& key) const
		{
			for (const auto& [name, value] : attributes)
			{
				if (name == key)
					return &value;
			}

			return nullptr;
		}
	};

	class Reader
	{
	   public:
		explicit Reader(const std::string& input) : data(input) {}

		bool parse(Element& root, std::string& error)
		{
			skipSpace();

			std::string prologName;

			while (peek() == '<' && (at(1) == '?' || at(1) == '!'))
			{
				if (at(1) == '?')
				{
					if (!skipUntil("?>", error))
						return false;
				}
				else
				{
					/* A comment, a doctype or a CData section of the prolog. */
					if (compareAt(0, "<!--"))
					{
						if (!skipUntil("-->", error))
							return false;
					}
					else if (!skipUntil(">", error))
						return false;
				}

				skipSpace();
			}

			if (peek() != '<')
			{
				error = "no element found";
				return false;
			}

			return readElement(root, error);
		}

	   private:
		const std::string& data;
		size_t pos = 0;

		bool eof() const { return pos >= data.size(); }
		char peek() const { return eof() ? '\0' : data[pos]; }
		char at(size_t ahead) const { return (pos + ahead) >= data.size() ? '\0' : data[pos + ahead]; }

		bool compareAt(size_t ahead, const char* text) const
		{
			const size_t start = pos + ahead;
			const size_t length = std::strlen(text);

			if (start + length > data.size())
				return false;

			return data.compare(start, length, text) == 0;
		}

		void skipSpace()
		{
			while (!eof() && std::isspace(static_cast<unsigned char>(peek())))
				pos++;
		}

		bool skipUntil(const char* marker, std::string& error)
		{
			const size_t found = data.find(marker, pos);

			if (found == std::string::npos)
			{
				error = std::string("unterminated '") + marker + "'";
				return false;
			}

			pos = found + std::strlen(marker);
			return true;
		}

		bool readName(std::string& out)
		{
			out.clear();

			while (!eof())
			{
				const char c = peek();

				if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.')
				{
					out.push_back(c);
					pos++;
					continue;
				}

				break;
			}

			return !out.empty();
		}

		static void decodeEntities(const std::string& input, std::string& out)
		{
			out.clear();
			out.reserve(input.size());

			for (size_t i = 0; i < input.size(); i++)
			{
				if (input[i] != '&')
				{
					out.push_back(input[i]);
					continue;
				}

				const size_t end = input.find(';', i);

				if (end == std::string::npos)
				{
					out.push_back(input[i]);
					continue;
				}

				const std::string entity = input.substr(i + 1, end - i - 1);

				if (entity == "lt")
					out.push_back('<');
				else if (entity == "gt")
					out.push_back('>');
				else if (entity == "amp")
					out.push_back('&');
				else if (entity == "quot")
					out.push_back('"');
				else if (entity == "apos")
					out.push_back('\'');
				else if (!entity.empty() && entity[0] == '#')
				{
					const bool hexadecimal = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
					const std::string digits = entity.substr(hexadecimal ? 2 : 1);

					if (!digits.empty() &&
						std::all_of(digits.begin(), digits.end(), [hexadecimal](unsigned char c)
									{ return hexadecimal ? std::isxdigit(c) != 0 : std::isdigit(c) != 0; }))
					{
						const long value = std::strtol(digits.c_str(), nullptr, hexadecimal ? 16 : 10);

						if (value > 0 && value < 0x80)
							out.push_back(static_cast<char>(value));
						else
							out.push_back('?');
					}
					else
						out.push_back('?');
				}
				else
					out.push_back('?');

				i = end;
			}
		}

		bool readAttribute(std::string& key, std::string& value, std::string& error)
		{
			if (!readName(key))
			{
				error = "attribute name expected";
				return false;
			}

			skipSpace();

			if (peek() != '=')
			{
				error = "attribute '" + key + "' has no value";
				return false;
			}

			pos++;
			skipSpace();

			const char quote = peek();

			if (quote != '"' && quote != '\'')
			{
				error = "attribute '" + key + "' is not quoted";
				return false;
			}

			pos++;

			const size_t start = pos;
			const size_t end = data.find(quote, start);

			if (end == std::string::npos)
			{
				error = "unterminated value of attribute '" + key + "'";
				return false;
			}

			decodeEntities(data.substr(start, end - start), value);
			pos = end + 1;

			return true;
		}

		bool readElement(Element& out, std::string& error)
		{
			/* The caller has established that the next character is '<'. */
			pos++;

			if (!readName(out.name))
			{
				error = "element name expected";
				return false;
			}

			while (true)
			{
				skipSpace();

				if (eof())
				{
					error = "unterminated element '" + out.name + "'";
					return false;
				}

				if (peek() == '/')
				{
					if (at(1) != '>')
					{
						error = "malformed closing of element '" + out.name + "'";
						return false;
					}

					pos += 2;
					return true;
				}

				if (peek() == '>')
				{
					pos++;
					break;
				}

				std::string key;
				std::string value;

				if (!readAttribute(key, value, error))
					return false;

				out.attributes.emplace_back(key, value);
			}

			std::string text;

			while (true)
			{
				if (eof())
				{
					error = "unterminated element '" + out.name + "'";
					return false;
				}

				if (peek() == '<')
				{
					if (compareAt(0, "<!--"))
					{
						if (!skipUntil("-->", error))
							return false;

						continue;
					}

					if (at(1) == '/')
					{
						pos += 2;

						std::string closing;

						if (!readName(closing))
						{
							error = "closing tag expected";
							return false;
						}

						skipSpace();

						if (peek() != '>')
						{
							error = "malformed closing tag of element '" + out.name + "'";
							return false;
						}

						pos++;

						if (closing != out.name)
						{
							error = "element '" + out.name + "' closed by '" + closing + "'";
							return false;
						}

						std::string decoded;
						decodeEntities(text, decoded);
						out.text += decoded;

						return true;
					}

					out.children.emplace_back();

					if (!readElement(out.children.back(), error))
						return false;

					continue;
				}

				text.push_back(data[pos]);
				pos++;
			}
		}
	};

	/* --------------------------------------------------------- value reading */

	bool looksNumericallyHex(const std::string& text)
	{
		/* Drop the separators a printer may add, then require a shape that is
		   only hexadecimal and that carries at least one letter or a leading
		   zero, which is what a report of an offset looks like. */
		std::string digits;

		for (char c : text)
		{
			if (c == '_' || c == ' ')
				continue;

			digits.push_back(c);
		}

		if (digits.empty())
			return false;

		std::string body = digits;

		if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
			body = body.substr(2);

		if (body.empty())
			return false;

		for (char c : body)
		{
			if (std::isdigit(static_cast<unsigned char>(c)) == 0 &&
				std::isxdigit(static_cast<unsigned char>(c)) == 0)
				return false;
		}

		return true;
	}

	/* Every reading of a field that names an offset. Debug information is
	   conventionally printed in hexadecimal and the tool is not documented to
	   do otherwise, but a decimal number written without a prefix would be read
	   as hexadecimal and point at the wrong DIE, so a value that is also a
	   valid decimal number is registered under both readings. */
	std::vector<uint64_t> parseOffsets(const std::string& text)
	{
		std::vector<uint64_t> values;

		std::string cleaned;

		for (char c : text)
		{
			if (c == '_' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
				continue;

			cleaned.push_back(c);
		}

		if (cleaned.empty() || !looksNumericallyHex(cleaned))
			return values;

		std::string body = cleaned;

		if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
			body = body.substr(2);

		if (body.empty())
			return values;

		const uint64_t asHex = std::strtoull(body.c_str(), nullptr, 16);
		values.push_back(asHex);

		const bool allDecimal = std::all_of(body.begin(), body.end(), [](unsigned char c)
											{ return std::isdigit(c) != 0; });

		if (allDecimal)
		{
			const uint64_t asDecimal = std::strtoull(body.c_str(), nullptr, 10);

			if (asDecimal != asHex)
				values.push_back(asDecimal);
		}

		return values;
	}

	uint64_t parseNumber(const std::string& text, bool& ok)
	{
		std::string cleaned;

		for (char c : text)
		{
			if (c == '_' || c == ' ' || c == '\t')
				continue;

			cleaned.push_back(c);
		}

		ok = false;

		if (cleaned.empty())
			return 0;

		std::string body = cleaned;

		if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
		{
			body = body.substr(2);

			if (body.empty() || !std::all_of(body.begin(), body.end(), [](unsigned char c)
											 { return std::isxdigit(c) != 0; }))
				return 0;

			ok = true;
			return std::strtoull(body.c_str(), nullptr, 16);
		}

		if (!std::all_of(body.begin(), body.end(), [](unsigned char c)
						 { return std::isxdigit(c) != 0; }))
			return 0;

		/* A size or an encoding is printed without a prefix and has to be read
		   in the base it was written in. A value that carries a hexadecimal
		   digit can only be hexadecimal; everything else is decimal. */
		const bool hexadecimal = body.find_first_of("abcdefABCDEF") != std::string::npos;

		ok = true;
		return std::strtoull(body.c_str(), nullptr, hexadecimal ? 16 : 10);
	}

	/* Where an attribute keeps its value. The document puts it in one of
	   several places depending on the form of the attribute, and the forms are
	   not published, so every place is tried in the order that cannot pick up a
	   wrong value: an explicit value, then a reference, then the text. */
	std::string valueOf(const Element& attribute)
	{
		for (const char* key : {"value", "ref", "idref", "offset"})
		{
			if (const std::string* found = attribute.attribute(key); found != nullptr && !found->empty())
				return *found;
		}

		if (!attribute.text.empty())
		{
			const size_t first = attribute.text.find_first_not_of(" \t\r\n");
			const size_t last = attribute.text.find_last_not_of(" \t\r\n");

			if (first != std::string::npos)
				return attribute.text.substr(first, last - first + 1);
		}

		if (!attribute.children.empty())
			return valueOf(attribute.children.front());

		return {};
	}

	/* True when an attribute that carries a flag is present and not set to an
	   explicit false. */
	bool flagValue(const std::string& text)
	{
		if (text.empty())
			return true;

		std::string lowered;

		for (char c : text)
			lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

		return lowered != "0" && lowered != "false" && lowered != "no";
	}

	std::string lowerCase(const std::string& text)
	{
		std::string out = text;

		for (char& c : out)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		return out;
	}

	/* Reads one `attribute` element into a DIE. */
	void applyAttribute(ofd::Die& die, const std::string& name, const std::string& value)
	{
		if (name == "DW_AT_name")
			die.name = value;
		else if (name == "DW_AT_TI_symbol_name")
			die.symbolName = value;
		else if (name == "DW_AT_location")
			die.location = value;
		else if (name == "DW_AT_type")
		{
			const std::vector<uint64_t> offsets = parseOffsets(value);

			if (!offsets.empty())
			{
				die.typeOffset = offsets.front();
				die.hasType = true;
			}
		}
		else if (name == "DW_AT_byte_size")
		{
			bool ok = false;
			const uint64_t number = parseNumber(value, ok);

			if (ok)
			{
				die.byteSize = number;
				die.hasByteSize = true;
			}
		}
		else if (name == "DW_AT_encoding")
		{
			bool ok = false;
			const uint64_t number = parseNumber(value, ok);

			if (ok)
			{
				die.encoding = number;
				die.hasEncoding = true;
			}
		}
		else if (name == "DW_AT_data_member_location")
		{
			bool ok = false;
			const uint64_t number = parseNumber(value, ok);

			if (ok)
			{
				die.dataMemberOffset = number;
				die.hasDataMemberOffset = true;
			}
		}
		else if (name == "DW_AT_upper_bound")
		{
			bool ok = false;
			const uint64_t number = parseNumber(value, ok);

			if (ok)
			{
				die.upperBound = number;
				die.hasUpperBound = true;
			}
		}
		else if (name == "DW_AT_count")
		{
			bool ok = false;
			const uint64_t number = parseNumber(value, ok);

			if (ok)
			{
				die.count = number;
				die.hasCount = true;
			}
		}
		else if (name == "DW_AT_declaration")
			die.isDeclaration = flagValue(value);
		else if (name == "DW_AT_external")
			die.isExternal = flagValue(value);
	}

	std::string attributeOf(const Element& element, const char* key)
	{
		const std::string* found = element.attribute(key);

		return found == nullptr ? std::string() : *found;
	}

	size_t appendDie(const Element& element, std::vector<ofd::Die>& dies, std::map<uint64_t, size_t>& byOffset);

	/* An element that is neither a DIE nor an attribute is a container the tool
	   puts around a group of DIEs. Its DIEs are siblings, not members, so the
	   walk descends into it without linking anything to it. */
	void walkContainer(const Element& element, std::vector<ofd::Die>& dies, std::map<uint64_t, size_t>& byOffset)
	{
		for (const Element& child : element.children)
		{
			const std::string name = lowerCase(child.name);

			if (name == "die")
				appendDie(child, dies, byOffset);
			else if (name != "attribute")
				walkContainer(child, dies, byOffset);
		}
	}

	size_t appendDie(const Element& element, std::vector<ofd::Die>& dies, std::map<uint64_t, size_t>& byOffset)
	{
		ofd::Die die;
		die.tag = attributeOf(element, "tag");

		const std::vector<uint64_t> offsets = parseOffsets(attributeOf(element, "id"));

		if (!offsets.empty())
			die.offset = offsets.front();

		const size_t self = dies.size();
		dies.push_back(die);

		/* Both readings of the offset are registered, for the reason given at
		   parseOffsets. It is done before descending so that a reference to an
		   entry further down the document still resolves. */
		for (const uint64_t offset : offsets)
			byOffset.emplace(offset, self);

		for (const Element& child : element.children)
		{
			const std::string name = lowerCase(child.name);

			if (name == "die")
			{
				const size_t childIndex = appendDie(child, dies, byOffset);
				dies[self].children.push_back(dies[childIndex].offset);
			}
			else if (name == "attribute")
				applyAttribute(dies[self], attributeOf(child, "name"), valueOf(child));
			else
				walkContainer(child, dies, byOffset);
		}

		return self;
	}
}  // namespace

namespace ofd
{
	bool isAggregateTag(const std::string& tag)
	{
		return tag == "DW_TAG_structure_type" || tag == "DW_TAG_union_type" ||
			   tag == "DW_TAG_class_type";
	}

	bool isTransparentTag(const std::string& tag)
	{
		return tag == "DW_TAG_typedef" || tag == "DW_TAG_const_type" ||
			   tag == "DW_TAG_volatile_type" || tag == "DW_TAG_TI_far_type" ||
			   tag == "DW_TAG_restrict_type";
	}

	bool DwarfIndex::load(const std::string& document, const std::string& sourceName, spdlog::logger* logger, std::string& error)
	{
		Element root;

		Reader reader(document);

		if (!reader.parse(root, error))
		{
			if (logger != nullptr)
				logger->error("[OfdDwarfXml] Failed to load/parse XML '{}': {}", sourceName, error);

			return false;
		}

		if (root.name.empty())
		{
			error = "no root element";

			if (logger != nullptr)
				logger->error("[OfdDwarfXml] XML document '{}' has no root element", sourceName);

			return false;
		}

		/* The root of the dump is a container, not a DIE: the entries hang off
		   it, at whatever depth the tool chose to put them. */
		std::vector<Die> collected;
		std::map<uint64_t, size_t> offsets;

		walkContainer(root, collected, offsets);

		dies = std::move(collected);
		byOffset = std::move(offsets);

		if (logger != nullptr)
		{
			logger->info("[OfdDwarfXml] Indexed {} DIEs from ofd2000 XML dump", dies.size());
			logger->debug("[OfdDwarfXml] Parsing complete. Found {} variables.", dies.size());
		}

		return true;
	}

	const Die* DwarfIndex::find(uint64_t offset) const
	{
		const auto it = byOffset.find(offset);

		if (it == byOffset.end() || it->second >= dies.size())
			return nullptr;

		return &dies[it->second];
	}
}  // namespace ofd
