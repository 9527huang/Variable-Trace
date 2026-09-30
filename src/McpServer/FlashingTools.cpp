#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "FlashingService.hpp"
#include "GlobalConfig.hpp"
#include "McpTools.hpp"
#include "ViewerDataHandler.hpp"

namespace mcp
{
	/*
	 * Flashing runs an external programmer and streams what it prints.
	 *
	 * The command, the firmware file and the inactivity timeout live in the
	 * global settings, because a programmer is a property of the machine rather
	 * than of the project. One call may override the command and the file for
	 * that run only, which is what makes "flash this build with that probe"
	 * possible without editing the settings first.
	 *
	 * The five tools all run on the request thread: none of them reads or
	 * changes the variable and plot model, and a flash takes minutes, so
	 * answering from a thread of its own is the point. Stopping the acquisition
	 * before a flash is the one exception, and that is a state flag the
	 * acquisition loop picks up by itself.
	 */
	namespace
	{
		FlashingService* requireFlasher(McpContext& context, const std::string& toolName)
		{
			if (context.flasher == nullptr)
				reportUnavailable("the firmware flashing subsystem", toolName);

			return context.flasher;
		}

		/* What the run will use. The two fields a single call may override are
		   replaced; everything else is what the settings window holds. */
		FlashingService::FlashSettings resolveSettings(McpContext& context, const Json& arguments)
		{
			FlashingService::FlashSettings settings{};

			if (context.globalConfig != nullptr)
				settings = context.globalConfig->getSettings().flash;

			if (has(arguments, "command"))
				settings.command = requireString(arguments, "command");

			if (has(arguments, "file"))
				settings.file = requireString(arguments, "file");

			return settings;
		}

		/* The firmware the {file} marker is replaced with. The *.elf of the
		   acquisition settings wins when the setting says to use it, because
		   that is what the user asked for by ticking the box. */
		std::string resolveFirmwareFile(McpContext& context, const FlashingService::FlashSettings& settings)
		{
			if (!settings.useElfFile)
				return settings.file;

			if (context.getElfPath == nullptr)
				return "";

			return context.getElfPath();
		}

		/* A timeout of zero means the service never gives up, which is what a
		   disabled timeout has to turn into. */
		int32_t resolveTimeout(const FlashingService::FlashSettings& settings)
		{
			if (!settings.timeoutEnabled)
				return 0;

			return std::clamp(settings.timeoutSeconds, FlashingService::minimumTimeoutSeconds, FlashingService::maximumTimeoutSeconds);
		}

		Json settingsToJson(const FlashingService::FlashSettings& settings)
		{
			return Json::object({{"command", settings.command},
								 {"file", settings.file},
								 {"use_elf_file", settings.useElfFile},
								 {"timeout_enabled", settings.timeoutEnabled},
								 {"timeout_seconds", settings.timeoutSeconds}});
		}

		Json flash(McpContext& context, const Json& arguments)
		{
			FlashingService* flasher = requireFlasher(context, "flash");

			if (flasher->isRunning())
				throw ToolError("Flash already in progress. Call abort_flash first or wait for it to finish.");

			const FlashingService::FlashSettings settings = resolveSettings(context, arguments);

			if (settings.command.empty())
				throw ToolError("No flash command configured. Provide 'command' or set it in Options -> Flashing.");

			const std::string file = resolveFirmwareFile(context, settings);

			if (file.empty())
				throw ToolError("No firmware file configured. Provide 'file' or set it in Options -> Flashing.");

			/* The target is erased and rewritten, so nothing may be reading it
			   while that happens. The acquisition is stopped here rather than
			   refused, because a caller that has to remember to stop it first
			   would sooner or later forget. */
			bool stoppedAcquisition = false;

			if (context.viewerDataHandler != nullptr && context.viewerDataHandler->getStateImmediate() == DataHandlerBase::State::RUN)
			{
				context.viewerDataHandler->setState(DataHandlerBase::State::STOP);
				stoppedAcquisition = true;
			}

			if (!flasher->startFlash(settings.command, file, resolveTimeout(settings)).valid())
				throw ToolError("Flash already in progress. Call abort_flash first or wait for it to finish.");

			return Json::object({{"state", "running"},
								 {"file", file},
								 {"command_line", FlashingService::buildCommandLine(settings.command, file)},
								 {"acquisition_stopped", stoppedAcquisition},
								 {"timeout_seconds", resolveTimeout(settings)},
								 {"message", "Flash started (acquisition stopped automatically). Call get_flash_status to poll for progress and result."}});
		}

		Json getFlashStatus(McpContext& context, const Json&)
		{
			FlashingService* flasher = requireFlasher(context, "get_flash_status");

			const FlashingService::State state = flasher->getState();
			const std::vector<std::string> lines = flasher->getOutput();

			/* One string rather than an array: the description promises a text
			   block, and a client that wants to show it does not have to join
			   anything itself. */
			std::string output;

			for (const std::string& line : lines)
			{
				output += line;
				output += '\n';
			}

			Json result{{"state", FlashingService::stateToString(state)}, {"output", output}};

			if (!lines.empty())
				result["line_count"] = lines.size();

			if (const size_t dropped = flasher->getDroppedLineCount(); dropped > 0)
				result["dropped_line_count"] = dropped;

			/* The outcome of the last run is repeated after it has finished, so
			   a client that polls late still gets the exit code and the reason
			   instead of an idle state with nothing attached. */
			if (state != FlashingService::State::Idle && state != FlashingService::State::Running)
			{
				const FlashingService::FlashResult last = flasher->getLastResult();
				result["exit_code"] = last.exitCode;
				result["duration_s"] = last.durationSeconds;

				if (last.timedOut)
					result["timed_out"] = true;

				const std::string error = flasher->getLastError();

				if (!error.empty())
					result["error"] = error;
			}

			return result;
		}

		Json abortFlash(McpContext& context, const Json&)
		{
			FlashingService* flasher = requireFlasher(context, "abort_flash");

			if (!flasher->isRunning())
				throw ToolError("No flash in progress.");

			/* The child is asked to stop, not killed outright, so that a
			   programmer holding the target's reset line has a chance to let go.
			   The result arrives through get_flash_status. */
			flasher->abort();

			return Json::object({{"state", "aborting"},
								 {"message", "Abort requested. Call get_flash_status to see the final state."}});
		}

		Json getFlashSettings(McpContext& context, const Json&)
		{
			requireFlasher(context, "get_flash_settings");

			if (context.globalConfig == nullptr)
				reportUnavailable("the global settings", "get_flash_settings");

			const FlashingService::FlashSettings& settings = context.globalConfig->getSettings().flash;

			Json result = settingsToJson(settings);

			/* Reported next to the setting that decides whether it is used, so a
			   client can tell "use the elf" from "use the elf, which is not set
			   yet" without a second call. */
			result["elf_file"] = (context.getElfPath == nullptr) ? "" : context.getElfPath();

			return result;
		}

		Json setFlashSettings(McpContext& context, const Json& arguments)
		{
			requireFlasher(context, "set_flash_settings");

			if (context.globalConfig == nullptr)
				reportUnavailable("the global settings", "set_flash_settings");

			FlashingService::FlashSettings& settings = context.globalConfig->getSettings().flash;

			if (const std::optional<std::string> command = optionalString(arguments, "command"); command.has_value())
				settings.command = *command;

			if (const std::optional<std::string> file = optionalString(arguments, "file"); file.has_value())
				settings.file = *file;

			if (const std::optional<bool> useElf = optionalBool(arguments, "use_elf_file"); useElf.has_value())
				settings.useElfFile = *useElf;

			if (const std::optional<bool> enabled = optionalBool(arguments, "timeout_enabled"); enabled.has_value())
				settings.timeoutEnabled = *enabled;

			if (const std::optional<int64_t> seconds = optionalInteger(arguments, "timeout_seconds"); seconds.has_value())
			{
				if (*seconds < FlashingService::minimumTimeoutSeconds || *seconds > FlashingService::maximumTimeoutSeconds)
					throw ToolError("'timeout_seconds' must be between " + std::to_string(FlashingService::minimumTimeoutSeconds) + " and " + std::to_string(FlashingService::maximumTimeoutSeconds) + ".");

				settings.timeoutSeconds = static_cast<int32_t>(*seconds);
			}

			/* Written through right away, because the settings belong to the
			   installation and the next launch has to find them. */
			context.globalConfig->save();

			return settingsToJson(settings);
		}
	}  // namespace

	void FlashingTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "flash",
					  .description = "Flash firmware to the target MCU using the configured flash command. Optionally override 'file' (firmware path) and 'command' for this run. When omitted, the values from project settings are used. Returns immediately; poll get_flash_status for progress and result.",
					  .inputSchema = objectSchema(Json::object({{"file", stringProperty("Override firmware file path for this flash (optional)")},
																{"command", stringProperty("Override flash command for this flash (optional)")}})),
					  .handler = flash,
					  /* The programmer runs for minutes and nothing here reads the
						 model, so the request thread answers and the interface keeps
						 drawing. */
					  .onGuiThread = false});

		registry.add({.name = "get_flash_status",
					  .description = "Poll the current flash operation status. Returns {state, output}: state='running' while in progress (output is live so far), state='success' or state='failed' when done (output is complete), state='aborted' when aborted, state='idle' when no flash has been started.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = getFlashStatus,
					  .onGuiThread = false});

		registry.add({.name = "abort_flash",
					  .description = "Abort the currently running flash operation.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = abortFlash,
					  .onGuiThread = false});

		registry.add({.name = "get_flash_settings",
					  .description = "Return current flash configuration: command, file path, whether to use ELF file, and timeout settings.",
					  .inputSchema = objectSchema(Json::object({})),
					  .handler = getFlashSettings,
					  .onGuiThread = false});

		registry.add({.name = "set_flash_settings",
					  .description = "Update flash configuration. All fields are optional - only provided fields are updated. "
									 "'command': flash command template; use {file} as placeholder for the firmware path. "
									 "'file': firmware file path (used when use_elf_file is false). "
									 "'use_elf_file' (0/1): use the ELF file from acquisition settings instead of 'file'. "
									 "'timeout_enabled' (0/1): enable inactivity timeout. "
									 "'timeout_seconds': inactivity timeout (1 - 3600).",
					  .inputSchema = objectSchema(Json::object({{"command", stringProperty("Flash command template with optional {file} placeholder")},
																{"file", stringProperty("Firmware file path")},
																{"use_elf_file", booleanProperty("1 = use ELF file, 0 = use 'file'")},
																{"timeout_enabled", booleanProperty("1 = enable timeout, 0 = disable")},
																{"timeout_seconds", integerProperty("Inactivity timeout in seconds (1 - 3600)")}})),
					  .handler = setFlashSettings,
					  /* Only a string in the global settings changes, which is
						 cheap enough to do inside the frame. */
					  .onGuiThread = false});
	}
}  // namespace mcp
