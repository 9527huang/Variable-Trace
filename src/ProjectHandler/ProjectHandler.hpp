#ifndef _PROJECTHANDLER_HPP
#define _PROJECTHANDLER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "VariableHandler.hpp"
#include "nlohmann/json.hpp"
#include "spdlog/spdlog.h"

/*
 * The project file is a JSON document with the extension .mcvproj.
 *
 * The structs below describe the file format, not the runtime objects. Keeping
 * them separate means the format can stay stable while the handlers evolve, and
 * it keeps this module free of any dependency on the probe implementations,
 * which makes the round trip testable without hardware.
 */

struct ProjectProbeSettings
{
	uint32_t type = 0;
	std::string serialNumber = "";
	std::string targetName = "";
	uint32_t mode = 0;
	uint32_t speedKHz = 10000;
	/* Serial probe only, and the port name travels in serialNumber. */
	uint32_t baudrate = 115200;
};

struct ProjectViewerSettings
{
	uint32_t sampleFrequencyHz = 100;
	uint32_t maxPoints = 10000;
	uint32_t maxViewportPoints = 5000;
	bool refreshAddressesOnElfChange = false;
	bool stopAcquisitionOnElfChange = false;
	bool loggingEnabled = false;
	std::string logDirectory = "";
	std::string gdbCommand = "gdb";
	/* Which program reads the symbol file, and where the C2000 one lives. Both
	   travel with the project because they describe how to read this project's
	   symbol file, which is a property of the project and not of the machine. */
	std::string elfParser = "gdb";
	std::string ofd2000Command = "ofd2000";
	ProjectProbeSettings probe{};
};

struct ProjectTraceSettings
{
	uint32_t coreFrequency = 160000;
	uint32_t tracePrescaler = 10;
	uint32_t maxPoints = 10000;
	uint32_t maxViewportPointsPercent = 10;
	int32_t triggerChannel = -1;
	double triggerLevel = 0.9;
	bool shouldReset = false;
	uint32_t timeout = 2;
	bool loggingEnabled = false;
	std::string logDirectory = "";
	ProjectProbeSettings probe{};
};

/* Trace channel plots live in a separate handler, so they travel as plain data. */
struct ProjectTracePlot
{
	std::string name = "";
	std::string alias = "";
	bool visibility = true;
	uint8_t domain = 0;
	uint8_t varType = 6;
	uint32_t color = 0xFFFFFFFF;
};

/* A write plan is a list of (time, value) steps, which the planner window edits
   and draws as a staircase. It names the variable rather than pointing at it,
   so that the plans still mean something after a symbol file has been imported
   again and every variable object has been rebuilt. */
struct ProjectWriteStep
{
	double time = 0.0;
	double value = 0.0;
};

struct ProjectWritePlan
{
	std::string name = "";
	std::string variable = "";
	std::vector<ProjectWriteStep> steps{};
};

struct ProjectData
{
	std::string elfPath = "";
	ProjectViewerSettings viewer{};
	ProjectTraceSettings trace{};
	std::vector<ProjectTracePlot> tracePlots{};
	std::vector<ProjectWritePlan> writePlans{};
};

class ProjectHandler
{
   public:
	/* Raised whenever a section was added that an older build would drop without
	   saying so. Version 2 added the enum labels of a variable and the parent of
	   a group, version 3 the write plans, version 4 the axis labels of a plot. */
	static constexpr uint32_t formatVersion = 4;
	static constexpr const char* fileExtension = "mcvproj";
	static constexpr const char* legacyFileExtension = "cfg";
	static constexpr const char* applicationName = "Variable-Trace";

	enum class OpenResult
	{
		Ok = 0,
		CannotOpenFile,
		ParseError,
		NewerFormatVersion,
	};

	ProjectHandler(VariableHandler* variableHandler, PlotHandler* plotHandler, PlotGroupHandler* plotGroupHandler, spdlog::logger* logger);

	/* Variables, plots and groups are replaced by the content of the file.
	   tracePlots is filled but not applied, the caller owns the trace handlers. */
	bool save(const std::string& path, const ProjectData& data);
	OpenResult open(const std::string& path, ProjectData& data);

	/* True when the in-memory state differs from the last saved or loaded one. */
	bool isSavingRequired(const ProjectData& data) const;

	/* Forgets the saved baseline, used when a new empty project is started. */
	void reset();

	/* Cheap test used to tell a JSON project from the legacy INI config. */
	static bool isJsonProjectFile(const std::string& path);

	static std::string resultToString(OpenResult result);

   private:
	std::string serialize(const ProjectData& data) const;
	bool deserialize(const nlohmann::json& root, ProjectData& data);

	void clearModel();
	void deserializeVariables(const nlohmann::json& variables);
	void deserializePlots(const nlohmann::json& plots);
	void deserializeGroups(const nlohmann::json& groups);
	void deserializeWritePlans(const nlohmann::json& plans, ProjectData& data);

	nlohmann::json serializeVariable(const std::shared_ptr<Variable>& var) const;
	nlohmann::json serializePlot(const std::shared_ptr<Plot>& plt) const;

   private:
	VariableHandler* variableHandler;
	PlotHandler* plotHandler;
	PlotGroupHandler* plotGroupHandler;
	spdlog::logger* logger;

	std::string lastSavedContent = "";
};

#endif
