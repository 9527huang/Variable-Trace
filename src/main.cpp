
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>

#include "../commons.hpp"
#include "CLI11.hpp"
#include "ConfigHandler.hpp"
#include "GlobalConfig.hpp"
#include "Gui.hpp"
#include "NFDFileHandler.hpp"
#include "ProjectHandler.hpp"
#include "VariableHandler.hpp"
#include "gitversion.hpp"
#include "spdlog/sinks/rotating_file_sink.h"
#include <exception>
#include <cstdlib>
#include "spdlog/spdlog.h"

std::atomic<bool> done = false;
std::mutex mtx;
std::shared_ptr<spdlog::logger> logger;
CLI::App app{"Variable-Trace"};


/*
 * What was going on when it died, in the file it was already writing.
 *
 * A windowed program that faults leaves nothing behind: no message, no stack,
 * no line in the log - the window is simply gone, and what remains is a fault
 * offset in the event log that has to be mapped back by hand. That is how the
 * last three faults were diagnosed, and it cost more than the fixes did.
 *
 * This catches both ways out: an unhandled Windows exception (an access
 * violation, a divide by zero) and a C++ exception nobody caught. Both are
 * written to the log with the code, the address, and the module base - the
 * offset in the event log is exactly address minus base, so the two records
 * name the same place in the same binary.
 */
#if defined(_WIN32)
#include <windows.h>

static LONG WINAPI logFatalException(EXCEPTION_POINTERS* info)
{
	if (info != nullptr && info->ExceptionRecord != nullptr && logger != nullptr)
	{
		const auto* record = info->ExceptionRecord;
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto at = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);

		logger->critical("Fatal: exception {:#010x} at {:#018x} (offset {:#x} from the module base {:#018x}).", static_cast<uint32_t>(record->ExceptionCode),
		                 static_cast<uint64_t>(at), static_cast<uint64_t>(at - base), static_cast<uint64_t>(base));

		/* The module a fault landed in is not always this one: a driver or a
		   runtime can take the process down. Naming the image says which binary
		   the offset above belongs to. */
		char path[MAX_PATH] = {};

		if (GetModuleFileNameA(GetModuleHandleW(nullptr), path, MAX_PATH) > 0)
			logger->critical("Fatal: the process image is '{}'.", path);

		logger->flush();
		spdlog::shutdown();
	}

	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void installCrashHandler()
{
#if defined(_WIN32)
	SetUnhandledExceptionFilter(logFatalException);
#endif

	std::set_terminate(
	    []()
	    {
		    if (logger != nullptr)
		    {
			    logger->critical("Fatal: terminate called - an exception nobody caught, or an assertion.");
			    logger->flush();
			    spdlog::shutdown();
		    }

		    std::abort();
	    });
}

void prepareLogger();
void prepareCLIParser(bool& debug, std::string& projectPath);

int main(int argc, char** argv)
{
	bool debug = false;
	std::string projectPath = "";
	prepareCLIParser(debug, projectPath);

	CLI11_PARSE(app, argc, argv);

	prepareLogger();
	installCrashHandler();

	if (debug)
		logger->set_level(spdlog::level::debug);
	else
		logger->set_level(spdlog::level::info);

	logger->info("Starting Variable-Trace!");
	logger->info("Version: {}.{}.{}", VARIABLE_TRACE_VERSION_MAJOR, VARIABLE_TRACE_VERSION_MINOR, VARIABLE_TRACE_VERSION_REVISION);
	logger->info("Commit hash {}", GIT_HASH);

	auto loggerPtr = logger.get();

	PlotGroupHandler plotGroupHandler;
	VariableHandler variableHandler;
	PlotHandler plotHandler;
	PlotHandler tracePlotHandler;

	ViewerDataHandler viewerDataHandler(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, done, &mtx, loggerPtr);
	TraceDataHandler traceDataHandler(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, done, &mtx, loggerPtr);

	/* The INI reader is kept for the legacy .cfg projects only. */
	ConfigHandler configHandler("", &plotHandler, &tracePlotHandler, &plotGroupHandler, &variableHandler, &viewerDataHandler, &traceDataHandler, loggerPtr);

	ProjectHandler projectHandler(&variableHandler, &plotHandler, &plotGroupHandler, loggerPtr);

	GlobalConfig globalConfig(loggerPtr);
	globalConfig.load();
	globalConfig.removeMissingRecentProjects();

	NFDFileHandler fileHandler;

	Gui gui(&plotHandler, &variableHandler, &configHandler, &projectHandler, &globalConfig, &plotGroupHandler, &fileHandler, &tracePlotHandler, &viewerDataHandler, &traceDataHandler, done, &mtx, loggerPtr, projectPath);

	while (!done)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	logger->info("Closing Variable-Trace!");
	logger->flush();
	spdlog::shutdown();
	return 0;
}

void prepareLogger()
{
#if defined(__APPLE__) || defined(_UNIX)
	std::string logDirectory = std::string(std::getenv("HOME")) + "/Variable-Trace/logs/logfile.txt";
#elif _WIN32
	std::string logDirectory = std::string(std::getenv("APPDATA")) + "/Variable-Trace/logs/logfile.txt";
#else
#error "Your system is not supported!"
#endif

	spdlog::sinks_init_list sinkList = {std::make_shared<spdlog::sinks::stdout_color_sink_mt>(),
										std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logDirectory, 5 * 1024 * 1024, 10)};
	logger = std::make_shared<spdlog::logger>("logger", sinkList.begin(), sinkList.end());

	/* The file sink writes into the buffer of its FILE handle, and this copy
	   of spdlog leaves the flush level at off, so a line would sit there until
	   the logger happens to be flushed - which this program only does on the
	   way out. A run that ends badly, a crash or a process killed from
	   outside, never reaches that flush, and the log of it would be empty.
	   The console sink hides this, because it flushes itself on every write.

	   Flushing on every level is what makes the log a record of the runs that
	   ended badly and not only of the ones that ended well. The volume is
	   small - the acquisition loop logs nothing per sample - so the cost is
	   one write syscall per line. */
	logger->flush_on(spdlog::level::trace);

	spdlog::register_logger(logger);
}

void prepareCLIParser(bool& debug, std::string& projectPath)
{
	app.fallthrough();
	app.ignore_case();
	app.add_flag("-d,--debug", debug, "Use for extra debug messages and logs");
	app.add_option("-p,--project", projectPath, "Use to open a project directly from command line");
}
