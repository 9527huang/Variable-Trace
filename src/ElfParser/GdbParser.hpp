#ifndef _GDBPARSER_HPP
#define _GDBPARSER_HPP

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "IElfParser.hpp"
#include "ProcessHandler.hpp"
#include "Variable.hpp"
#include "VariableHandler.hpp"
#include "spdlog/spdlog.h"

/*
 * Reading the symbol file through a GDB child process.
 *
 * GDB is driven in its machine interface mode and answers three questions per
 * name: where the symbol lives (`p /d &name`), what type it has (`ptype name`),
 * and, for a name that turns out to be a structure, which members it has. The
 * member names are then asked about in turn, so a `struct` reaches the variable
 * table as one entry per field rather than as one opaque blob.
 *
 * This is the parser that has to be right for every dialect, which is why it is
 * the default even though a child process per file is the slowest way to get the
 * answers.
 */
class GdbParser : public IElfParser
{
   public:
	GdbParser(VariableHandler* variableHandler, spdlog::logger* logger);

	Type getType() const override;
	std::string getName() const override;
	std::string getDescription() const override;
	bool checkAvailability(std::string& reason) override;

	bool parse(const std::string& elfPath) override;
	bool updateVariableMap(const std::string& elfPath) override;
	SymbolMap getParsedData() const override;
	std::string getLastErrorMsg() const override;

	/* Runs the configured program with `-v` and reports whether the answer
	   looks like GDB. Kept public because the startup path checks it once to
	   tell the user early. */
	bool validateGDB();

	void changeCurrentGDBCommand(const std::string& command);
	std::string getCurrentGDBCommand() const;

	/* What `-v` has to contain for the program to be accepted as GDB. */
	static constexpr const char* versionProbe = " -v";

   private:
	/* The parser runs on a worker thread while the interface reads the error
	   text, so every write of it goes through this and takes the lock. */
	void setError(const std::string& message);

	void parseVariableChunk(const std::string& chunk);
	void checkVariableType(const std::string& name);
	Variable::Type checkType(const std::string& name, std::string* output);
	std::optional<uint32_t> checkAddress(const std::string& name);

   private:
	std::string currentGDBCommand = "gdb";
	VariableHandler* variableHandler;
	spdlog::logger* logger;
	mutable std::mutex mtx;
	SymbolMap parsedData;
	mutable std::string lastErrorMsg;
	ProcessHandler process;

	std::unordered_map<std::string, Variable::Type> typeNames = {
		{"_Bool", Variable::Type::U8},
		{"bool", Variable::Type::U8},
		{"unsigned char", Variable::Type::U8},
		{"unsigned 8-bit", Variable::Type::U8},

		{"char", Variable::Type::I8},
		{"signed char", Variable::Type::I8},
		{"signed 8-bit", Variable::Type::I8},

		{"unsigned short", Variable::Type::U16},
		{"unsigned 16-bit", Variable::Type::U16},
		{"unsigned short int", Variable::Type::U16},
		{"short unsigned int", Variable::Type::U16},

		{"short", Variable::Type::I16},
		{"short int", Variable::Type::I16},
		{"signed short", Variable::Type::I16},
		{"signed 16-bit", Variable::Type::I16},
		{"signed short int", Variable::Type::I16},
		{"short signed int", Variable::Type::I16},

		{"unsigned int", Variable::Type::U32},
		{"unsigned long", Variable::Type::U32},
		{"unsigned 32-bit", Variable::Type::U32},
		{"unsigned long int", Variable::Type::U32},
		{"long unsigned int", Variable::Type::U32},

		{"int", Variable::Type::I32},
		{"long", Variable::Type::I32},
		{"long int", Variable::Type::I32},
		{"signed int", Variable::Type::I32},
		{"signed long", Variable::Type::I32},
		{"signed 32-bit", Variable::Type::I32},
		{"signed long int", Variable::Type::I32},
		{"long signed int", Variable::Type::I32},

		{"float", Variable::Type::F32},
	};
};

#endif
