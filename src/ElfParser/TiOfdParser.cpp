#include "TiOfdParser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "ProcessHandler.hpp"

namespace
{
	constexpr const char* tagVariable = "DW_TAG_variable";
	constexpr const char* tagMember = "DW_TAG_member";
	constexpr const char* tagBaseType = "DW_TAG_base_type";
	constexpr const char* tagPointerType = "DW_TAG_pointer_type";
	constexpr const char* tagArrayType = "DW_TAG_array_type";
	constexpr const char* tagSubrangeType = "DW_TAG_subrange_type";
	constexpr const char* tagEnumerationType = "DW_TAG_enumeration_type";

	/* The one operation of a location list that states an address outright. Its
	   operand is the address. */
	constexpr const char* opAddress = "DW_OP_addr";

	/* The same operation with an index instead of an address. The index points
	   into a section this walk does not read, so the entry is dropped rather
	   than guessed at. It is spelled out separately because the name of the
	   first operation is a prefix of this one. */
	constexpr const char* opAddressIndexed = "DW_OP_addrx";

	bool isNameCharacter(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
	}

	/* True when `token` sits at `position` and ends there: the first operation
	   name is a prefix of the second, so a match that runs into another name
	   character is not a match. */
	bool tokenAt(const std::string& text, size_t position, const char* token, size_t& end)
	{
		const size_t length = std::strlen(token);

		if (position + length > text.size())
			return false;

		if (text.compare(position, length, token) != 0)
			return false;

		const size_t after = position + length;

		if (after < text.size() && isNameCharacter(text[after]))
			return false;

		end = after;
		return true;
	}

	bool containsToken(const std::string& text, const char* token)
	{
		size_t end = 0;

		for (size_t at = 0; at < text.size(); at++)
		{
			if (tokenAt(text, at, token, end))
				return true;
		}

		return false;
	}

	size_t findToken(const std::string& text, const char* token)
	{
		size_t end = 0;

		for (size_t at = 0; at < text.size(); at++)
		{
			if (tokenAt(text, at, token, end))
				return at;
		}

		return std::string::npos;
	}

	/* Reads the number that follows an operation. An address in a location list
	   is printed in hexadecimal, with or without the prefix, which is the one
	   place in this format where a bare number is not decimal. */
	bool readAddress(const std::string& text, size_t from, uint64_t& value)
	{
		size_t at = from;

		while (at < text.size() && (std::isspace(static_cast<unsigned char>(text[at])) != 0 ||
									text[at] == '|' || text[at] == ':' || text[at] == ','))
			at++;

		if (at + 2 <= text.size() && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X'))
			at += 2;

		const size_t start = at;

		while (at < text.size() && std::isxdigit(static_cast<unsigned char>(text[at])) != 0)
			at++;

		if (at == start)
			return false;

		value = std::strtoull(text.substr(start, at - start).c_str(), nullptr, 16);

		return true;
	}

	/* Splits a name such as `buffer[12]` into the array it belongs to and the
	   index asked for. */
	bool splitIndex(const std::string& name, std::string& base, uint32_t& index)
	{
		const size_t open = name.rfind('[');

		if (open == std::string::npos || name.empty() || name.back() != ']')
			return false;

		const std::string digits = name.substr(open + 1, name.size() - open - 2);

		if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c)
										   { return std::isdigit(c) != 0; }))
			return false;

		base = name.substr(0, open);
		index = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 10));

		return true;
	}

	/* Whether a name the operating system can be asked to run exists. A command
	   with a separator in it is a path and is asked directly; a bare name is
	   looked for in every directory of PATH, which is what the operating system
	   would do. Windows appends the executable suffix, and both separators are
	   accepted there because a path can be written either way. */
	bool findOnPath(const std::string& command)
	{
		if (command.empty())
			return false;

		const std::filesystem::path asPath(command);

		if (asPath.has_parent_path())
			return std::filesystem::exists(asPath);

		const char* pathVariable = std::getenv("PATH");

		if (pathVariable == nullptr)
			return false;

		std::string path = pathVariable;

		/* A Windows PATH separates with a semicolon, a Unix one with a colon. */
#ifdef _WIN32
		constexpr char separator = ';';
#else
		constexpr char separator = ':';
#endif

		std::vector<std::string> directories;
		size_t start = 0;

		while (start <= path.size())
		{
			const size_t found = path.find(separator, start);
			directories.push_back(path.substr(start, found == std::string::npos ? std::string::npos : found - start));

			if (found == std::string::npos)
				break;

			start = found + 1;
		}

		/* A Windows path may be written with either separator, and the name may
		   have been given without the executable suffix. */
		for (std::string& directory : directories)
			std::replace(directory.begin(), directory.end(), '/', '\\');

		for (const std::string& directory : directories)
		{
			if (directory.empty())
				continue;

			const std::filesystem::path candidate = std::filesystem::path(directory) / command;

			std::error_code error;

			if (std::filesystem::exists(candidate, error))
				return true;

#ifdef _WIN32
			if (!candidate.has_extension())
			{
				std::filesystem::path withSuffix = candidate;
				withSuffix += ".exe";

				if (std::filesystem::exists(withSuffix, error))
					return true;
			}
#endif
		}

		return false;
	}
}  // namespace

TiOfdParser::TiOfdParser(VariableHandler* variableHandler, spdlog::logger* logger) : variableHandler(variableHandler), logger(logger)
{
	setError("");
}

IElfParser::Type TiOfdParser::getType() const
{
	return Type::TiOfd;
}

std::string TiOfdParser::getName() const
{
	return nameOf(Type::TiOfd);
}

std::string TiOfdParser::getDescription() const
{
	return descriptionOf(Type::TiOfd);
}

void TiOfdParser::setToolCommand(const std::string& command)
{
	std::lock_guard<std::mutex> lock(mtx);
	toolCommand = command;
}

std::string TiOfdParser::getToolCommand() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return toolCommand;
}

void TiOfdParser::setError(const std::string& message)
{
	std::lock_guard<std::mutex> lock(mtx);
	lastErrorMsg = message;
}

std::string TiOfdParser::getLastErrorMsg() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return lastErrorMsg;
}

bool TiOfdParser::toolExists(const std::string& command)
{
	return findOnPath(command);
}

bool TiOfdParser::checkAvailability(std::string& reason)
{
	const std::string command = getToolCommand();

	if (!toolExists(command))
	{
		setError("NOT FOUND. Please set the ofd2000 path manually");

		if (logger != nullptr)
			logger->error("[TiOfdElfParser] ofd2000 not found at '{}'", command);

		reason = getLastErrorMsg();
		return false;
	}

	setError("");
	return true;
}

uint32_t TiOfdParser::toByteAddress(uint64_t address)
{
	/* An address unit is one 16 bit word. A quantity that is already measured in
	   address units - a size, a member offset - goes through the same
	   multiplication, because the file states it in the same unit. */
	return static_cast<uint32_t>(address * bytesPerAddressUnit);
}

bool TiOfdParser::decodeLocation(const std::string& expression, uint64_t& address)
{
	if (containsToken(expression, opAddressIndexed))
		return false;

	const size_t position = findToken(expression, opAddress);

	if (position == std::string::npos)
		return false;

	const size_t after = position + std::strlen(opAddress);

	return readAddress(expression, after, address);
}

bool TiOfdParser::runDump(const std::string& elfPath, std::string& xml, std::string& error) const
{
	if (elfPath.empty() || !std::filesystem::exists(elfPath))
	{
		error = "ELF path not set or does not exist: '" + elfPath + "'";
		return false;
	}

	const std::string command = getToolCommand();

	if (!toolExists(command))
	{
		error = "NOT FOUND. Please set the ofd2000 path manually";
		return false;
	}

	ProcessHandler process;

	/* The path is quoted here, unlike the command the GDB parser builds, so an
	   object file under a directory with a space in its name is read rather
	   than split into arguments. */
	const std::string commandLine = command + " \"" + elfPath + "\"" + dumpArguments;

	xml = process.executeCmdToEnd(commandLine);

	if (xml.find_first_not_of(" \t\r\n") == std::string::npos)
	{
		error = "ofd2000 produced no (or empty) output for '" + elfPath + "'";
		return false;
	}

	return true;
}

void TiOfdParser::parseDump(const std::string& xml, const std::string& sourceName)
{
	ofd::DwarfIndex index;
	std::string error;

	if (!index.load(xml, sourceName, logger, error))
		return;

	SymbolMap symbols;
	std::map<std::string, ArrayInfo> found;

	for (const ofd::Die& die : index.all())
	{
		if (die.tag == tagVariable)
			emitVariable(index, die, symbols, found);
	}

	const size_t count = symbols.size();

	{
		std::lock_guard<std::mutex> lock(mtx);
		parsedData = std::move(symbols);
		arrays = std::move(found);
	}

	if (logger != nullptr)
		logger->info("[TiOfdElfParser] Parsing complete. Found {} variables.", count);
}

bool TiOfdParser::parse(const std::string& elfPath)
{
	std::string xml;
	std::string error;

	if (!runDump(elfPath, xml, error))
	{
		setError(error);

		if (logger != nullptr)
			logger->error("[TiOfdElfParser] {}", error);

		return false;
	}

	parseDump(xml, elfPath);

	setError("");

	return true;
}

bool TiOfdParser::updateVariableMap(const std::string& elfPath)
{
	if (variableHandler == nullptr)
		return false;

	{
		std::lock_guard<std::mutex> lock(mtx);

		if (parsedData.empty() && logger != nullptr)
			logger->info("[TiOfdElfParser] variableMap is empty - calling parseElf() automatically.");
	}

	if (getParsedData().empty() && !parse(elfPath))
		return false;

	for (const std::shared_ptr<Variable>& variable : *variableHandler)
	{
		if (variable->getShouldUpdateFromElf() == false)
			continue;

		variable->setIsFound(false);
		variable->setType(Variable::Type::UNKNOWN);

		Symbol symbol;

		if (!resolveName(variable->getTrackedName(), symbol))
			continue;

		variable->setIsFound(true);
		variable->setAddress(symbol.address);
		variable->setType(symbol.type);
	}

	return true;
}

IElfParser::SymbolMap TiOfdParser::getParsedData() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return parsedData;
}

bool TiOfdParser::resolveName(const std::string& name, Symbol& symbol) const
{
	std::lock_guard<std::mutex> lock(mtx);

	const auto exact = parsedData.find(name);

	if (exact != parsedData.end())
	{
		symbol = exact->second;
		return true;
	}

	std::string base;
	uint32_t index = 0;

	if (!splitIndex(name, base, index))
	{
		if (logger != nullptr)
			logger->debug("[TiOfdElfParser] Cannot parse array index '{}' in name: {}", name, name);

		return false;
	}

	const auto info = arrays.find(base + "[0]");

	if (info == arrays.end())
	{
		if (logger != nullptr)
			logger->debug("[TiOfdElfParser] Array element not found: {} (zero-key: {})", name, base + "[0]");

		return false;
	}

	if (index >= info->second.count)
	{
		if (logger != nullptr)
			logger->warn("[TiOfdElfParser] Malformed array index in name: {}", name);

		return false;
	}

	symbol.address = info->second.firstElementAddress + index * info->second.elementSize;
	symbol.type = info->second.type;

	return true;
}

TiOfdParser::Kind TiOfdParser::resolveType(const ofd::DwarfIndex& index, uint64_t offset, Variable::Type& type,
										   uint64_t& byteSizeInUnits, const ofd::Die** resolved) const
{
	const ofd::Die* die = index.find(offset);

	/* A chain of typedefs and qualifiers is followed to its end. The count
	   bounds it: a file that states a type as its own definition would
	   otherwise be followed forever. */
	uint32_t guard = 0;

	while (die != nullptr && ofd::isTransparentTag(die->tag) && guard++ < 32)
	{
		if (!die->hasType)
			return Kind::Unknown;

		die = index.find(die->typeOffset);
	}

	if (die == nullptr)
		return Kind::Unknown;

	if (resolved != nullptr)
		*resolved = die;

	byteSizeInUnits = die->hasByteSize ? die->byteSize : 0;

	if (die->tag == tagBaseType)
	{
		/* The size is in address units, and the viewer counts bytes, so it is
		   converted before the encoding is asked what it describes: a two unit
		   integer is a 32 bit number on this family, not a 16 bit one. */
		type = IElfParser::typeFromEncoding(die->hasEncoding ? die->encoding : 0,
											toByteAddress(byteSizeInUnits));

		return type == Variable::Type::UNKNOWN ? Kind::Unknown : Kind::Scalar;
	}

	if (die->tag == tagEnumerationType)
	{
		/* An enumeration is stored as the integer it is built on. The members
		   are the names of the values, not fields of the variable. */
		type = IElfParser::typeFromEncoding(0x05 /* signed */, toByteAddress(byteSizeInUnits));

		return type == Variable::Type::UNKNOWN ? Kind::Unknown : Kind::Scalar;
	}

	if (die->tag == tagArrayType)
		return Kind::Array;

	if (die->tag == tagPointerType)
		return Kind::Pointer;

	if (ofd::isAggregateTag(die->tag))
		return Kind::Aggregate;

	return Kind::Unknown;
}

void TiOfdParser::emitVariable(const ofd::DwarfIndex& index, const ofd::Die& die, SymbolMap& out,
							   std::map<std::string, ArrayInfo>& arrays) const
{
	/* An entry that only declares a name describes a variable defined in
	   another translation unit, and the address of that one is not in this
	   file. */
	if (die.isDeclaration)
		return;

	if (die.name.empty() || die.location.empty())
		return;

	uint64_t addressInUnits = 0;

	if (!decodeLocation(die.location, addressInUnits))
		return;

	if (addressInUnits == 0 || !die.hasType)
		return;

	const uint32_t byteAddress = toByteAddress(addressInUnits);

	Variable::Type type = Variable::Type::UNKNOWN;
	uint64_t byteSizeInUnits = 0;
	const ofd::Die* resolved = nullptr;

	switch (resolveType(index, die.typeOffset, type, byteSizeInUnits, &resolved))
	{
		case Kind::Scalar:
			out[die.name] = Symbol{byteAddress, type};
			break;

		case Kind::Aggregate:
			if (resolved != nullptr)
				emitMembers(index, *resolved, die.name, byteAddress, 0, out, arrays);
			break;

		case Kind::Array:
			if (resolved != nullptr)
				emitArray(index, *resolved, die.name, byteAddress, 0, out, arrays);
			break;

		default:
			/* A pointer holds an address, which the viewer has nothing to draw,
			   and an entry of an unknown kind holds nothing it can read. */
			break;
	}
}

void TiOfdParser::emitMembers(const ofd::DwarfIndex& index, const ofd::Die& aggregate, const std::string& prefix,
							  uint32_t byteAddress, uint32_t depth, SymbolMap& out,
							  std::map<std::string, ArrayInfo>& arrays) const
{
	if (depth >= maximumNesting)
		return;

	for (const uint64_t childOffset : aggregate.children)
	{
		const ofd::Die* member = index.find(childOffset);

		if (member == nullptr || member->tag != tagMember)
			continue;

		if (member->name.empty() || !member->hasType)
			continue;

		const uint32_t memberOffset = member->hasDataMemberOffset ? toByteAddress(member->dataMemberOffset) : 0;
		const std::string memberName = prefix + "." + member->name;
		const uint32_t memberAddress = byteAddress + memberOffset;

		Variable::Type type = Variable::Type::UNKNOWN;
		uint64_t byteSizeInUnits = 0;
		const ofd::Die* resolved = nullptr;

		switch (resolveType(index, member->typeOffset, type, byteSizeInUnits, &resolved))
		{
			case Kind::Scalar:
				out[memberName] = Symbol{memberAddress, type};
				break;

			case Kind::Aggregate:
				if (resolved != nullptr)
					emitMembers(index, *resolved, memberName, memberAddress, depth + 1, out, arrays);
				break;

			case Kind::Array:
				if (resolved != nullptr)
					emitArray(index, *resolved, memberName, memberAddress, depth + 1, out, arrays);
				break;

			default:
				break;
		}
	}
}

void TiOfdParser::emitArray(const ofd::DwarfIndex& index, const ofd::Die& array, const std::string& name,
							uint32_t byteAddress, uint32_t depth, SymbolMap& out,
							std::map<std::string, ArrayInfo>& arrays) const
{
	if (depth >= maximumNesting || !array.hasType)
		return;

	uint32_t count = 0;

	for (const uint64_t childOffset : array.children)
	{
		const ofd::Die* subrange = index.find(childOffset);

		if (subrange == nullptr || subrange->tag != tagSubrangeType)
			continue;

		if (subrange->hasCount)
			count = static_cast<uint32_t>(subrange->count);
		else if (subrange->hasUpperBound)
			count = static_cast<uint32_t>(subrange->upperBound + 1);

		break;
	}

	if (count == 0)
		return;

	Variable::Type elementType = Variable::Type::UNKNOWN;
	uint64_t elementSizeInUnits = 0;
	const ofd::Die* resolved = nullptr;

	if (resolveType(index, array.typeOffset, elementType, elementSizeInUnits, &resolved) != Kind::Scalar)
	{
		/* An array of structures would have to be expanded element by element,
		   and the viewer reads one number per name. */
		if (logger != nullptr)
			logger->debug("[TiOfdElfParser] Could not determine elementSize for '{}' (index {}). Address offset may be wrong.", name, arrayBaseIndex);

		return;
	}

	const uint32_t elementSize = toByteAddress(elementSizeInUnits);

	if (elementSize == 0)
	{
		if (logger != nullptr)
			logger->warn("[TiOfdElfParser] Could not determine elementSize for '{}' (index {}). Address offset may be wrong.", name, arrayBaseIndex);

		return;
	}

	/* Only the first element is named in the table. The rest are answered by
	   arithmetic when a variable asks for one of them, which keeps a buffer of
	   a few thousand elements from filling the table with names nobody asked
	   for. */
	const std::string zeroKey = name + "[" + std::to_string(arrayBaseIndex) + "]";

	out[zeroKey] = Symbol{byteAddress, elementType};

	arrays[zeroKey] = ArrayInfo{byteAddress, elementSize, count, elementType};
}
