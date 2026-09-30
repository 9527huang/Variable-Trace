#ifndef _FLASHINGSERVICE_HPP
#define _FLASHINGSERVICE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
 * Runs a programmer as a child process and streams what it prints.
 *
 * The command is a template held in the global settings. A "{file}" in it is
 * replaced by the firmware path, which is how one template serves both a file
 * the user picked and the *.elf the acquisition settings already point at.
 *
 * The output is kept in a bounded list of lines, because a programmer can print
 * for minutes and the panel only ever shows the tail. A flash erases and
 * rewrites the target, so only one run may be in flight at a time.
 *
 * The process is started through a shell: "cmd.exe /c" on Windows, "sh -c"
 * elsewhere. That is what lets the user write a command the way they would in a
 * terminal, including a chain of steps.
 */
namespace spdlog
{
	class logger;
}

class FlashingService
{
   public:
	enum class State : uint8_t
	{
		Idle = 0,
		Running = 1,
		Success = 2,
		Failed = 3,
		Aborted = 4,
	};

	struct FlashResult
	{
		State state = State::Idle;
		int32_t exitCode = -1;
		double durationSeconds = 0.0;
		/* True when the run was cut short because it stopped printing, which
		   tells a caller apart from a command that simply failed. */
		bool timedOut = false;
	};

	/* What the settings panel edits and the global config keeps. */
	struct FlashSettings
	{
		std::string command = "";
		std::string file = "";
		bool useElfFile = false;
		bool timeoutEnabled = false;
		int32_t timeoutSeconds = 60;
	};

	/* Marker in the command template that the firmware path replaces. */
	static constexpr const char* filePlaceholder = "{file}";

	/* Lines kept for the panel. A programmer writing a megabyte of output should
	   not grow the process without limit, and nobody reads back that far. */
	static constexpr size_t maximumOutputLines = 2000;

	static constexpr int32_t minimumTimeoutSeconds = 1;
	static constexpr int32_t maximumTimeoutSeconds = 3600;

	/* Windows caps a command line at 32767 characters, and a shell that is handed
	   more refuses to start at all. */
	static constexpr size_t maximumCommandLineLength = 32767;

	explicit FlashingService(spdlog::logger* logger = nullptr);
	~FlashingService();

	FlashingService(const FlashingService&) = delete;
	FlashingService& operator=(const FlashingService&) = delete;

	/* Substitutes the firmware path into the template. A template without the
	   marker gets the path appended, which is what a bare programmer invocation
	   like "openocd -f x.cfg" needs to become usable. A path with a space is
	   quoted unless the template already quotes the marker. */
	static std::string buildCommandLine(const std::string& commandTemplate, const std::string& file);

	/* Starts a run and returns at once. The future carries the result, and
	   onUpdate is called whenever a new line has arrived, which is what lets a
	   panel redraw without polling on a timer.
	   The returned future is invalid when a run is already in flight or the
	   command line is empty; check with future.valid(). A caller that must tell
	   the two apart asks isRunning() first. */
	std::future<FlashResult> startFlash(const std::string& commandTemplate, const std::string& file, int32_t timeoutSeconds, std::function<void()> onUpdate = {});

	/* Asks the running child to stop. Returns false when nothing is running. The
	   call does not wait for the process to go away; the future reports the
	   aborted result. */
	bool abort();

	bool isRunning();

	State getState();

	/* A copy of everything printed so far, oldest line first. */
	std::vector<std::string> getOutput();

	/* How many lines have passed through the buffer so far, dropped ones
	   included. It only ever grows while a run is going on and resets when the
	   next one starts, which is enough for a panel to tell whether its copy of
	   the buffer is still current. */
	size_t getOutputRevision();

	/* How many lines were dropped because the buffer is bounded. */
	size_t getDroppedLineCount();

	/* Why the last run failed, empty when it did not. */
	std::string getLastError();

	/* The result of the last finished run, so a client that polls after the fact
	   still sees the outcome instead of an idle state. */
	FlashResult getLastResult();

	/* Waits for the current run to finish, so no worker outlives the object it
	   writes into. */
	void waitForCompletion();

	static std::string stateToString(State state);

   private:
	/* How far the child has got, filled by one look at the pipe. */
	struct ProcessState
	{
		bool producedOutput = false;
		bool pipeClosed = false;
		bool processExited = false;
		int32_t exitCode = -1;
	};

	void run(const std::string& commandLine, int32_t timeoutSeconds, const std::shared_ptr<std::promise<FlashResult>>& promise, const std::function<void()>& onUpdate);

	/* Hands one complete line to the output list. */
	void appendLine(const std::string& line);

	/* Platform specific. Starts the child; returns false when it could not be
	   started, in which case no handle is left behind. */
	bool startProcess(const std::string& commandLine);
	/* Platform specific. Reads whatever is buffered without blocking, appends
	   the whole lines it finds to lines, and reports how far the child has got.
	   Takes no lock, so the caller can hold one across the whole step. */
	void readAvailable(std::string& pending, ProcessState& processState, std::vector<std::string>& lines);
	/* Platform specific. Asks the child to stop. */
	void terminateProcess();
	/* Platform specific. Waits for the child and releases its handles. Returns
	   the exit code when the wait revealed one, and -1 when the code is already
	   known to the caller or was never available. */
	int32_t closeProcess();

   private:
	spdlog::logger* logger;

	mutable std::mutex mutex;
	/* Serialises the reap-and-start sequence without being held while the
	   worker publishes its last line. */
	std::mutex startMutex;

	std::thread worker;

	State state = State::Idle;
	FlashResult lastResult{};
	std::string lastError = "";

	std::deque<std::string> output;
	size_t droppedLines = 0;

	std::atomic<bool> abortRequested{false};
	/* Set by the reader whenever a line arrived, and cleared by the worker when
	   it has told the panel about it. */
	std::atomic<bool> outputChanged{false};

	/* When the child last printed something. Held under the mutex, because the
	   idle check and the reader run on the worker while a getter may ask for it
	   later. */
	std::chrono::steady_clock::time_point lastOutputTime{};

	/* Platform handles. Windows keeps a process and a pipe, POSIX a pid and a
	   descriptor; only one set is ever in use. Kept as plain integers so that
	   neither windows.h nor the POSIX headers have to be included here. */
	void* processHandle = nullptr;
	void* pipeHandle = nullptr;
	int32_t pipeDescriptor = -1;
	int64_t childPid = -1;
};

#endif
