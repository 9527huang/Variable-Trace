#include "GdbParser.hpp"

#include <filesystem>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

GdbParser::GdbParser(VariableHandler* variableHandler, spdlog::logger* logger) : variableHandler(variableHandler), logger(logger)
{
	validateGDB();
}

IElfParser::Type GdbParser::getType() const
{
	return Type::Gdb;
}

std::string GdbParser::getName() const
{
	return nameOf(Type::Gdb);
}

std::string GdbParser::getDescription() const
{
	return descriptionOf(Type::Gdb);
}

void GdbParser::changeCurrentGDBCommand(const std::string& command)
{
	currentGDBCommand = command;
}

std::string GdbParser::getCurrentGDBCommand() const
{
	return currentGDBCommand;
}

bool GdbParser::validateGDB()
{
	ProcessHandler process;

	auto output = process.executeCmd(currentGDBCommand + std::string(versionProbe), "GNU gdb");

	if (output.find("GNU") != std::string::npos || output.find("gnu") != std::string::npos)
	{
		logger->info("GDB executable working!");
		setError("");
		return true;
	}

	setError("The program '" + currentGDBCommand + "' did not answer like GDB. Check the GDB command in the acquisition settings.");
	logger->error("GDB executable error! Please check the GDB path in the acqusition settings!");
	return false;
}

bool GdbParser::checkAvailability(std::string& reason)
{
	if (validateGDB())
		return true;

	reason = getLastErrorMsg();
	return false;
}

void GdbParser::setError(const std::string& message)
{
	std::lock_guard<std::mutex> lock(mtx);
	lastErrorMsg = message;
}

std::string GdbParser::getLastErrorMsg() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return lastErrorMsg;
}

bool GdbParser::updateVariableMap(const std::string& elfPath)
{
	if (!validateGDB())
		return false;

	if (!std::filesystem::exists(elfPath))
	{
		setError("The symbol file '" + elfPath + "' does not exist.");
		return false;
	}

	std::string cmd = currentGDBCommand + std::string(" --interpreter=mi ") + elfPath;
	process.executeCmd(cmd, "(gdb)");

	/* The list is copied first, and the shared pointers with it.

	   This runs on a thread of its own while the interface stays usable, so a
	   reader can delete a variable in the middle of it: the collection would
	   then be modified under a loop that is walking it. Copying costs one short
	   hold of the lock and keeps the objects alive for the rest of the pass. */
	std::vector<std::pair<std::shared_ptr<Variable>, std::string>> pending;

	{
		auto modelLock = variableHandler->lockModel();

		for (const std::shared_ptr<Variable>& var : *variableHandler)
		{
			if (var->getShouldUpdateFromElf() == false)
				continue;

			pending.emplace_back(var, var->getTrackedName());
		}
	}

	/* Every answer is collected before any of them is written. Asking gdb costs
	   a process round trip per variable, and the acquisition thread reads these
	   same fields on every sample - a type is a string and an enumeration is a
	   vector, so a write landing between two of its reads is not something it
	   could survive. Holding the lock across the round trips would work as
	   well, and would stop sampling for as long as the refresh takes. */
	std::vector<std::tuple<std::shared_ptr<Variable>, uint32_t, Variable::Type>> found;
	found.reserve(pending.size());

	for (auto& [var, trackedName] : pending)
	{
		auto maybeAddress = checkAddress(trackedName);

		if (!maybeAddress.has_value())
			continue;

		found.emplace_back(var, maybeAddress.value(), checkType(trackedName, nullptr));
	}

	/* Written back together, and under the lock, for the reason above.

	   A variable the reader deleted while this was running is written into as
	   well, which is harmless: the copy taken at the start is the only thing
	   keeping it alive now, and no plot names it any more. */
	{
		auto modelLock = variableHandler->lockModel();

		for (auto& entry : pending)
		{
			entry.first->setIsFound(false);
			entry.first->setType(Variable::Type::UNKNOWN);
		}

		for (auto& [var, address, type] : found)
		{
			var->setIsFound(true);
			var->setAddress(address);
			var->setType(type);
		}
	}

	process.closePipes();
	setError("");

	return true;
}

bool GdbParser::parse(const std::string& elfPath)
{
	if (!validateGDB())
		return false;

	if (!std::filesystem::exists(elfPath))
	{
		setError("The symbol file '" + elfPath + "' does not exist.");
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mtx);
		parsedData.clear();
	}

	std::string cmd = currentGDBCommand + std::string(" --interpreter=mi ") + elfPath;
	process.executeCmd(cmd, "(gdb)");
	auto out = process.executeCmd("info variables\n", "(gdb)");

	size_t start = 0;

	while (out.length() > 0)
	{
		std::string delimiter = "File";

		auto end = out.find(delimiter, start);

		if (end == std::string::npos)
			break;

		/* find tylda sign next */
		start = out.find("~", end);

		if (start == std::string::npos)
			break;

		/* account for tylda and " */
		start += 2;
		/* find the end of filepath */
		end = out.find(":", start);

		if (end == std::string::npos)
			break;

		auto filename = out.substr(start, end - start);

		auto end1 = out.find("~\"\\n", end);
		start = end;

		if (end1 != std::string::npos)
		{
			end = end1;
			parseVariableChunk(out.substr(start, end - start));
		}

		start = end;
	}

	process.closePipes();
	setError("");

	return true;
}

void GdbParser::parseVariableChunk(const std::string& chunk)
{
	size_t start = 0;

	while (1)
	{
		auto semicolonPos = chunk.find(';', start);

		if (semicolonPos == std::string::npos)
			break;

		auto spacePos = chunk.rfind(' ', semicolonPos);

		if (spacePos == std::string::npos)
			break;

		std::string variableName = chunk.substr(spacePos + 1, semicolonPos - spacePos - 1);

		checkVariableType(variableName);
		start = semicolonPos + 1;
	}
}

void GdbParser::checkVariableType(const std::string& name)
{
	auto maybeAddress = checkAddress(name);

	if (!maybeAddress.has_value())
		return;

	std::string out;
	const Variable::Type type = checkType(name, &out);

	if (type != Variable::Type::UNKNOWN)
	{
		std::lock_guard<std::mutex> lock(mtx);
		parsedData[name] = Symbol{maybeAddress.value(), type};
		return;
	}

	/* The name holds a structure rather than a number. Its fields are what the
	   debug link can read, so each of them is asked about in turn. */
	auto subStart = 0;

	while (1)
	{
		auto semicolonPos = out.find(';', subStart);

		logger->debug("POS: {}", subStart);
		logger->debug("SEMICOLON POS: {}", semicolonPos);
		logger->debug("OUT: {}", out);

		if (semicolonPos == std::string::npos)
			break;

		if (out[semicolonPos - 1] == ')')
		{
			subStart = semicolonPos + 1;
			continue;
		}

		auto spacePos = out.rfind(' ', semicolonPos);

		logger->debug("SPACE POS: {}", spacePos);

		if (spacePos == std::string::npos)
			break;

		auto varName = out.substr(spacePos + 1, semicolonPos - spacePos - 1);

		logger->debug("VAR NAME: {}", varName);

		/* if a const method or a pointer */
		if (varName == "const" || varName[0] == '*')
		{
			subStart = semicolonPos + 1;
			continue;
		}

		auto fullName = name + "." + varName;

		logger->debug("FULL NAME: {}", fullName);

		if (fullName.size() < 100)
			checkVariableType(fullName);

		subStart = semicolonPos + 1;
	}
}

Variable::Type GdbParser::checkType(const std::string& name, std::string* output)
{
	auto out = process.executeCmd(std::string("ptype ") + name + std::string("\n"), "(gdb)");

	if (output != nullptr)
		*output = out;

	auto start = out.find("=");

	if (start == std::string::npos || start + 2 > out.size())
		return Variable::Type::UNKNOWN;

	auto end = out.find("\\n", start);

	if (end == std::string::npos || end < start + 2)
		return Variable::Type::UNKNOWN;

	auto line = out.substr(start + 2, end - start - 2);

	logger->debug("CHECKING TYPE FOR LINE: {}", line);

	/* remove const and volatile */
	if (line.find("volatile ", 0) != std::string::npos)
		line.erase(0, 9);

	if (line.find("const ", 0) != std::string::npos)
		line.erase(0, 6);

	if (line.find("static const ", 0) != std::string::npos)
		line.erase(0, 13);

	if (line.find("enum {", 0) != std::string::npos)
		return Variable::Type::I32;

	if (!typeNames.contains(line))
		return Variable::Type::UNKNOWN;

	return typeNames.at(line);
}

std::optional<uint32_t> GdbParser::checkAddress(const std::string& name)
{
	auto out = process.executeCmd(std::string("p /d &") + name + std::string("\n"), "(gdb)");

	size_t dolarSignPos = out.find('$');

	if (dolarSignPos == std::string::npos)
		return std::nullopt;

	auto out2 = out.substr(dolarSignPos + 1);

	size_t equalSignPos = out2.find('=');

	if (equalSignPos == std::string::npos)
		return std::nullopt;

	/* this finds the \n as a string consting of '\' and 'n' not '\n' */
	size_t eol = out2.find("\\n");

	if (eol == std::string::npos || eol < equalSignPos + 2)
		return std::nullopt;

	/* +2 is to skip = and a space */
	auto address = out2.substr(equalSignPos + 2, eol - equalSignPos - 2);

	uint32_t addressValue = 0;

	try
	{
		addressValue = stoi(address);
	}
	catch (...)
	{
		logger->warn("stoi incorect argument: {}", address);
	}

	return addressValue;
}

IElfParser::SymbolMap GdbParser::getParsedData() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return parsedData;
}
