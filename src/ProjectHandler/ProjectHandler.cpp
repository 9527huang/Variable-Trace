#include "ProjectHandler.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using nlohmann::json;

namespace
{
	const std::map<Plot::displayFormat, std::string> displayFormatNames{{Plot::displayFormat::DEC, "dec"},
																		{Plot::displayFormat::HEX, "hex"},
																		{Plot::displayFormat::BIN, "bin"}};

	const std::map<std::string, Plot::displayFormat> displayFormatFromName{{"dec", Plot::displayFormat::DEC},
																		   {"hex", Plot::displayFormat::HEX},
																		   {"bin", Plot::displayFormat::BIN}};

	std::string displayFormatToName(Plot::displayFormat format)
	{
		auto entry = displayFormatNames.find(format);
		return entry == displayFormatNames.end() ? "dec" : entry->second;
	}

	Plot::displayFormat displayFormatFromString(const std::string& name)
	{
		auto entry = displayFormatFromName.find(name);
		return entry == displayFormatFromName.end() ? Plot::displayFormat::DEC : entry->second;
	}

	ProjectProbeSettings deserializeProbe(const json& probe)
	{
		ProjectProbeSettings settings{};

		if (!probe.is_object())
			return settings;

		settings.type = probe.value("type", settings.type);
		settings.serialNumber = probe.value("serialNumber", settings.serialNumber);
		settings.targetName = probe.value("targetName", settings.targetName);
		settings.mode = probe.value("mode", settings.mode);
		settings.speedKHz = probe.value("speedKHz", settings.speedKHz);
		settings.baudrate = probe.value("baudrate", settings.baudrate);

		return settings;
	}

	json serializeProbe(const ProjectProbeSettings& probe)
	{
		return json::object({{"type", probe.type},
							 {"serialNumber", probe.serialNumber},
							 {"targetName", probe.targetName},
							 {"mode", probe.mode},
							 {"speedKHz", probe.speedKHz},
							 {"baudrate", probe.baudrate}});
	}
}  // namespace

ProjectHandler::ProjectHandler(VariableHandler* variableHandler, PlotHandler* plotHandler, PlotGroupHandler* plotGroupHandler, spdlog::logger* logger)
	: variableHandler(variableHandler), plotHandler(plotHandler), plotGroupHandler(plotGroupHandler), logger(logger)
{
}

bool ProjectHandler::isJsonProjectFile(const std::string& path)
{
	std::ifstream file(path);

	if (!file.is_open())
		return false;

	char firstCharacter = 0;

	while (file.get(firstCharacter))
	{
		if (!std::isspace(static_cast<unsigned char>(firstCharacter)))
			return firstCharacter == '{';
	}

	return false;
}

std::string ProjectHandler::resultToString(OpenResult result)
{
	switch (result)
	{
		case OpenResult::Ok:
			return "ok";
		case OpenResult::CannotOpenFile:
			return "cannot open file";
		case OpenResult::ParseError:
			return "not a valid project file";
		case OpenResult::NewerFormatVersion:
			return "created by a newer version";
	}

	return "unknown error";
}

void ProjectHandler::reset()
{
	lastSavedContent = "";
}

void ProjectHandler::clearModel()
{
	variableHandler->clear();
	plotHandler->removeAllPlots();
	plotGroupHandler->removeAllGroups();
}

json ProjectHandler::serializeVariable(const std::shared_ptr<Variable>& var) const
{
	const Variable::Color& color = var->getColor();

	json entry;
	entry["name"] = var->getName();
	entry["trackedName"] = var->getTrackedName();
	entry["address"] = var->getAddress();
	entry["type"] = static_cast<uint8_t>(var->getType());
	entry["color"] = json::object({{"r", color.r}, {"g", color.g}, {"b", color.b}, {"a", color.a}});
	entry["shouldUpdateFromElf"] = var->getShouldUpdateFromElf();
	entry["shift"] = var->getShift();
	entry["mask"] = var->getMask();
	entry["highLevelType"] = static_cast<uint8_t>(var->getHighLevelType());
	entry["writeLimits"] = json::object({{"enabled", var->getWriteLimitsEnabled()},
										 {"min", var->getWriteMin()},
										 {"max", var->getWriteMax()}});

	/* An enumeration interpretation carries its own names for values, so the
	   list has to travel with the variable or the labels would be lost on the
	   next load. */
	if (var->isEnum())
	{
		json labels = json::array();

		for (const Variable::EnumLabel& label : var->getEnumLabels())
			labels.push_back(json::object({{"label", label.label}, {"value", label.value}}));

		entry["enumLabels"] = labels;
	}

	if (var->isFractional())
	{
		const Variable::Fractional fractional = var->getFractional();
		entry["fractional"] = json::object({{"bits", fractional.fractionalBits},
											{"base", fractional.base},
											{"baseVariable", fractional.baseVariable != nullptr ? fractional.baseVariable->getName() : ""}});
	}

	return entry;
}

json ProjectHandler::serializePlot(const std::shared_ptr<Plot>& plt) const
{
	json entry;
	entry["name"] = plt->getName();
	entry["type"] = static_cast<uint8_t>(plt->getType());

	/* Written even when empty, which is the usual state: the key being there
	   is what tells a reader that the plot has labels of its own rather than
	   ones this build forgot to save. */
	entry["xAxisLabel"] = plt->getXAxisLabel();
	entry["yAxisLabel"] = plt->getYAxisLabel();

	/* Where the cursors sit is part of what the user set up, so it is saved
	   with the plot rather than left to be found again by hand. The mode is
	   stored as a number: the names are for the combo box, and a project that
	   reads back an unknown number falls back to the first mode. */
	entry["cursorsVisible"] = plt->getCursorsVisible();
	entry["cursorMode"] = Plot::cursorModeToIndex(plt->getCursorMode());
	entry["statisticsVisible"] = plt->getStatisticsVisible();
	entry["cursorValues"] = json::object({{"x0", plt->markerX0.getValue()},
										  {"x1", plt->markerX1.getValue()},
										  {"y0", plt->markerY0.getValue()},
										  {"y1", plt->markerY1.getValue()}});

	if (plt->getType() == Plot::Type::XY)
		entry["xAxisVariable"] = plt->getXAxisVariable() != nullptr ? plt->getXAxisVariable()->getName() : "";

	json series = json::array();

	for (const auto& [name, ser] : plt->getSeriesMap())
	{
		series.push_back(json::object({{"name", ser->var->getName()},
									   {"visibility", ser->visible},
									   {"format", displayFormatToName(ser->format)}}));
	}

	entry["series"] = series;

	return entry;
}

std::string ProjectHandler::serialize(const ProjectData& data) const
{
	json root;

	root["formatVersion"] = formatVersion;
	root["application"] = applicationName;
	root["elf"] = json::object({{"path", data.elfPath}, {"relativeToProject", true}});

	root["acquisition"] = json::object(
		{{"sampleFrequencyHz", data.viewer.sampleFrequencyHz},
		 {"maxPoints", data.viewer.maxPoints},
		 {"maxViewportPoints", data.viewer.maxViewportPoints},
		 {"refreshAddressesOnElfChange", data.viewer.refreshAddressesOnElfChange},
		 {"stopAcquisitionOnElfChange", data.viewer.stopAcquisitionOnElfChange},
		 {"logging", json::object({{"enabled", data.viewer.loggingEnabled}, {"directory", data.viewer.logDirectory}})},
		 {"gdbCommand", data.viewer.gdbCommand},
		 {"elfParser", data.viewer.elfParser},
		 {"ofd2000Command", data.viewer.ofd2000Command},
		 {"probe", serializeProbe(data.viewer.probe)}});

	root["trace"] = json::object(
		{{"coreFrequency", data.trace.coreFrequency},
		 {"tracePrescaler", data.trace.tracePrescaler},
		 {"maxPoints", data.trace.maxPoints},
		 {"maxViewportPointsPercent", data.trace.maxViewportPointsPercent},
		 {"triggerChannel", data.trace.triggerChannel},
		 {"triggerLevel", data.trace.triggerLevel},
		 {"shouldReset", data.trace.shouldReset},
		 {"timeout", data.trace.timeout},
		 {"logging", json::object({{"enabled", data.trace.loggingEnabled}, {"directory", data.trace.logDirectory}})},
		 {"probe", serializeProbe(data.trace.probe)}});

	json tracePlots = json::array();

	for (const ProjectTracePlot& plot : data.tracePlots)
	{
		tracePlots.push_back(json::object({{"name", plot.name},
										   {"alias", plot.alias},
										   {"visibility", plot.visibility},
										   {"domain", plot.domain},
										   {"varType", plot.varType},
										   {"color", plot.color}}));
	}

	root["trace"]["plots"] = tracePlots;

	json variables = json::array();

	for (const std::shared_ptr<Variable>& var : *variableHandler)
		variables.push_back(serializeVariable(var));

	root["variables"] = variables;

	json plots = json::array();

	for (const std::shared_ptr<Plot>& plt : *plotHandler)
		plots.push_back(serializePlot(plt));

	root["plots"] = plots;

	json groups = json::array();

	for (const auto& [name, group] : *plotGroupHandler)
	{
		json groupEntry;
		groupEntry["name"] = group->getName();
		/* A group the recorder works on holds a fixed variable set, so which kind
		   it is has to survive a save and a load. */
		groupEntry["type"] = group->getTypeName();
		/* Empty for a top-level group. The name rather than an index, so the
		   entry stays readable and a group can be moved by editing the file. */
		groupEntry["parent"] = group->getParentName();

		json groupPlots = json::array();

		for (const auto& [plotName, plotEntry] : *group)
			groupPlots.push_back(json::object({{"name", plotEntry.plot->getName()}, {"visibility", plotEntry.visibility}}));

		groupEntry["plots"] = groupPlots;
		groups.push_back(groupEntry);
	}

	root["groups"] = groups;
	root["activeGroup"] = plotGroupHandler->getActiveGroupName();

	json writePlans = json::array();

	for (const ProjectWritePlan& plan : data.writePlans)
	{
		json steps = json::array();

		for (const ProjectWriteStep& step : plan.steps)
			steps.push_back(json::object({{"time", step.time}, {"value", step.value}}));

		writePlans.push_back(json::object({{"name", plan.name}, {"variable", plan.variable}, {"steps", steps}}));
	}

	root["writePlans"] = writePlans;

	return root.dump(2);
}

bool ProjectHandler::save(const std::string& path, const ProjectData& data)
{
	if (path.empty())
	{
		logger->error("Project save requested without a file path");
		return false;
	}

	std::error_code errorCode;
	std::filesystem::path filePath(path);

	if (!filePath.parent_path().empty())
		std::filesystem::create_directories(filePath.parent_path(), errorCode);

	const std::string content = serialize(data);

	/* Write to a temporary file first so a crash mid-write cannot truncate an
	   existing project. */
	const std::string temporaryPath = path + ".tmp";

	{
		std::ofstream file(temporaryPath, std::ios::trunc);

		if (!file.is_open())
		{
			logger->error("Failed to open project for writing: {}", temporaryPath);
			return false;
		}

		file << content;

		if (!file.good())
		{
			logger->error("Failed to write project: {}", temporaryPath);
			return false;
		}
	}

	std::filesystem::rename(temporaryPath, filePath, errorCode);

	if (errorCode)
	{
		/* Some filesystems refuse to replace an existing target, retry once
		   after removing it. */
		std::error_code removeError;
		std::filesystem::remove(filePath, removeError);
		errorCode.clear();
		std::filesystem::rename(temporaryPath, filePath, errorCode);
	}

	if (errorCode)
	{
		logger->error("Failed to replace project file {}: {}", path, errorCode.message());
		std::error_code cleanupError;
		std::filesystem::remove(temporaryPath, cleanupError);
		return false;
	}

	lastSavedContent = content;
	logger->info("Saved project: {}", path);

	return true;
}

void ProjectHandler::deserializeVariables(const json& variables)
{
	/* Fractional variables reference a base by name, which only resolves once
	   every variable is in place. */
	std::vector<std::pair<std::string, std::string>> pendingBaseVariables;

	for (const auto& entry : variables)
	{
		if (!entry.is_object())
			continue;

		const std::string name = entry.value("name", "");

		if (name.empty())
			continue;

		auto var = std::make_shared<Variable>(name);

		std::string trackedName = entry.value("trackedName", name);

		if (trackedName.empty())
			trackedName = name;

		var->setTrackedName(trackedName);
		var->setIsTrackedNameDifferent(trackedName != name);

		var->setAddress(entry.value("address", 0));
		var->setType(static_cast<Variable::Type>(entry.value("type", static_cast<uint8_t>(Variable::Type::UNKNOWN))));
		var->setShouldUpdateFromElf(entry.value("shouldUpdateFromElf", true));
		var->setShift(entry.value("shift", 0));

		uint32_t mask = entry.value("mask", 0xFFFFFFFFu);

		if (mask == 0)
			mask = 0xFFFFFFFF;

		var->setMask(mask);
		var->setHighLevelType(static_cast<Variable::HighLevelType>(entry.value("highLevelType", static_cast<uint8_t>(Variable::HighLevelType::NONE))));

		if (entry.contains("writeLimits") && entry.at("writeLimits").is_object())
		{
			const auto& limits = entry.at("writeLimits");
			var->setWriteLimits(limits.value("enabled", false), limits.value("min", 0.0), limits.value("max", 0.0));
		}

		if (entry.contains("enumLabels") && entry.at("enumLabels").is_array())
		{
			std::vector<Variable::EnumLabel> labels;

			for (const auto& label : entry.at("enumLabels"))
			{
				if (!label.is_object())
					continue;

				const std::string text = label.value("label", "");

				/* A label without text would be impossible to show and could not be
				   told apart from the next nameless entry, so it is dropped. */
				if (text.empty())
					continue;

				labels.push_back({text, label.value("value", static_cast<int64_t>(0))});
			}

			var->setEnumLabels(labels);
		}

		if (entry.contains("color") && entry.at("color").is_object())
		{
			const auto& color = entry.at("color");
			var->setColor(color.value("r", 1.0f), color.value("g", 1.0f), color.value("b", 1.0f), color.value("a", 1.0f));
		}

		if (entry.contains("fractional") && entry.at("fractional").is_object())
		{
			const auto& fractional = entry.at("fractional");

			Variable::Fractional fraction;
			fraction.fractionalBits = fractional.value("bits", 15u);
			fraction.base = fractional.value("base", 1.0);
			fraction.baseVariable = nullptr;

			var->setFractional(fraction);
			pendingBaseVariables.emplace_back(name, fractional.value("baseVariable", ""));
		}

		variableHandler->addVariable(var);
		var->setIsFound(true);

		logger->info("Adding variable: {}", name);
	}

	for (const auto& [variableName, baseVariableName] : pendingBaseVariables)
	{
		if (!variableHandler->contains(variableName))
			continue;

		Variable* baseVariable = nullptr;

		if (variableHandler->contains(baseVariableName))
			baseVariable = variableHandler->getVariable(baseVariableName).get();
		else
			logger->error("Fractional variable {} has no base variable {}", variableName, baseVariableName);

		auto variable = variableHandler->getVariable(variableName);
		Variable::Fractional fractional = variable->getFractional();
		fractional.baseVariable = baseVariable;
		variable->setFractional(fractional);
	}
}

void ProjectHandler::deserializePlots(const json& plots)
{
	for (const auto& entry : plots)
	{
		if (!entry.is_object())
			continue;

		const std::string name = entry.value("name", "");

		if (name.empty())
			continue;

		auto plot = plotHandler->addPlot(name);
		plot->setType(static_cast<Plot::Type>(entry.value("type", static_cast<uint8_t>(Plot::Type::CURVE))));

		/* A project written before the labels existed has neither key, and an
		   empty label is the same as no label, so both read back the same. */
		plot->setXAxisLabel(entry.value("xAxisLabel", std::string()));
		plot->setYAxisLabel(entry.value("yAxisLabel", std::string()));

		/* A project written before the cursors existed has none of these
		   keys, which reads back as cursors that are switched off - the same
		   as a new plot, so an older project opens looking as it was left. */
		plot->setCursorsVisible(entry.value("cursorsVisible", false));
		plot->setCursorMode(Plot::cursorModeFromIndex(entry.value("cursorMode", 0u)));
		plot->setStatisticsVisible(entry.value("statisticsVisible", false));

		if (entry.contains("cursorValues") && entry.at("cursorValues").is_object())
		{
			const json& values = entry.at("cursorValues");

			plot->markerX0.setValue(values.value("x0", 0.0));
			plot->markerX1.setValue(values.value("x1", 0.0));
			plot->markerY0.setValue(values.value("y0", 0.0));
			plot->markerY1.setValue(values.value("y1", 0.0));
		}

		if (plot->getType() == Plot::Type::XY)
		{
			const std::string xAxisVariable = entry.value("xAxisVariable", "");

			if (variableHandler->contains(xAxisVariable))
				plot->setXAxisVariable(variableHandler->getVariable(xAxisVariable).get());
		}

		logger->info("Adding plot: {}", name);

		if (!entry.contains("series") || !entry.at("series").is_array())
			continue;

		for (const auto& series : entry.at("series"))
		{
			const std::string variableName = series.value("name", "");

			if (!variableHandler->contains(variableName))
			{
				logger->warn("Plot {} references unknown variable {}", name, variableName);
				continue;
			}

			plot->addSeries(variableHandler->getVariable(variableName).get());
			auto newSeries = plot->getSeries(variableName);
			newSeries->visible = series.value("visibility", true);
			newSeries->format = displayFormatFromString(series.value("format", std::string("dec")));

			logger->info("Adding series: {}", variableName);
		}
	}
}

void ProjectHandler::deserializeGroups(const json& groups)
{
	/* A group names the group it is nested in, and the file is free to list a
	   child before its parent, so the names are attached in a second pass once
	   every group exists. */
	std::vector<std::pair<std::string, std::string>> pendingParents;

	for (const auto& entry : groups)
	{
		if (!entry.is_object())
			continue;

		const std::string name = entry.value("name", "");

		if (name.empty())
			continue;

		auto group = plotGroupHandler->addGroup(name, entry.value("type", "sampling") == "recorder" ? PlotGroup::Type::Recorder : PlotGroup::Type::Sampling);

		logger->info("Adding group: {} ({})", name, group->getTypeName());

		const std::string parent = entry.value("parent", "");

		if (!parent.empty())
			pendingParents.emplace_back(name, parent);

		if (!entry.contains("plots") || !entry.at("plots").is_array())
			continue;

		for (const auto& plot : entry.at("plots"))
		{
			const std::string plotName = plot.value("name", "");

			if (!plotHandler->checkIfPlotExists(plotName))
			{
				logger->warn("Group {} references unknown plot {}", name, plotName);
				continue;
			}

			group->addPlot(plotHandler->getPlot(plotName), plot.value("visibility", true));
		}
	}

	for (const auto& [name, parent] : pendingParents)
	{
		if (!plotGroupHandler->checkIfGroupExists(parent))
		{
			/* A parent that is not in the file would hide the group under a name
			   that cannot be reached, so it stays at the top level instead. */
			logger->warn("Group {} names a parent that is not in the project: {}", name, parent);
			continue;
		}

		/* Refused when the two names form a cycle, which would leave every group
		   of the circle unreachable from the top level. */
		if (!plotGroupHandler->moveGroup(name, parent))
			logger->warn("Group {} cannot be nested in {}", name, parent);
	}

	/* A project without groups still needs one so that sampling has a target. */
	if (plotGroupHandler->getGroupCount() == 0)
	{
		const std::string defaultGroupName = "default group";
		auto group = plotGroupHandler->addGroup(defaultGroupName);
		plotGroupHandler->setActiveGroup(defaultGroupName);

		for (const std::shared_ptr<Plot>& plot : *plotHandler)
			group->addPlot(plot);

		logger->info("Adding group: {}", defaultGroupName);
	}
}

bool ProjectHandler::deserialize(const json& root, ProjectData& data)
{
	if (!root.is_object())
		return false;

	clearModel();

	data = ProjectData{};

	const uint32_t fileVersion = root.value("formatVersion", formatVersion);

	if (fileVersion > formatVersion)
	{
		logger->error("Project file uses format version {}, this build supports {}", fileVersion, formatVersion);
		return false;
	}

	if (root.contains("elf") && root.at("elf").is_object())
		data.elfPath = root.at("elf").value("path", "");

	if (root.contains("acquisition") && root.at("acquisition").is_object())
	{
		const auto& acquisition = root.at("acquisition");

		data.viewer.sampleFrequencyHz = acquisition.value("sampleFrequencyHz", data.viewer.sampleFrequencyHz);
		data.viewer.maxPoints = acquisition.value("maxPoints", data.viewer.maxPoints);
		data.viewer.maxViewportPoints = acquisition.value("maxViewportPoints", data.viewer.maxViewportPoints);
		data.viewer.refreshAddressesOnElfChange = acquisition.value("refreshAddressesOnElfChange", data.viewer.refreshAddressesOnElfChange);
		data.viewer.stopAcquisitionOnElfChange = acquisition.value("stopAcquisitionOnElfChange", data.viewer.stopAcquisitionOnElfChange);
		data.viewer.gdbCommand = acquisition.value("gdbCommand", data.viewer.gdbCommand);
		data.viewer.elfParser = acquisition.value("elfParser", data.viewer.elfParser);
		data.viewer.ofd2000Command = acquisition.value("ofd2000Command", data.viewer.ofd2000Command);

		if (acquisition.contains("logging") && acquisition.at("logging").is_object())
		{
			data.viewer.loggingEnabled = acquisition.at("logging").value("enabled", data.viewer.loggingEnabled);
			data.viewer.logDirectory = acquisition.at("logging").value("directory", data.viewer.logDirectory);
		}

		if (acquisition.contains("probe"))
			data.viewer.probe = deserializeProbe(acquisition.at("probe"));
	}

	if (root.contains("trace") && root.at("trace").is_object())
	{
		const auto& trace = root.at("trace");

		data.trace.coreFrequency = trace.value("coreFrequency", data.trace.coreFrequency);
		data.trace.tracePrescaler = trace.value("tracePrescaler", data.trace.tracePrescaler);
		data.trace.maxPoints = trace.value("maxPoints", data.trace.maxPoints);
		data.trace.maxViewportPointsPercent = trace.value("maxViewportPointsPercent", data.trace.maxViewportPointsPercent);
		data.trace.triggerChannel = trace.value("triggerChannel", data.trace.triggerChannel);
		data.trace.triggerLevel = trace.value("triggerLevel", data.trace.triggerLevel);
		data.trace.shouldReset = trace.value("shouldReset", data.trace.shouldReset);
		data.trace.timeout = trace.value("timeout", data.trace.timeout);

		if (trace.contains("logging") && trace.at("logging").is_object())
		{
			data.trace.loggingEnabled = trace.at("logging").value("enabled", data.trace.loggingEnabled);
			data.trace.logDirectory = trace.at("logging").value("directory", data.trace.logDirectory);
		}

		if (trace.contains("probe"))
			data.trace.probe = deserializeProbe(trace.at("probe"));

		if (trace.contains("plots") && trace.at("plots").is_array())
		{
			for (const auto& entry : trace.at("plots"))
			{
				if (!entry.is_object() || !entry.contains("name"))
					continue;

				ProjectTracePlot plot{};
				plot.name = entry.value("name", "");
				plot.alias = entry.value("alias", plot.name);
				plot.visibility = entry.value("visibility", true);
				plot.domain = entry.value("domain", static_cast<uint8_t>(0));
				plot.varType = entry.value("varType", static_cast<uint8_t>(6));
				plot.color = entry.value("color", 0xFFFFFFFFu);

				if (!plot.name.empty())
					data.tracePlots.push_back(plot);
			}
		}
	}

	if (root.contains("variables") && root.at("variables").is_array())
		deserializeVariables(root.at("variables"));

	if (root.contains("plots") && root.at("plots").is_array())
		deserializePlots(root.at("plots"));

	if (root.contains("groups") && root.at("groups").is_array())
		deserializeGroups(root.at("groups"));

	const std::string activeGroup = root.value("activeGroup", "");

	if (!activeGroup.empty() && plotGroupHandler->checkIfGroupExists(activeGroup))
		plotGroupHandler->setActiveGroup(activeGroup);

	if (root.contains("writePlans") && root.at("writePlans").is_array())
		deserializeWritePlans(root.at("writePlans"), data);

	return true;
}

void ProjectHandler::deserializeWritePlans(const json& plans, ProjectData& data)
{
	for (const auto& entry : plans)
	{
		if (!entry.is_object())
			continue;

		ProjectWritePlan plan{};
		plan.name = entry.value("name", "");
		plan.variable = entry.value("variable", "");

		if (plan.name.empty())
		{
			logger->warn("Skipping a write plan without a name");
			continue;
		}

		if (entry.contains("steps") && entry.at("steps").is_array())
		{
			for (const auto& step : entry.at("steps"))
			{
				if (!step.is_object())
					continue;

				ProjectWriteStep converted{};
				converted.time = step.value("time", 0.0);
				converted.value = step.value("value", 0.0);
				plan.steps.push_back(converted);
			}
		}

		data.writePlans.push_back(plan);
	}
}

ProjectHandler::OpenResult ProjectHandler::open(const std::string& path, ProjectData& data)
{
	if (path.empty())
		return OpenResult::CannotOpenFile;

	std::ifstream file(path);

	if (!file.is_open())
	{
		logger->error("Cannot open project file: {}", path);
		return OpenResult::CannotOpenFile;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();

	json root;

	try
	{
		root = json::parse(buffer.str());
	}
	catch (const std::exception& ex)
	{
		logger->error("Not a JSON project file ({}): {}", path, ex.what());
		return OpenResult::ParseError;
	}

	if (!root.is_object())
		return OpenResult::ParseError;

	if (root.value("formatVersion", formatVersion) > formatVersion)
	{
		logger->error("Project file {} was created by a newer version of {}", path, applicationName);
		return OpenResult::NewerFormatVersion;
	}

	if (!deserialize(root, data))
	{
		clearModel();
		return OpenResult::ParseError;
	}

	lastSavedContent = serialize(data);
	logger->info("Opened project: {}", path);

	return OpenResult::Ok;
}

bool ProjectHandler::isSavingRequired(const ProjectData& data) const
{
	return serialize(data) != lastSavedContent;
}
