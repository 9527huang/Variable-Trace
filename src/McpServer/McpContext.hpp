#ifndef _MCPCONTEXT_HPP
#define _MCPCONTEXT_HPP

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "McpTypes.hpp"

/*
 * The handlers are only ever held by pointer here, so they are forward declared
 * rather than included. That keeps this header free of the application's own
 * headers, which is what lets the protocol and the registry be built and tested
 * on their own. The translation units that actually call into a handler include
 * the matching header through McpTools.hpp.
 */
class VariableHandler;
class PlotHandler;
class PlotGroupHandler;
class ViewerDataHandler;
class TraceDataHandler;
class GlobalConfig;
class ProjectHandler;
class FlashingService;

namespace recorder
{
	class RecorderHandler;
}

namespace spdlog
{
	class logger;
}

/*
 * The surface a tool handler is allowed to use.
 *
 * Handlers that read or change the variable and plot model are executed on the
 * GUI thread, so they may work with the handlers directly - that is what keeps
 * them consistent with what the drawing code does. Operations that only touch
 * files, or that spawn a parser process, run on the request thread, which is why
 * the few actions that need GUI state are exposed as callbacks the application
 * installs. An empty callback means the capability is not available in this
 * build and the tool reports that instead of failing silently.
 */
struct McpContext
{
	/* ---------------------------------------------------------------- model */
	VariableHandler* variableHandler = nullptr;
	PlotHandler* plotHandler = nullptr;
	PlotHandler* tracePlotHandler = nullptr;
	PlotGroupHandler* plotGroupHandler = nullptr;
	ViewerDataHandler* viewerDataHandler = nullptr;
	TraceDataHandler* traceDataHandler = nullptr;
	GlobalConfig* globalConfig = nullptr;
	ProjectHandler* projectHandler = nullptr;
	spdlog::logger* logger = nullptr;

	/* The recorder works through the probe the viewer already has open, so it
	   needs no connection of its own. The probe is taken from the viewer data
	   handler rather than held here, so that a change of probe type is picked up
	   without anything having to be kept in step. */
	recorder::RecorderHandler* recorderHandler = nullptr;

	/* The programmer is the same object the toolbar button uses, so a flash
	   started over the API is the run the output panel shows. It is a pointer
	   because the application owns it; a null one means this build has no
	   flashing at all. */
	FlashingService* flasher = nullptr;

	/* Guards the sample buffers and the probe. The plotting code takes it before
	   it copies a ring buffer, and the read tools do the same so that a copy is
	   taken while the acquisition thread is not writing. */
	std::mutex* mtx = nullptr;

	/* -------------------------------------------------- application actions */
	std::function<bool(const std::string& path)> openProject;
	std::function<bool(const std::string& path)> saveProjectAs;
	std::function<bool()> saveProject;

	/* Discards the current model and starts an empty project. */
	std::function<void()> newProject;

	/* Needs the application to keep the path relative to the project file. */
	std::function<bool(const std::string& path, bool relative)> setElfPath;

	/* Re-parses the ELF and refreshes the addresses of the tracked variables.
	   Runs on the request thread, exactly like the asynchronous refresh the
	   variable table already performs. */
	std::function<bool(const std::string& elfPath)> refreshVariableAddresses;

	/* Points the handler at the probe object that matches the configured type. */
	std::function<void()> selectProbeDevice;

	/* Gives the recorder the addresses of ____recorder and ____recorderSettings,
	   which move whenever another symbol file is loaded. Returns false when the
	   symbol file has no recorder in it. */
	std::function<bool()> syncRecorderSymbols;

	/* ---------------------------------------------------- read-only plumbing */
	std::function<std::string()> getProjectPath;
	std::function<std::string()> getElfPath;
	std::function<std::string()> getApplicationVersion;

	/* Port the server is currently listening on, so get_instance_info can report
	   the address a client should use for the next connection. */
	std::function<uint16_t()> getServerPort;

	/* May an API client write to the target at all. This is the outer guard; the
	   per-variable min/max limits are the inner one. */
	std::function<bool()> getApiWritesEnabled;
};

#endif
