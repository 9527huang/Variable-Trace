#include "ElfParserFactory.hpp"

#include "GdbParser.hpp"
#include "LibDwarfParser.hpp"
#include "TiOfdParser.hpp"

namespace ElfParserFactory
{
	std::shared_ptr<IElfParser> create(IElfParser::Type type, VariableHandler* variableHandler, spdlog::logger* logger)
	{
		switch (type)
		{
			case IElfParser::Type::LibDwarf:
				return std::make_shared<LibDwarfParser>(variableHandler, logger);

			case IElfParser::Type::TiOfd:
				return std::make_shared<TiOfdParser>(variableHandler, logger);

			case IElfParser::Type::Gdb:
			default:
				return std::make_shared<GdbParser>(variableHandler, logger);
		}
	}

	std::shared_ptr<IElfParser> create(const std::string& name, VariableHandler* variableHandler, spdlog::logger* logger)
	{
		IElfParser::Type type = IElfParser::Type::Gdb;

		if (!IElfParser::typeFromName(name, type))
		{
			if (logger != nullptr)
				logger->warn("Unknown ELF parser '{}', falling back to '{}'.", name, IElfParser::nameOf(type));
		}

		return create(type, variableHandler, logger);
	}

	std::vector<IElfParser::Type> availableTypes()
	{
		/* The order the settings window offers them in. The default comes first
		   because it is the one that works for every dialect. */
		return {IElfParser::Type::Gdb, IElfParser::Type::LibDwarf, IElfParser::Type::TiOfd};
	}

	std::string labelOf(IElfParser::Type type)
	{
		/* Short enough for a combo box, where the full sentence would not fit. */
		switch (type)
		{
			case IElfParser::Type::Gdb:
				return "GDB";

			case IElfParser::Type::LibDwarf:
				return "DWARF";

			case IElfParser::Type::TiOfd:
				return "C2000";
		}

		return "GDB";
	}

	void applySettings(IElfParser& parser, const std::string& gdbCommand, const std::string& ofd2000Command)
	{
		if (auto* gdb = dynamic_cast<GdbParser*>(&parser); gdb != nullptr)
			gdb->changeCurrentGDBCommand(gdbCommand);

		if (auto* ofd = dynamic_cast<TiOfdParser*>(&parser); ofd != nullptr)
			ofd->setToolCommand(ofd2000Command);
	}
}  // namespace ElfParserFactory
