#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "McpTools.hpp"
#include "RecorderHandler.hpp"

namespace mcp
{
	/*
	 * The recorder is the half of the tool set that works on data the target
	 * collected on its own.
	 *
	 * Two things about it shape the code below. The first is that it has no
	 * connection of its own: it borrows the one the viewer acquisition has open,
	 * so every tool that touches the target starts by checking that the
	 * acquisition is running. The second is that the settings live on the host
	 * until a run begins, which is why the setters only change the stored
	 * configuration and write to the target at the moment a run is restarted.
	 */
	namespace
	{
		recorder::RecorderHandler& requireRecorder(McpContext& context, const std::string& toolName)
		{
			if (context.recorderHandler == nullptr)
				reportUnavailable("the recorder subsystem", toolName);

			return *context.recorderHandler;
		}

		/* A probe that is not running has no port open, and the failure would
		   otherwise surface inside the transport as a much less specific
		   message. */
		void requireProbeOpen(McpContext& context, const std::string& toolName)
		{
			if (context.viewerDataHandler == nullptr || context.viewerDataHandler->getDebugProbe() == nullptr)
				reportUnavailable("the debug probe", toolName);

			if (context.viewerDataHandler->getStateImmediate() != DataHandlerBase::State::RUN)
				throw ToolError("The recorder works through the connection the acquisition has open, and the acquisition is stopped. Call start_acquisition first, then set up the recorder.");
		}

		/* The recorder sends its own frames over the connection the sampling loop
		   is reading, so it waits for its turn the same way the window does. */
		std::unique_lock<std::mutex> lockProbe(McpContext& context)
		{
			if (context.viewerDataHandler == nullptr)
				reportUnavailable("the viewer data handler", "the recorder tools");

			return context.viewerDataHandler->lockProbe();
		}

		std::shared_ptr<PlotGroup> requireRecorderGroup(McpContext& context)
		{
			if (context.plotGroupHandler == nullptr)
				reportUnavailable("the group list", "the recorder tools");

			std::shared_ptr<PlotGroup> group = context.plotGroupHandler->getActiveRecorderGroup();

			if (group == nullptr)
				throw ToolError("No recorder group is active. Add one with add_group using type 'recorder', or select it with set_active_group.");

			return group;
		}

		/* Pushes the stored configuration to the target again. Only called while
		   a run is in progress, where a change would otherwise sit in the host
		   until the next run and look like it had been ignored. */
		void restartIfRunning(McpContext& context, recorder::RecorderHandler& handler, const std::shared_ptr<PlotGroup>& group)
		{
			if (!handler.isRunning() || context.viewerDataHandler == nullptr || context.viewerDataHandler->getDebugProbe() == nullptr)
				return;

			recorder::Config config = handler.getPendingConfig();
			std::string error;

			if (!recorder::configFromGroup(*group, config, error))
				throw ToolError(error);

			handler.setPendingConfig(config);

			IDebugProbe& probe = *context.viewerDataHandler->getDebugProbe();
			std::unique_lock<std::mutex> probeLock = lockProbe(context);

			if (!handler.start(probe, config, error))
				throw ToolError("The recorder could not be restarted: " + error);
		}

		Json triggerToJson(const recorder::TriggerConfig& trigger)
		{
			return Json::object({{"trigger_enabled", trigger.enabled},
								 {"trigger_edge", recorder::triggerEdgeToString(trigger.edge)},
								 {"trigger_value", trigger.value},
								 {"pre_trigger_samples", trigger.preTriggerSamples},
								 {"trigger_source", trigger.source}});
		}

		Json setRecorderEnabled(McpContext& context, const Json& arguments)
		{
			recorder::RecorderHandler& handler = requireRecorder(context, "set_recorder_enabled");

			const bool enabled = optionalBool(arguments, "enabled").value_or(false);
			handler.setEnabled(enabled);

			/* Turning the subsystem off has to leave the target idle. A timer
			   interrupt that keeps sampling into a buffer nobody will read is
			   exactly the kind of thing that is forgotten about and then blamed
			   for a timing problem elsewhere. */
			if (!enabled && handler.isRunning() && context.viewerDataHandler != nullptr && context.viewerDataHandler->getDebugProbe() != nullptr)
			{
				std::string error;
				std::unique_lock<std::mutex> probeLock = lockProbe(context);

				if (!handler.stop(*context.viewerDataHandler->getDebugProbe(), error))
					throw ToolError("The recorder was disabled, but it could not be stopped on the target: " + error);
			}

			return Json::object({{"enabled", handler.isEnabled()}});
		}

		Json detectRecorder(McpContext& context, const Json&)
		{
			recorder::RecorderHandler& handler = requireRecorder(context, "detect_recorder");

			/* The addresses sit in the symbol file, so they are looked up again
			   here rather than assumed to be in step with the last import. */
			if (!handler.hasSymbols() && context.syncRecorderSymbols)
				context.syncRecorderSymbols();

			const auto zeroes = [](const std::string& detail)
			{
				return Json::object({{"version", 0},
									 {"revision", 0},
									 {"timestep_ns", 0},
									 {"sampling_hz", 0.0},
									 {"max_buf_size", 0},
									 {"max_variables", 0},
									 {"float_support", false},
									 {"detected", false},
									 {"detail", detail}});
			};

			if (!handler.hasSymbols())
				return zeroes("The symbol file has no ____recorderSettings in it. Import the variables from an *.elf built with the recorder sources.");

			if (context.viewerDataHandler == nullptr || context.viewerDataHandler->getDebugProbe() == nullptr)
				return zeroes("No debug probe is selected.");

			if (context.viewerDataHandler->getStateImmediate() != DataHandlerBase::State::RUN)
				return zeroes("The acquisition is stopped, so there is no open connection to read the settings over.");

			IDebugProbe& probe = *context.viewerDataHandler->getDebugProbe();
			std::string error;
			bool detected = false;

			{
				std::unique_lock<std::mutex> probeLock = lockProbe(context);
				detected = handler.detect(probe, error);
			}

			if (!detected)
				return zeroes(error);

			const recorder::Settings& settings = handler.getDetectedSettings();

			return Json::object({{"version", settings.version},
								 {"revision", settings.revision},
								 {"timestep_ns", settings.timestepNs},
								 {"sampling_hz", settings.timestepNs == 0 ? 0.0 : 1e9 / static_cast<double>(settings.timestepNs)},
								 {"max_buf_size", settings.maxBufferSize},
								 {"max_variables", settings.maxVariables},
								 {"float_support", settings.floatSupport != 0},
								 {"detected", true}});
		}

		Json getRecorderSettings(McpContext& context, const Json&)
		{
			recorder::RecorderHandler& handler = requireRecorder(context, "get_recorder_settings");
			std::shared_ptr<PlotGroup> group = requireRecorderGroup(context);
			const recorder::Config& config = handler.getPendingConfig();

			Json result = Json::object({{"group", group->getName()},
										{"mode", recorder::modeToString(config.mode)},
										{"skipped_samples", config.skippedSamples},
										{"running", handler.isRunning()}});

			for (const auto& field : triggerToJson(config.trigger).items())
				result[field.key()] = field.value();

			return result;
		}

		Json setRecorderMode(McpContext& context, const Json& arguments)
		{
			recorder::RecorderHandler& handler = requireRecorder(context, "set_recorder_mode");
			requireApiWritesEnabled(context);

			std::shared_ptr<PlotGroup> group = requireRecorderGroup(context);

			const std::string name = requireString(arguments, "mode");
			recorder::Mode mode = recorder::Mode::Run;

			if (!recorder::modeFromString(name, mode))
				throw ToolError("Unknown recorder mode '" + name + "'. Use 'run', 'normal' or 'single'.");

			/* The two triggered modes are the only ones that arm the comparison,
			   so a run without a source would silently behave like free running
			   and look like the trigger had been ignored. */
			if (mode != recorder::Mode::Run)
			{
				const recorder::Config& current = handler.getPendingConfig();

				if (!current.trigger.enabled || current.trigger.source.empty())
					throw ToolError("Mode '" + name + "' needs a trigger. Call set_recorder_trigger with a source_variable first.");
			}

			recorder::Config config = handler.getPendingConfig();
			config.mode = mode;
			handler.setPendingConfig(config);

			restartIfRunning(context, handler, group);

			return Json::object({{"mode", recorder::modeToString(mode)}, {"restarted", handler.isRunning()}});
		}

		Json setRecorderTrigger(McpContext& context, const Json& arguments)
		{
			recorder::RecorderHandler& handler = requireRecorder(context, "set_recorder_trigger");
			requireApiWritesEnabled(context);

			std::shared_ptr<PlotGroup> group = requireRecorderGroup(context);
			recorder::Config config = handler.getPendingConfig();

			if (has(arguments, "source_variable"))
			{
				const std::string source = optionalString(arguments, "source_variable").value_or("");

				/* Checked against the group now rather than at the start of the
				   run, because a name that matches nothing would otherwise leave
				   the trigger unarmed and the capture looking like a free run. */
				bool found = false;

				for (auto iterator = group->begin(); iterator != group->end() && !found; ++iterator)
				{
					for (const auto& entry : iterator->second.plot->getSeriesMap())
					{
						if (entry.second && entry.second->var != nullptr && entry.second->var->getName() == source)
						{
							found = true;
							break;
						}
					}
				}

				if (!found)
					throw ToolError("The active recorder group has no variable named '" + source + "'. Add it to a plot of that group first.");

				config.trigger.source = source;
				config.trigger.enabled = true;
			}

			if (has(arguments, "edge"))
			{
				const std::string edge = optionalString(arguments, "edge").value_or("");

				if (edge == "rising")
					config.trigger.edge = recorder::TriggerEdge::Rising;
				else if (edge == "falling")
					config.trigger.edge = recorder::TriggerEdge::Falling;
				else
					throw ToolError("Unknown trigger edge '" + edge + "'. Use 'rising' or 'falling'.");
			}

			if (has(arguments, "value"))
				config.trigger.value = optionalNumber(arguments, "value").value_or(0.0);

			if (has(arguments, "pre_trigger_samples"))
			{
				const std::optional<int64_t> samples = optionalInteger(arguments, "pre_trigger_samples");

				if (samples.has_value() && *samples < 0)
					throw ToolError("Field 'pre_trigger_samples' must not be negative.");

				config.trigger.preTriggerSamples = static_cast<uint32_t>(samples.value_or(0));
			}

			if (has(arguments, "trigger_enabled"))
				config.trigger.enabled = optionalBool(arguments, "trigger_enabled").value_or(false);

			/* Disabling the trigger while a triggered mode is selected would leave
			   the mode asking for something it no longer has. */
			if (!config.trigger.enabled && config.mode != recorder::Mode::Run)
				throw ToolError("The trigger cannot be disabled while the recorder mode is '" + recorder::modeToString(config.mode) + "'. Set the mode to 'run' first.");

			handler.setPendingConfig(config);

			restartIfRunning(context, handler, group);

			Json result = triggerToJson(config.trigger);
			result["restarted"] = handler.isRunning();
			return result;
		}

		Json setRecorderDownsampling(McpContext& context, const Json& arguments)
		{
			recorder::RecorderHandler& handler = requireRecorder(context, "set_recorder_downsampling");
			requireApiWritesEnabled(context);

			std::shared_ptr<PlotGroup> group = requireRecorderGroup(context);

			const std::optional<int64_t> skipped = optionalInteger(arguments, "skipped_samples");

			if (!skipped.has_value())
				throw ToolError("Field 'skipped_samples' is required.");

			if (*skipped < 0)
				throw ToolError("Field 'skipped_samples' must not be negative.");

			recorder::Config config = handler.getPendingConfig();
			config.skippedSamples = static_cast<uint32_t>(*skipped);
			handler.setPendingConfig(config);

			restartIfRunning(context, handler, group);

			const recorder::Settings& settings = handler.getDetectedSettings();
			Json result = Json::object({{"skipped_samples", config.skippedSamples}, {"restarted", handler.isRunning()}});

			/* The two together are the useful number, so the effective period is
			   reported whenever the target has told us its time base. */
			if (handler.isDetected() && settings.timestepNs != 0)
				result["sample_period_ns"] = static_cast<uint64_t>(settings.timestepNs) * (config.skippedSamples + 1);

			return result;
		}
	}  // namespace

	void RecorderTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "set_recorder_enabled",
					  .description = "Enable or disable the recorder subsystem globally.",
					  .inputSchema = objectSchema(Json::object({{"enabled", booleanProperty("1 to enable, 0 to disable")}}), {"enabled"}),
					  .handler = setRecorderEnabled});

		registry.add({.name = "detect_recorder",
					  .description = "Return recorder parameters detected from the MCU as JSON: {version, revision, timestep_ns, sampling_hz, max_buf_size, max_variables, float_support}. Values are zero before the first successful acquisition.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = detectRecorder});

		registry.add({.name = "get_recorder_settings",
					  .description = "Return recorder settings of the active group as JSON: {group, mode, skipped_samples, trigger_enabled, trigger_edge, trigger_value, pre_trigger_samples, trigger_source}.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = getRecorderSettings});

		registry.add({.name = "set_recorder_mode",
					  .description = "Set recorder mode for the active recorder group. Acquisition is restarted if running. "
									 "Works like an oscilloscope. Trigger required in 'normal' and 'single' modes.",
					  .inputSchema = objectSchema(Json::object({{"mode", enumProperty("'run', 'normal', or 'single'", {"run", "normal", "single"})}}), {"mode"}),
					  .handler = setRecorderMode});

		registry.add({.name = "set_recorder_trigger",
					  .description = "Configure trigger for the active recorder group. All fields optional - only provided fields updated. ",
					  .inputSchema = objectSchema(Json::object({{"source_variable", stringProperty("Var as trigger source")},
																{"edge", enumProperty("'rising' or 'falling'", {"rising", "falling"})},
																{"value", numberProperty("Trigger threshold value")},
																{"pre_trigger_samples", integerProperty("Samples captured before the trigger event")},
																{"trigger_enabled", booleanProperty("1 = enable trigger, 0 = disable")}})),
					  .handler = setRecorderTrigger});

		registry.add({.name = "set_recorder_downsampling",
					  .description = "Set samples to skip between recorded samples (0 = no downsampling). Acquisition is restarted if running.",
					  .inputSchema = objectSchema(Json::object({{"skipped_samples", integerProperty("Samples to skip (0 = none)")}}), {"skipped_samples"}),
					  .handler = setRecorderDownsampling});
	}
}  // namespace mcp
