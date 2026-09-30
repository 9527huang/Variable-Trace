#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "McpTools.hpp"

#include "IElfParser.hpp"
#include "LibDwarfParser.hpp"
#include "TiOfdParser.hpp"

namespace mcp
{
	namespace
	{
		/* Limits the acquisition settings window enforces, repeated here so that an
		   out of range request is refused with the accepted range instead of being
		   silently adjusted. */
		constexpr uint32_t minPoints = 100;
		constexpr uint32_t maxPoints = 20000;

		struct VariableRequest
		{
			std::string name;
			Variable::Type type = Variable::Type::U32;
		};

		std::vector<VariableRequest> collectVariableRequests(const Json& arguments)
		{
			std::vector<VariableRequest> requests;

			if (!has(arguments, "variables"))
			{
				VariableRequest request;
				request.name = requireString(arguments, "name");
				request.type = variableTypeFromString(optionalString(arguments, "type").value_or("u32"));
				requests.push_back(request);

				return requests;
			}

			const Json& entries = arguments.at("variables");

			if (!entries.is_array())
				throw ToolError("Field 'variables' must be a JSON array of {name, type} objects.");

			if (entries.empty())
				throw ToolError("Field 'variables' must not be empty.");

			for (const Json& entry : entries)
			{
				if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string())
					throw ToolError("Every entry of 'variables' must be an object with a string 'name'.");

				VariableRequest request;
				request.name = entry.at("name").get<std::string>();

				if (request.name.empty())
					throw ToolError("A name in 'variables' must not be empty.");

				if (entry.contains("type") && !entry.at("type").is_null())
				{
					if (!entry.at("type").is_string())
						throw ToolError("The 'type' of every entry of 'variables' must be a string.");

					request.type = variableTypeFromString(entry.at("type").get<std::string>());
				}

				requests.push_back(request);
			}

			return requests;
		}

		Json addVariable(McpContext& context, const Json& arguments)
		{
			if (context.variableHandler == nullptr)
				reportUnavailable("the variable table", "add_variable");

			const bool batch = has(arguments, "variables");

			if (!batch && !has(arguments, "name"))
				throw ToolError("Pass 'name' with a single variable name, or 'variables' with an array of {name, type} objects.");

			const std::vector<VariableRequest> requests = collectVariableRequests(arguments);

			Json results = Json::array();

			for (const VariableRequest& request : requests)
			{
				const bool existed = context.variableHandler->contains(request.name);

				if (!existed)
				{
					/* addNewVariable appends a suffix when the name is taken, which
					   would make the stored name differ from the requested one, so it
					   is only reached for a free name. */
					context.variableHandler->addNewVariable(request.name);
					context.variableHandler->getVariable(request.name)->setType(request.type);
				}

				const Variable::Type type = context.variableHandler->getVariable(request.name)->getType();

				results.push_back(Json::object({{"name", request.name},
												{"type", variableTypeToString(type)},
												{"status", existed ? "already_exists" : "created"}}));
			}

			/* The single form answers with one object so that a caller does not
			   have to branch on which form it used. */
			return batch ? results : results.at(0);
		}

		Json removeVariable(McpContext& context, const Json& arguments)
		{
			const std::string name = requireString(arguments, "name");

			requireVariable(context, name);

			/* A plot holds a raw pointer to the variable, so every series has to go
			   before the variable itself is dropped. */
			uint32_t removedSeries = 0;

			for (PlotHandler* handler : {context.plotHandler, context.tracePlotHandler})
			{
				if (handler == nullptr)
					continue;

				for (const std::shared_ptr<Plot>& plot : *handler)
				{
					if (plot->removeSeries(name))
						removedSeries++;
				}
			}

			context.variableHandler->erase(name);

			const bool restarted = requestAcquisitionRestart(context);

			return Json::object({{"name", name}, {"removed_series", removedSeries}, {"acquisition_restarted", restarted}});
		}

		Json configureVariable(McpContext& context, const Json& arguments)
		{
			const std::string name = requireString(arguments, "name");
			const std::shared_ptr<Variable> variable = requireVariable(context, name);

			Json changed = Json::object();

			if (has(arguments, "tracked_name"))
			{
				const std::string trackedName = optionalString(arguments, "tracked_name").value_or("");
				const std::string effective = trackedName.empty() ? name : trackedName;

				variable->setTrackedName(effective);
				variable->setIsTrackedNameDifferent(effective != name);

				changed["tracked_name"] = trackedName;
			}

			const bool writingLimits = has(arguments, "write_limits_enabled") || has(arguments, "write_min") || has(arguments, "write_max");

			if (writingLimits)
			{
				const bool enabled = has(arguments, "write_limits_enabled") ? optionalBool(arguments, "write_limits_enabled").value_or(false)
																			 : variable->getWriteLimitsEnabled();

				double min = has(arguments, "write_min") ? *optionalNumber(arguments, "write_min") : variable->getWriteMin();
				double max = has(arguments, "write_max") ? *optionalNumber(arguments, "write_max") : variable->getWriteMax();

				/* Enabling without both bounds would clamp every write to whatever
				   the variable happened to carry, so the pair is demanded together. */
				if (enabled && (!has(arguments, "write_min") || !has(arguments, "write_max")))
					throw ToolError("Fields 'write_min' and 'write_max' are required when 'write_limits_enabled' is 1.");

				if (enabled && min > max)
					throw ToolError("Field 'write_min' must not be greater than 'write_max'.");

				variable->setWriteLimits(enabled, min, max);

				changed["write_limits_enabled"] = enabled;
				changed["write_min"] = min;
				changed["write_max"] = max;
			}

			if (has(arguments, "shift"))
			{
				const int64_t shift = *optionalInteger(arguments, "shift");

				if (shift < 0 || shift > 31)
					throw ToolError("Field 'shift' must be between 0 and 31.");

				variable->setShift(static_cast<uint32_t>(shift));
				changed["shift"] = shift;
			}

			if (has(arguments, "mask"))
			{
				const uint64_t mask = *optionalUnsigned(arguments, "mask");

				if (mask > 0xFFFFFFFFull)
					throw ToolError("Field 'mask' must fit into 32 bits.");

				variable->setMask(static_cast<uint32_t>(mask));
				changed["mask"] = mask;
			}

			const bool changingInterpretation = has(arguments, "interpretation") || has(arguments, "fractional_bits") || has(arguments, "base") || has(arguments, "enum_labels");

			if (changingInterpretation)
			{
				const Variable::HighLevelType interpretation = has(arguments, "interpretation")
																  ? interpretationFromString(*optionalString(arguments, "interpretation"))
																  : variable->getHighLevelType();

				const bool fractionalMode = interpretation == Variable::HighLevelType::SIGNEDFRAC || interpretation == Variable::HighLevelType::UNSIGNEDFRAC;
				const bool enumMode = interpretation == Variable::HighLevelType::ENUM || interpretation == Variable::HighLevelType::CUSTOM_ENUM;

				/* Both fields describe the fixed point format, which the other
				   interpretations do not use, so they are refused rather than stored
				   where nothing would read them. */
				if (!fractionalMode && (has(arguments, "fractional_bits") || has(arguments, "base")))
					throw ToolError("'fractional_bits' and 'base' only apply to a fractional interpretation. Pass 'interpretation' as 'signed_frac' or 'unsigned_frac'.");

				/* The label list belongs to an enumeration. Under any other
				   interpretation it would sit unread in the project file. */
				if (!enumMode && has(arguments, "enum_labels"))
					throw ToolError("'enum_labels' only applies to an enum interpretation. Pass 'interpretation' as 'enum' or 'custom_enum'.");

				Variable::Fractional fractional = variable->getFractional();

				if (has(arguments, "fractional_bits"))
				{
					const int64_t bits = *optionalInteger(arguments, "fractional_bits");

					if (bits < 1 || bits > 31)
						throw ToolError("Field 'fractional_bits' must be between 1 and 31.");

					fractional.fractionalBits = static_cast<uint32_t>(bits);
				}
				else if (fractionalMode && !variable->isFractional())
					throw ToolError("Field 'fractional_bits' is required when 'interpretation' is 'signed_frac' or 'unsigned_frac'.");

				if (has(arguments, "base"))
					fractional.base = *optionalNumber(arguments, "base");

				/* A zero scale would divide by zero the moment a raw value is derived
				   from a physical one. */
				if (fractionalMode && fractional.base == 0.0)
					throw ToolError("Field 'base' must not be zero for a fractional interpretation.");

				/* The labels are parsed before anything is written, so a malformed
				   array leaves the variable as it was. */
				std::vector<Variable::EnumLabel> labels = variable->getEnumLabels();

				if (has(arguments, "enum_labels"))
					labels = enumLabelsFromJson(arguments.at("enum_labels"), "enum_labels");

				/* Switching away from an enumeration drops the labels, because
				   nothing would consult them and they would outlive the decision
				   that created them. */
				if (!enumMode)
					labels.clear();

				variable->setFractional(fractional);
				variable->setHighLevelType(interpretation);
				variable->setEnumLabels(labels);

				changed["interpretation"] = interpretationToString(interpretation);
				changed["fractional_bits"] = fractional.fractionalBits;
				changed["base"] = fractional.base;

				if (enumMode)
					changed["enum_labels"] = enumLabelsToJson(labels);
			}

			if (changed.empty())
				throw ToolError("No property was provided. Pass at least one of 'tracked_name', 'write_limits_enabled', "
								"'write_min', 'write_max', 'shift', 'mask', 'interpretation', 'fractional_bits', 'base' or 'enum_labels'.");

			changed["name"] = name;

			return changed;
		}

		Json refreshVariableAddresses(McpContext& context, const Json&)
		{
			if (context.variableHandler == nullptr)
				reportUnavailable("the variable table", "refresh_variable_addresses");

			if (!context.refreshVariableAddresses || !context.getElfPath)
				reportUnavailable("ELF address refresh", "refresh_variable_addresses");

			const std::string elfPath = context.getElfPath();

			if (elfPath.empty())
				throw ToolError("No ELF file is configured. Call set_elf_path first.");

			if (!std::filesystem::exists(elfPath))
				throw ToolError("The ELF file '" + elfPath + "' does not exist.");

			if (!context.refreshVariableAddresses(elfPath))
				throw ToolError("The ELF file '" + elfPath + "' could not be parsed. Check the ELF path and the GDB command in the acquisition settings.");

			uint32_t found = 0;
			uint32_t notFound = 0;

			for (const std::shared_ptr<Variable>& variable : *context.variableHandler)
			{
				if (variable->getIsFound())
					found++;
				else
					notFound++;
			}

			return Json::object({{"elf_path", elfPath}, {"found", found}, {"not_found", notFound}});
		}

		Json setElfPath(McpContext& context, const Json& arguments)
		{
			if (!context.setElfPath)
				reportUnavailable("the ELF path setting", "set_elf_path");

			const std::string path = requireString(arguments, "path");
			const bool relative = optionalBool(arguments, "relative").value_or(true);
			const std::filesystem::path elfPath(path);

			/* 'relative' says how the path is stored, not how it is passed in, so an
			   absolute path with relative storage is the normal case. A relative
			   path is resolved against the project file, which the tool cannot see,
			   so only an absolute path can be checked here. */
			if (elfPath.is_absolute() && !std::filesystem::exists(elfPath))
				throw ToolError("The ELF file '" + path + "' does not exist.");

			if (!context.setElfPath(path, relative))
				throw ToolError("The ELF file '" + path + "' could not be set.");

			return Json::object({{"path", path}, {"relative", relative}});
		}

		Json setSampling(McpContext& context, const Json& arguments)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("sampling settings", "set_sampling");

			ViewerDataHandler::Settings settings = context.viewerDataHandler->getSettings();

			Json changed = Json::object();
			bool frequencyChanged = false;

			if (has(arguments, "sample_frequency_hz"))
			{
				const int64_t frequency = *optionalInteger(arguments, "sample_frequency_hz");

				if (frequency < static_cast<int64_t>(ViewerDataHandler::minSamplinFrequencyHz) || frequency > static_cast<int64_t>(ViewerDataHandler::maxSamplinFrequencyHz))
					throw ToolError("Field 'sample_frequency_hz' must be between 1 and 1000000.");

				settings.sampleFrequencyHz = static_cast<uint32_t>(frequency);
				frequencyChanged = true;
				changed["sample_frequency_hz"] = frequency;
			}

			if (has(arguments, "max_points"))
			{
				const int64_t points = *optionalInteger(arguments, "max_points");

				if (points < static_cast<int64_t>(minPoints) || points > static_cast<int64_t>(maxPoints))
					throw ToolError("Field 'max_points' must be between " + std::to_string(minPoints) + " and " + std::to_string(maxPoints) + ".");

				settings.maxPoints = static_cast<uint32_t>(points);
				changed["max_points"] = points;
			}

			if (has(arguments, "max_viewport_points"))
			{
				const int64_t points = *optionalInteger(arguments, "max_viewport_points");

				if (points < static_cast<int64_t>(minPoints) || points > static_cast<int64_t>(maxPoints))
					throw ToolError("Field 'max_viewport_points' must be between " + std::to_string(minPoints) + " and " + std::to_string(maxPoints) + ".");

				settings.maxViewportPoints = static_cast<uint32_t>(points);
			}

			if (has(arguments, "viewport_width_ms"))
			{
				const double windowMs = *optionalNumber(arguments, "viewport_width_ms");

				if (windowMs <= 0.0)
					throw ToolError("Field 'viewport_width_ms' must be greater than zero.");

				/* The viewport of this build shows the newest maxViewportPoints
				   samples instead of a wall clock window, and the relation between
				   the two is seconds = points / frequency. A time window is
				   therefore the more specific request and wins when both are sent. */
				const double points = std::clamp((windowMs / 1000.0) * static_cast<double>(settings.sampleFrequencyHz),
												 static_cast<double>(minPoints),
												 static_cast<double>(std::min(settings.maxPoints, maxPoints)));

				settings.maxViewportPoints = static_cast<uint32_t>(points);
				changed["viewport_width_ms"] = windowMs;
			}

			if (changed.empty())
				throw ToolError("No sampling parameter was provided. Pass at least one of 'sample_frequency_hz', 'max_points', "
								"'max_viewport_points' or 'viewport_width_ms'.");

			/* The rendered count can never exceed the stored count. */
			settings.maxViewportPoints = std::clamp(settings.maxViewportPoints, minPoints, settings.maxPoints);

			context.viewerDataHandler->setSettings(settings);

			changed["max_viewport_points"] = settings.maxViewportPoints;

			/* The frequency is handed to the probe when it is started, so a changed
			   value only takes effect through a restart. */
			changed["acquisition_restarted"] = frequencyChanged && requestAcquisitionRestart(context);

			return changed;
		}

		Json setElfParser(McpContext& context, const Json& arguments)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("ELF parser settings", "set_elf_parser");

			const std::string parser = optionalString(arguments, "parser").value_or("gdb");

			IElfParser::Type type = IElfParser::Type::Gdb;

			if (!IElfParser::typeFromName(parser, type))
				throw ToolError("Unknown parser '" + parser + "'. Use 'gdb', 'libdwarf' or 'c2000'.");

			const std::optional<std::string> ofd2000Command = optionalString(arguments, "ofd2000_command");

			if (ofd2000Command.has_value() && type != IElfParser::Type::TiOfd)
				throw ToolError("Field 'ofd2000_command' only applies to the 'c2000' parser.");

			if (type == IElfParser::Type::LibDwarf && !LibDwarfParser::isCompiledIn())
				reportUnavailable("the 'libdwarf' ELF parser", "set_elf_parser");

			ViewerDataHandler::Settings settings = context.viewerDataHandler->getSettings();

			/* Everything is checked before anything is written, so a rejected
			   call leaves the settings as they were rather than half applied. */
			if (has(arguments, "gdb_command"))
			{
				const std::string command = optionalString(arguments, "gdb_command").value_or("");

				if (command.empty())
					throw ToolError("Field 'gdb_command' must not be empty.");

				settings.gdbCommand = command;
			}

			if (ofd2000Command.has_value())
			{
				if (ofd2000Command->empty())
					throw ToolError("Field 'ofd2000_command' must not be empty.");

				settings.ofd2000Command = *ofd2000Command;
			}

			/* The C2000 parser is useless without its tool, and the tool is not
			   shipped with the application, so the switch is refused rather
			   than made and then failing at the first refresh. */
			if (type == IElfParser::Type::TiOfd && !TiOfdParser::toolExists(settings.ofd2000Command))
				throw ToolError("ofd2000 was not found at '" + settings.ofd2000Command + "'. Set 'ofd2000_command' to the path of the tool.");

			settings.elfParser = IElfParser::nameOf(type);
			context.viewerDataHandler->setSettings(settings);

			return Json::object({{"parser", settings.elfParser},
								 {"gdb_command", settings.gdbCommand},
								 {"ofd2000_command", settings.ofd2000Command}});
		}

		Json setLogging(McpContext& context, const Json& arguments)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("CSV logging settings", "set_logging");

			if (!has(arguments, "enabled"))
				throw ToolError("Field 'enabled' is required.");

			const bool enabled = optionalBool(arguments, "enabled").value_or(false);
			const std::optional<std::string> directory = optionalString(arguments, "log_dir");

			if (enabled && (!directory.has_value() || directory->empty()))
				throw ToolError("Field 'log_dir' is required when 'enabled' is 1.");

			ViewerDataHandler::Settings settings = context.viewerDataHandler->getSettings();

			settings.shouldLog = enabled;

			if (directory.has_value())
				settings.logFilePath = *directory;

			if (enabled && settings.logFilePath.empty())
				throw ToolError("No log directory is configured. Pass 'log_dir'.");

			context.viewerDataHandler->setSettings(settings);

			return Json::object({{"enabled", enabled}, {"log_dir", settings.logFilePath}});
		}
	}  // namespace

	void VariableTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "add_variable",
					  .description = "Create one or more variables. Single: 'name' + optional 'type' (u8/i8/u16/i16/u32/i32/f32, default u32). Batch: 'variables' as JSON array of {\"name\",\"type\"?} objects -> JSON array of {name, type, status: 'created'|'already_exists'}.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name (single mode)")},
																{"type", enumProperty("Memory type: u8/i8/u16/i16/u32/i32/f32", {"u8", "i8", "u16", "i16", "u32", "i32", "f32"})},
																{"variables", arrayProperty("JSON array of {name, type?} objects (batch mode)",
																							objectSchema(Json::object({{"name", stringProperty("Variable name")},
																													   {"type", stringProperty("Memory type: u8/i8/u16/i16/u32/i32/f32")}}),
																										 {"name"}))}}),
												  {}),
					  .handler = addVariable});

		registry.add({.name = "remove_variable",
					  .description = "Delete a variable by name.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name")}}), {"name"}),
					  .handler = removeVariable});

		registry.add({.name = "configure_variable",
					  .description = "Configure variable properties. All fields except 'name' are optional - only provided fields are updated. "
									 "tracked_name: ELF symbol override (empty = same as name). "
									 "write_limits_enabled (0/1) + write_min/write_max: write clamping. "
									 "shift (0-31), mask: post-processing (right-shift then AND). "
									 "interpretation: 'none'|'signed_frac'|'unsigned_frac'|'enum'|'custom_enum'; fractional_bits (1-31) + base required for frac modes. "
									 "enum_labels: JSON array of {\"label\":string,\"value\":number} for custom_enum. "
									 "Returns a JSON object with the fields that were changed.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name")},
																{"tracked_name", stringProperty("ELF symbol override (empty = reset to variable name)")},
																{"write_limits_enabled", booleanProperty("1 = enable, 0 = disable write limits")},
																{"write_min", numberProperty("Min allowed write value (required when enabling)")},
																{"write_max", numberProperty("Max allowed write value (required when enabling)")},
																{"shift", integerProperty("Right-shift bits (0-31)")},
																{"mask", integerProperty("Bitmask after shift (default 0xFFFFFFFF)")},
																{"interpretation", enumProperty("'none', 'signed_frac', 'unsigned_frac', 'enum', or 'custom_enum'",
																								{"none", "signed_frac", "unsigned_frac", "enum", "custom_enum"})},
																{"fractional_bits", integerProperty("Fractional bits for frac modes (1-31)")},
																{"base", numberProperty("Scale factor for frac modes (default 1.0)")},
																{"enum_labels", arrayProperty("JSON array of {label, value} for custom_enum",
																							  objectSchema(Json::object({{"label", stringProperty("Label text")},
																														 {"value", numberProperty("Numeric value")}}),
																										   {"label", "value"}))}}),
												  {"name"}),
					  .handler = configureVariable});

		registry.add({.name = "refresh_variable_addresses",
					  .description = "Re-parse the ELF and update variable addresses. Returns {found, not_found} counts.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = refreshVariableAddresses,
					  /* Parsing an ELF starts a GDB process that runs for seconds, and
						 the refresh is already asynchronous in the application. */
					  .onGuiThread = false});

		registry.add({.name = "set_elf_path",
					  .description = "Set the symbol file (*.elf or *.axf, both are ELF) for variable address resolution. 'relative': store path relative to project file (default 1).",
					  .inputSchema = objectSchema(Json::object({{"path", stringProperty("Path to the *.elf/*.axf file")},
																{"relative", booleanProperty("1 = relative path (default), 0 = absolute")}}),
												  {"path"}),
					  .handler = setElfPath});

		registry.add({.name = "set_sampling",
					  .description = "Configure sampling parameters. All fields optional - only provided fields are updated. "
									 "  sample_frequency_hz : 1 - 1 000 000 "
									 "  max_points          : total samples kept per series "
									 "  max_viewport_points : samples rendered in viewport "
									 "  viewport_width_ms   : visible time window in ms",
					  .inputSchema = objectSchema(Json::object({{"sample_frequency_hz", integerProperty("Sampling frequency in Hz (1 - 1 000 000)")},
																{"max_points", integerProperty("Max stored samples per series")},
																{"max_viewport_points", integerProperty("Max rendered samples in viewport")},
																{"viewport_width_ms", numberProperty("Visible time window in ms")}})),
					  .handler = setSampling});

		registry.add({.name = "set_elf_parser",
					  .description = "Choose the ELF parser. "
									 "  parser: 'gdb' (reliable, default), 'libdwarf' (experimental) or 'c2000' (TI C2000 targets, via ofd2000) "
									 "  gdb_command: GDB executable path (only for 'gdb' parser) "
									 "  ofd2000_command: ofd2000 executable path (only for 'c2000' parser)",
					  .inputSchema = objectSchema(Json::object({{"parser", enumProperty("'gdb', 'libdwarf' or 'c2000'", {"gdb", "libdwarf", "c2000"})},
																{"gdb_command", stringProperty("GDB executable path")},
																{"ofd2000_command", stringProperty("ofd2000 executable path")}})),
					  .handler = setElfParser});

		registry.add({.name = "set_logging",
					  .description = "Enable or disable CSV file logging. 'log_dir' is required when enabling.",
					  .inputSchema = objectSchema(Json::object({{"enabled", booleanProperty("1 to enable, 0 to disable")},
																{"log_dir", stringProperty("Directory for CSV log files")}}),
												  {"enabled"}),
					  .handler = setLogging});
	}
}  // namespace mcp
