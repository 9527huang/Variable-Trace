#include "LibDwarfParser.hpp"

#include <filesystem>
#include <set>

#if defined(VARIABLE_TRACE_HAS_LIBDWARF)

#include <dwarf.h>
#include <libdwarf.h>

namespace
{
	/* What a `DW_AT_type` reference leads to, after the typedefs and the
	   qualifiers in front of it have been followed. */
	struct ResolvedType
	{
		enum class Kind : uint8_t
		{
			/* No representation in the viewer, or not a type at all. */
			Unusable = 0,
			/* A number the viewer can read, with its type. */
			Scalar = 1,
			/* A structure or a union, whose fields are walked separately. */
			Aggregate = 2,
		};

		Kind kind = Kind::Unusable;
		Variable::Type type = Variable::Type::UNKNOWN;

		/* Where the aggregate DIE sits, so its members can be read later. */
		Dwarf_Off offset = 0;
	};

	/*
	 * One pass over one object file.
	 *
	 * The walk is kept apart from the parser object because everything it needs -
	 * the library handle, the address size, the map it fills - is per file and
	 * lasts no longer than the parse.
	 *
	 * The library allocates a DIE, an attribute or an error object per call and
	 * hands the ownership over, so every one of them is released here. Strings
	 * are the exception: they point into the file's own string section and must
	 * not be freed.
	 */
	class DwarfWalk
	{
	   public:
		DwarfWalk(Dwarf_Debug debug, uint32_t addressSize, IElfParser::SymbolMap& symbols, spdlog::logger* logger) : debug_(debug), addressSize_(addressSize), symbols_(symbols), logger_(logger)
		{
		}

		/* The count is reported by the caller as one summary line, so that a
		   file that holds nothing usable still says why. */
		uint32_t getSkippedCount() const
		{
			return skipped_;
		}

		void walkCompilationUnit(Dwarf_Die unitDie)
		{
			walkChildren(unitDie, true, 0);
		}

	   private:
		void walkChildren(Dwarf_Die parent, bool globalScope, uint32_t depth)
		{
			Dwarf_Die child = nullptr;
			Dwarf_Error error = nullptr;
			int result = dwarf_child(parent, &child, &error);

			if (result == DW_DLV_ERROR)
			{
				release(error, "reading the children of a DIE");
				return;
			}

			while (result == DW_DLV_OK)
			{
				visit(child, globalScope, depth);

				Dwarf_Die sibling = nullptr;
				result = dwarf_siblingof_b(debug_, child, true, &sibling, &error);

				if (result == DW_DLV_ERROR)
				{
					release(error, "reading the sibling of a DIE");
					dwarf_dealloc_die(child);
					return;
				}

				dwarf_dealloc_die(child);
				child = sibling;
			}
		}

		void visit(Dwarf_Die die, bool globalScope, uint32_t depth)
		{
			Dwarf_Half tag = 0;

			if (!tagOf(die, tag))
				return;

			switch (tag)
			{
				case DW_TAG_namespace:
					/* A namespace does not change what is reachable: what it
					   holds is still a global. */
					walkChildren(die, globalScope, depth);
					return;

				case DW_TAG_variable:
					if (!globalScope)
					{
						/* A local has no address until the program runs, so
						   there is nothing for the viewer to read. */
						logger_->debug("[DieProcessor] Skipped DW_TAG_variable");
						skipped_++;
						return;
					}

					emitVariable(die);
					return;

				case DW_TAG_subprogram:
					/* Everything below a subprogram is a local. The subtree is
					   not walked at all: it holds no name the viewer can use,
					   and on a large file it is most of the DIEs. */
					return;

				default:
					/* The type DIEs that hang off the unit are reached through
					   `DW_AT_type` of the variable that uses them, where the
					   name of the owner is still known. Walking them here would
					   report a member without the structure it belongs to. */
					return;
			}
		}

		void emitVariable(Dwarf_Die die)
		{
			const std::string name = nameOf(die);

			if (name.empty())
			{
				logger_->debug("[DieProcessor] no DW_AT_name and no DW_AT_specification");
				skipped_++;
				return;
			}

			const LibDwarfParser::Location location = locationOf(die);

			if (location.form == LibDwarfParser::LocationForm::Empty)
			{
				logger_->debug("[DieProcessor] no DW_AT_location attribute for {}", name);
				skipped_++;
				return;
			}

			if (location.form == LibDwarfParser::LocationForm::Unsupported)
			{
				logger_->debug("[DieProcessor] could not resolve address from DW_AT_location for {} (no DW_OP_addr/DW_OP_addrx)", name);
				skipped_++;
				return;
			}

			uint64_t address = location.value;

			if (location.form == LibDwarfParser::LocationForm::AddressIndex)
			{
				/* Only the DIE can turn the index into an address, because the
				   base of .debug_addr differs per compilation unit. */
				Dwarf_Addr resolved = 0;
				Dwarf_Error error = nullptr;
				const int result = dwarf_debug_addr_index_to_addr(die, location.value, &resolved, &error);

				if (result != DW_DLV_OK)
				{
					release(error, "resolving a .debug_addr index");
					logger_->debug("[DieProcessor] could not resolve address from DW_AT_location for {} (no DW_OP_addr/DW_OP_addrx)", name);
					skipped_++;
					return;
				}

				address = resolved;
			}

			const ResolvedType type = typeOf(die);

			if (type.kind == ResolvedType::Kind::Unusable)
			{
				skipped_++;
				return;
			}

			if (type.kind == ResolvedType::Kind::Scalar)
			{
				symbols_[name] = IElfParser::Symbol{static_cast<uint32_t>(address), type.type};
				return;
			}

			emitMembers(name, type, static_cast<uint32_t>(address), 0);
		}

		void emitMembers(const std::string& owner, const ResolvedType& aggregate, uint32_t baseAddress, uint32_t depth)
		{
			Dwarf_Die die = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_offdie_b(debug_, aggregate.offset, true, &die, &error) != DW_DLV_OK)
			{
				release(error, "reading the DIE of an aggregate type");
				return;
			}

			Dwarf_Die child = nullptr;
			int result = dwarf_child(die, &child, &error);

			if (result == DW_DLV_ERROR)
				release(error, "reading the members of an aggregate type");

			while (result == DW_DLV_OK)
			{
				Dwarf_Half tag = 0;

				if (tagOf(child, tag) && tag == DW_TAG_member)
					emitMember(owner, child, baseAddress, depth);
				else
					logger_->debug("[TypeInfo] Skipped DIE");

				Dwarf_Die sibling = nullptr;
				result = dwarf_siblingof_b(debug_, child, true, &sibling, &error);

				if (result == DW_DLV_ERROR)
				{
					release(error, "reading the sibling of a member DIE");
					dwarf_dealloc_die(child);
					break;
				}

				dwarf_dealloc_die(child);
				child = sibling;
			}

			dwarf_dealloc_die(die);
		}

		void emitMember(const std::string& owner, Dwarf_Die die, uint32_t baseAddress, uint32_t depth)
		{
			uint64_t bitSize = 0;

			if (udataOf(die, DW_AT_bit_size, bitSize))
			{
				/* A bitfield shares its storage with its neighbours, so it has
				   no address of its own to read. */
				logger_->debug("[TypeInfo] Skipped DIE");
				skipped_++;
				return;
			}

			const std::string memberName = nameOf(die);

			if (memberName.empty())
			{
				skipped_++;
				return;
			}

			/* Every member of a union sits at the start of it, and the compiler
			   leaves the attribute out there instead of writing a zero. */
			uint64_t offset = 0;
			udataOf(die, DW_AT_data_member_location, offset);

			uint32_t memberAddress = 0;

			/* An offset that does not fit an address is not an offset. */
			if (offset <= 0xffffffffu && baseAddress <= 0xffffffffu - static_cast<uint32_t>(offset))
				memberAddress = baseAddress + static_cast<uint32_t>(offset);
			else
			{
				logger_->debug("[TypeInfo] Skipped DIE");
				skipped_++;
				return;
			}

			const std::string fullName = owner + "." + memberName;

			/* A member is reached exactly as a top level variable is: a field of
			   a structure is its own name with its owner in front of it. */
			const ResolvedType memberType = typeOf(die);

			if (memberType.kind == ResolvedType::Kind::Scalar)
			{
				symbols_[fullName] = IElfParser::Symbol{memberAddress, memberType.type};
				return;
			}

			if (memberType.kind == ResolvedType::Kind::Aggregate && depth + 1 < LibDwarfParser::maximumNesting)
			{
				emitMembers(fullName, memberType, memberAddress, depth + 1);
				return;
			}

			skipped_++;
		}

		ResolvedType typeOf(Dwarf_Die die)
		{
			Dwarf_Attribute attribute = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_attr(die, DW_AT_type, &attribute, &error) != DW_DLV_OK)
			{
				release(error, "reading DW_AT_type");
				logger_->debug("[TypeInfo] DW_AT_type attribute missing (undefined type or void)");
				return {};
			}

			Dwarf_Off offset = 0;
			Dwarf_Bool isInfo = 0;
			const int result = dwarf_global_formref_b(attribute, &offset, &isInfo, &error);
			dwarf_dealloc(debug_, attribute, DW_DLA_ATTR);

			if (result != DW_DLV_OK)
			{
				release(error, "reading the offset in DW_AT_type");
				logger_->debug("[TypeInfo] cannot determine form of DW_AT_type attribute");
				return {};
			}

			return resolveType(offset, isInfo);
		}

		ResolvedType resolveType(Dwarf_Off offset, Dwarf_Bool isInfo)
		{
			const auto remembered = resolved_.find(offset);

			if (remembered != resolved_.end())
				return remembered->second;

			/* A type that is reached again while it is still being worked out
			   contains itself. Walking it would not end. */
			if (!inProgress_.insert(offset).second)
			{
				logger_->debug("[TypeInfo] Skipped DIE");
				return {};
			}

			ResolvedType answer;

			Dwarf_Die die = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_offdie_b(debug_, offset, isInfo, &die, &error) != DW_DLV_OK)
				release(error, "reading the DIE of a type");
			else
			{
				Dwarf_Half tag = 0;

				if (tagOf(die, tag))
					answer = classifyType(die, tag);

				dwarf_dealloc_die(die);
			}

			inProgress_.erase(offset);
			resolved_[offset] = answer;

			return answer;
		}

		ResolvedType classifyType(Dwarf_Die die, Dwarf_Half tag)
		{
			switch (tag)
			{
				case DW_TAG_base_type:
				{
					uint64_t byteSize = 0;
					uint64_t encoding = 0;

					if (!udataOf(die, DW_AT_byte_size, byteSize) || !udataOf(die, DW_AT_encoding, encoding))
					{
						logger_->debug("[TypeInfo] Skipped DIE");
						return {};
					}

					return scalarOf(encoding, byteSize);
				}

				case DW_TAG_enumeration_type:
				{
					/* An enumeration is read as the integer behind it, which is
					   what the GDB parser reports for one as well. */
					uint64_t byteSize = 0;

					if (!udataOf(die, DW_AT_byte_size, byteSize))
						byteSize = 4;

					return scalarOf(DW_ATE_signed, byteSize);
				}

				case DW_TAG_typedef:
				case DW_TAG_const_type:
				case DW_TAG_volatile_type:
				case DW_TAG_restrict_type:
					/* None of these has a size of its own; each points at the
					   type it qualifies. */
					return typeOf(die);

				case DW_TAG_structure_type:
				case DW_TAG_union_type:
				case DW_TAG_class_type:
				{
					Dwarf_Off offset = 0;
					Dwarf_Error error = nullptr;

					if (dwarf_dieoffset(die, &offset, &error) != DW_DLV_OK)
					{
						release(error, "reading the offset of an aggregate DIE");
						return {};
					}

					ResolvedType answer;
					answer.kind = ResolvedType::Kind::Aggregate;
					answer.offset = offset;
					return answer;
				}

				default:
					/* A pointer, a reference, an array and everything else: the
					   viewer reads one number at one address, and the GDB parser
					   reports none of these either. */
					logger_->debug("[TypeInfo] Skipped DIE");
					return {};
			}
		}

		ResolvedType scalarOf(uint64_t encoding, uint64_t byteSize)
		{
			const Variable::Type type = IElfParser::typeFromEncoding(encoding, byteSize);

			if (type == Variable::Type::UNKNOWN)
			{
				logger_->debug("[TypeInfo] Skipped DIE");
				return {};
			}

			ResolvedType answer;
			answer.kind = ResolvedType::Kind::Scalar;
			answer.type = type;
			return answer;
		}

		LibDwarfParser::Location locationOf(Dwarf_Die die)
		{
			Dwarf_Attribute attribute = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_attr(die, DW_AT_location, &attribute, &error) != DW_DLV_OK)
			{
				release(error, "reading DW_AT_location");
				return {};
			}

			std::vector<uint8_t> expression;
			const bool usable = expressionOf(attribute, expression);
			dwarf_dealloc(debug_, attribute, DW_DLA_ATTR);

			if (!usable)
			{
				/* The attribute is there, but what it holds describes where to
				   find the value at run time rather than where it lives. */
				LibDwarfParser::Location refused;
				refused.form = LibDwarfParser::LocationForm::Unsupported;
				return refused;
			}

			return LibDwarfParser::decodeLocation(expression, addressSize_);
		}

		bool expressionOf(Dwarf_Attribute attribute, std::vector<uint8_t>& expression)
		{
			Dwarf_Unsigned length = 0;
			Dwarf_Ptr data = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_formexprloc(attribute, &length, &data, &error) == DW_DLV_OK)
			{
				const uint8_t* bytes = static_cast<const uint8_t*>(data);
				expression.assign(bytes, bytes + length);
				return true;
			}

			release(error, "reading a location expression");

			Dwarf_Block* block = nullptr;

			if (dwarf_formblock(attribute, &block, &error) != DW_DLV_OK)
			{
				release(error, "reading a location block");
				return false;
			}

			if (block->bl_from_loclist)
			{
				/* The bytes are a location list rather than an expression, so
				   the address is only known once the program runs. */
				dwarf_dealloc(debug_, block, DW_DLA_BLOCK);
				return false;
			}

			const uint8_t* bytes = static_cast<const uint8_t*>(block->bl_data);
			expression.assign(bytes, bytes + block->bl_len);
			dwarf_dealloc(debug_, block, DW_DLA_BLOCK);

			return true;
		}

		std::string nameOf(Dwarf_Die die)
		{
			std::string name = directNameOf(die);

			if (!name.empty())
				return name;

			/* A definition the compiler kept apart from its declaration carries
			   no name of its own; the declaration it points at has it. */
			Dwarf_Attribute attribute = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_attr(die, DW_AT_specification, &attribute, &error) != DW_DLV_OK)
			{
				release(error, "reading DW_AT_specification");
				return name;
			}

			Dwarf_Off offset = 0;
			Dwarf_Bool isInfo = 0;
			const int result = dwarf_global_formref_b(attribute, &offset, &isInfo, &error);
			dwarf_dealloc(debug_, attribute, DW_DLA_ATTR);

			if (result != DW_DLV_OK)
			{
				release(error, "reading the offset in DW_AT_specification");
				logger_->debug("[DieProcessor] DW_AT_specification offset unresolvable");
				return name;
			}

			Dwarf_Die specification = nullptr;

			if (dwarf_offdie_b(debug_, offset, isInfo, &specification, &error) != DW_DLV_OK)
			{
				release(error, "reading the DIE DW_AT_specification points at");
				return name;
			}

			name = directNameOf(specification);

			if (name.empty())
				logger_->debug("[DieProcessor] spec DIE has no DW_AT_name");

			dwarf_dealloc_die(specification);

			return name;
		}

		std::string directNameOf(Dwarf_Die die)
		{
			Dwarf_Attribute attribute = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_attr(die, DW_AT_name, &attribute, &error) != DW_DLV_OK)
			{
				release(error, "reading DW_AT_name");
				logger_->debug("[DieProcessor] dwarf_attr(DW_AT_name) failed");
				return "";
			}

			char* text = nullptr;
			const int result = dwarf_formstring(attribute, &text, &error);
			dwarf_dealloc(debug_, attribute, DW_DLA_ATTR);

			if (result != DW_DLV_OK || text == nullptr)
			{
				release(error, "reading the text of DW_AT_name");
				return "";
			}

			/* The text belongs to the library and outlives the DIE, so it is
			   copied rather than freed. */
			return std::string(text);
		}

		bool tagOf(Dwarf_Die die, Dwarf_Half& tag)
		{
			Dwarf_Error error = nullptr;

			if (dwarf_tag(die, &tag, &error) != DW_DLV_OK)
			{
				release(error, "failed to read DWARF tag (DW_DLV_ERROR)");
				return false;
			}

			return true;
		}

		bool udataOf(Dwarf_Die die, Dwarf_Half attributeNumber, uint64_t& value)
		{
			Dwarf_Attribute attribute = nullptr;
			Dwarf_Error error = nullptr;

			if (dwarf_attr(die, attributeNumber, &attribute, &error) != DW_DLV_OK)
			{
				release(error, "reading a constant attribute");
				return false;
			}

			Dwarf_Unsigned result = 0;
			const int read = dwarf_formudata(attribute, &result, &error);
			dwarf_dealloc(debug_, attribute, DW_DLA_ATTR);

			if (read != DW_DLV_OK)
			{
				release(error, "reading a constant attribute");
				return false;
			}

			value = result;
			return true;
		}

		/* Reports a failure and gives the error object back. A null error means
		   the call had nothing to report, which is the common case: asking for
		   an attribute a DIE does not carry is answered with NO_ENTRY. */
		void release(Dwarf_Error& error, const std::string& context)
		{
			if (error == nullptr)
				return;

			logger_->debug("[ElfParser] {}: {}", context, dwarf_errmsg(error));
			dwarf_dealloc_error(debug_, error);
			error = nullptr;
		}

	   private:
		Dwarf_Debug debug_;
		uint32_t addressSize_;
		IElfParser::SymbolMap& symbols_;
		spdlog::logger* logger_;
		uint32_t skipped_ = 0;

		/* A type is reached once per variable that uses it, so the answer is
		   kept. This is what makes a file with a few hundred variables cheap to
		   read even though the walk visits the same types over and over. */
		std::map<Dwarf_Off, ResolvedType> resolved_;
		std::set<Dwarf_Off> inProgress_;
	};

	/* Reads one unit header and returns the DIE that carries it, so that the
	   caller can walk its children. */
	bool nextCompilationUnit(Dwarf_Debug debug, Dwarf_Die& unitDie, uint32_t& addressSize, spdlog::logger* logger)
	{
		Dwarf_Unsigned headerLength = 0;
		Dwarf_Half version = 0;
		Dwarf_Off abbrevOffset = 0;
		Dwarf_Half unitAddressSize = 0;
		Dwarf_Half lengthSize = 0;
		Dwarf_Half extensionSize = 0;
		Dwarf_Sig8 signature{};
		Dwarf_Unsigned typeOffset = 0;
		Dwarf_Unsigned nextHeader = 0;
		Dwarf_Half unitType = 0;
		Dwarf_Error error = nullptr;

		const int result = dwarf_next_cu_header_d(debug, true, &headerLength, &version, &abbrevOffset, &unitAddressSize, &lengthSize, &extensionSize, &signature, &typeOffset, &nextHeader, &unitType, &error);

		if (result == DW_DLV_NO_ENTRY)
			return false;

		if (result != DW_DLV_OK)
		{
			if (error != nullptr)
			{
				logger->debug("[ElfParser] reading a unit header: {}", dwarf_errmsg(error));
				dwarf_dealloc_error(debug, error);
			}

			return false;
		}

		Dwarf_Die die = nullptr;

		if (dwarf_siblingof_b(debug, nullptr, true, &die, &error) != DW_DLV_OK)
		{
			if (error != nullptr)
			{
				logger->debug("[ElfParser] reading a unit DIE: {}", dwarf_errmsg(error));
				dwarf_dealloc_error(debug, error);
			}

			return true;
		}

		unitDie = die;
		addressSize = unitAddressSize;

		return true;
	}
}  // namespace

#endif

LibDwarfParser::LibDwarfParser(VariableHandler* variableHandler, spdlog::logger* logger) : variableHandler(variableHandler), logger(logger)
{
}

IElfParser::Type LibDwarfParser::getType() const
{
	return Type::LibDwarf;
}

std::string LibDwarfParser::getName() const
{
	return nameOf(Type::LibDwarf);
}

std::string LibDwarfParser::getDescription() const
{
	return descriptionOf(Type::LibDwarf);
}

void LibDwarfParser::setError(const std::string& message)
{
	std::lock_guard<std::mutex> lock(mtx);
	lastErrorMsg = message;
}

std::string LibDwarfParser::getLastErrorMsg() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return lastErrorMsg;
}

IElfParser::SymbolMap LibDwarfParser::getParsedData() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return parsedData;
}

bool LibDwarfParser::lookup(const std::string& name, Symbol& symbol)
{
	std::lock_guard<std::mutex> lock(mtx);

	const auto found = parsedData.find(name);

	if (found == parsedData.end())
		return false;

	symbol = found->second;
	return true;
}

#if defined(VARIABLE_TRACE_HAS_LIBDWARF)

bool LibDwarfParser::isCompiledIn()
{
	return true;
}

bool LibDwarfParser::checkAvailability(std::string& reason)
{
	(void)reason;
	return true;
}

bool LibDwarfParser::parse(const std::string& elfPath)
{
	if (elfPath.empty())
	{
		setError("[ElfParser] ELF file path has not been set.");
		return false;
	}

	if (!std::filesystem::exists(elfPath))
	{
		setError("The symbol file '" + elfPath + "' does not exist.");
		return false;
	}

	Dwarf_Debug debug = nullptr;
	Dwarf_Error error = nullptr;

	if (dwarf_init_path(elfPath.c_str(), nullptr, 0, DW_GROUPNUMBER_ANY, nullptr, nullptr, &debug, &error) != DW_DLV_OK)
	{
		std::string message = "[ElfParser] Automatic parseElf failed.";

		if (error != nullptr)
		{
			message += " ";
			message += dwarf_errmsg(error);

			/* Deallocating an error is allowed to outlive the handle: the
			   library keeps a separate pool for the errors raised while one was
			   being opened. */
			dwarf_dealloc_error(debug, error);
			error = nullptr;
		}

		logger->error("{}", message);
		setError(message);
		return false;
	}

	logger->debug("[ElfParser] ELF file opened.");
	logger->debug("[ElfParser] DWARF initialized successfully.");

	SymbolMap symbols;
	uint32_t skipped = 0;
	uint32_t units = 0;

	while (true)
	{
		Dwarf_Die unitDie = nullptr;
		uint32_t addressSize = 4;

		if (!nextCompilationUnit(debug, unitDie, addressSize, logger))
			break;

		if (unitDie == nullptr)
			continue;

		DwarfWalk walk(debug, addressSize, symbols, logger);
		walk.walkCompilationUnit(unitDie);
		skipped += walk.getSkippedCount();

		dwarf_dealloc_die(unitDie);
		units++;
	}

	dwarf_finish(debug);
	logger->debug("[ElfParser] DWARF context released.");

	{
		std::lock_guard<std::mutex> lock(mtx);
		parsedData = std::move(symbols);
		parsedPath = elfPath;
		lastErrorMsg.clear();
	}

	logger->debug("[ElfParser] {} variable(s) in {} unit(s), {} DIE(s) skipped", getParsedData().size(), units, skipped);

	return true;
}

bool LibDwarfParser::updateVariableMap(const std::string& elfPath)
{
	bool alreadyRead = false;

	{
		std::lock_guard<std::mutex> lock(mtx);
		alreadyRead = parsedPath == elfPath && !parsedData.empty();
	}

	if (!alreadyRead)
	{
		logger->debug("[ElfParser] variableMap is empty - parseElf has not been called. Calling parseElf automatically.");

		if (!parse(elfPath))
			return false;
	}

	for (std::shared_ptr<Variable> var : *variableHandler)
	{
		if (!var->getShouldUpdateFromElf())
			continue;

		var->setIsFound(false);
		var->setType(Variable::Type::UNKNOWN);

		Symbol symbol;

		if (!lookup(var->getTrackedName(), symbol))
			continue;

		var->setIsFound(true);
		var->setAddress(symbol.address);
		var->setType(symbol.type);
	}

	setError("");

	return true;
}

#else

/* The reason lives in this branch as well as in the other one: the two are
   exclusive, so a single definition outside them would be the only way to share
   it, and an unused constant in the branch that does not need it is worse than
   the repetition. */
namespace
{
	const char* const unavailableReason = "This build was made without libdwarf, so the 'libdwarf' parser is not in it.";
}  // namespace

bool LibDwarfParser::isCompiledIn()
{
	return false;
}

bool LibDwarfParser::checkAvailability(std::string& reason)
{
	reason = unavailableReason;
	return false;
}

bool LibDwarfParser::parse(const std::string& elfPath)
{
	(void)elfPath;
	setError(unavailableReason);
	logger->error("{}", unavailableReason);
	return false;
}

bool LibDwarfParser::updateVariableMap(const std::string& elfPath)
{
	(void)elfPath;
	setError(unavailableReason);
	return false;
}

#endif
