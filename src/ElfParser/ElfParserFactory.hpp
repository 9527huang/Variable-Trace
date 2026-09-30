#ifndef _ELFPARSERFACTORY_HPP
#define _ELFPARSERFACTORY_HPP

#include <memory>
#include <string>
#include <vector>

#include "IElfParser.hpp"
#include "VariableHandler.hpp"
#include "spdlog/spdlog.h"

/*
 * The one place that decides which parser a name means.
 *
 * The setting stores a name, the interface stores an enumerator, and the
 * window, the project file and the API each hold one of the two. Turning any
 * of them into an object in one place is what keeps a name the setting accepts
 * from being a name that cannot be built.
 */
namespace ElfParserFactory
{
	std::shared_ptr<IElfParser> create(IElfParser::Type type, VariableHandler* variableHandler, spdlog::logger* logger);

	/* Falls back to the default parser for a name that no parser answers to, so
	   an unreadable setting cannot leave the application without a parser. */
	std::shared_ptr<IElfParser> create(const std::string& name, VariableHandler* variableHandler, spdlog::logger* logger);

	/* The kinds, in the order the settings window offers them. */
	std::vector<IElfParser::Type> availableTypes();

	/* The labels the settings window shows: what the parser is, not the value
	   the setting stores. */
	std::string labelOf(IElfParser::Type type);

	/* Hands a parser the fields of the settings that belong to it. Each parser
	   runs one program, and which program that is differs; a parser that runs
	   nothing ignores both. Keeping this here means the window never has to
	   know which class it is holding. */
	void applySettings(IElfParser& parser, const std::string& gdbCommand, const std::string& ofd2000Command);
}  // namespace ElfParserFactory

#endif
