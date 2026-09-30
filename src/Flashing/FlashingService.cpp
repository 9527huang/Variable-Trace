#include "FlashingService.hpp"

#include <algorithm>
#include <utility>

#include "spdlog/spdlog.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	/* Between two looks at the pipe. Short enough that an abort is felt as
	   immediate, long enough that an idle flash costs nothing measurable. */
	constexpr int32_t pollIntervalMilliseconds = 10;

	/* How long the child is given to go away after it was asked to stop. */
	constexpr uint32_t terminationGraceMilliseconds = 2000;

	/* How many empty polls are allowed after the process is gone before the run
	   is called finished. cmd.exe waits for the program it started, so a process
	   that has exited has normally taken its output with it; the count only
	   matters for a detached grandchild that still holds the pipe. */
	constexpr int32_t exitDrainPolls = 200;

	bool containsSpace(const std::string& text)
	{
		return text.find(' ') != std::string::npos || text.find('\t') != std::string::npos;
	}

	/* A path that a shell would split has to be quoted, unless the template
	   already quotes the marker, in which case quoting again would nest them. */
	std::string replacementFor(const std::string& command, const std::string& file)
	{
		const bool markerIsQuoted = command.find(std::string("\"") + FlashingService::filePlaceholder + "\"") != std::string::npos;

		if (markerIsQuoted || !containsSpace(file))
			return file;

		return "\"" + file + "\"";
	}

	void trimTrailingBlanks(std::string& text)
	{
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
			text.pop_back();
	}

	/* Splits what has been read into whole lines, holding the last partial line
	   back so that the panel never shows a torn one. */
	bool extractLines(std::string& pending, std::vector<std::string>& lines)
	{
		size_t newline = pending.find('\n');

		if (newline == std::string::npos)
			return false;

		while (newline != std::string::npos)
		{
			std::string line = pending.substr(0, newline);
			pending.erase(0, newline + 1);

			if (!line.empty() && line.back() == '\r')
				line.pop_back();

			lines.push_back(std::move(line));
			newline = pending.find('\n');
		}

		return true;
	}
}  // namespace

FlashingService::FlashingService(spdlog::logger* logger) : logger(logger)
{
}

FlashingService::~FlashingService()
{
	/* A worker that is still reading would write into members that are about to
	   go away, so it is stopped and reaped first. */
	abort();
	waitForCompletion();
}

std::string FlashingService::buildCommandLine(const std::string& commandTemplate, const std::string& file)
{
	std::string command = commandTemplate;

	/* Trailing blanks are invisible in the settings field and would hand the
	   shell an empty trailing argument. */
	trimTrailingBlanks(command);

	if (command.empty())
		return "";

	const std::string placeholder = filePlaceholder;
	const std::string replacement = replacementFor(command, file);

	if (command.find(placeholder) == std::string::npos)
	{
		if (file.empty())
			return command;

		return command + " " + replacement;
	}

	/* Every occurrence is replaced, because a command that programs two regions
	   from one file would otherwise keep a literal marker in it. */
	std::string result;
	size_t at = 0;

	while (true)
	{
		const size_t found = command.find(placeholder, at);

		if (found == std::string::npos)
		{
			result += command.substr(at);
			break;
		}

		result += command.substr(at, found - at);
		result += replacement;
		at = found + placeholder.size();
	}

	return result;
}

/* The return type is written out in full: a name in front of the declarator is
   looked up in the enclosing namespace, not in the class. */
std::future<FlashingService::FlashResult> FlashingService::startFlash(const std::string& commandTemplate, const std::string& file, int32_t timeoutSeconds, std::function<void()> onUpdate)
{
	/* Refused before anything is touched, so a second caller never disturbs the
	   run that is under way. */
	{
		std::lock_guard<std::mutex> guard(mutex);

		if (state == State::Running)
			return {};
	}

	const std::string commandLine = buildCommandLine(commandTemplate, file);

	if (commandLine.empty())
	{
		std::lock_guard<std::mutex> guard(mutex);
		lastError = "Flash command is empty";
		state = State::Failed;
		lastResult = {};
		lastResult.state = State::Failed;
		return {};
	}

	/* Reaping the previous worker is done without the state lock, because that
	   worker may still be publishing its last line under it. */
	std::lock_guard<std::mutex> startGuard(startMutex);

	{
		std::lock_guard<std::mutex> guard(mutex);

		/* Re-checked: another caller may have started a run while this one waited
		   for the start lock. */
		if (state == State::Running)
			return {};
	}

	if (worker.joinable())
		worker.join();

	auto promise = std::make_shared<std::promise<FlashResult>>();
	std::future<FlashResult> future = promise->get_future();

	{
		std::lock_guard<std::mutex> guard(mutex);

		state = State::Running;
		lastError.clear();
		output.clear();
		droppedLines = 0;
		lastOutputTime = std::chrono::steady_clock::now();
	}

	abortRequested.store(false, std::memory_order_relaxed);
	outputChanged.store(false, std::memory_order_relaxed);

	worker = std::thread(&FlashingService::run, this, commandLine, timeoutSeconds, promise, onUpdate);

	return future;
}

bool FlashingService::abort()
{
	std::lock_guard<std::mutex> guard(mutex);

	if (state != State::Running)
		return false;

	abortRequested.store(true, std::memory_order_relaxed);

	/* The worker is the one that reads the pipe, so the kill happens here and
	   the worker finds the pipe closed on its next look. */
	terminateProcess();

	return true;
}

bool FlashingService::isRunning()
{
	std::lock_guard<std::mutex> guard(mutex);
	return state == State::Running;
}

FlashingService::State FlashingService::getState()
{
	std::lock_guard<std::mutex> guard(mutex);
	return state;
}

std::vector<std::string> FlashingService::getOutput()
{
	std::lock_guard<std::mutex> guard(mutex);
	return std::vector<std::string>(output.begin(), output.end());
}

size_t FlashingService::getDroppedLineCount()
{
	std::lock_guard<std::mutex> guard(mutex);
	return droppedLines;
}

size_t FlashingService::getOutputRevision()
{
	std::lock_guard<std::mutex> guard(mutex);

	/* Every line either lands in the deque or is counted as dropped, so the
	   sum grows by exactly one per line. */
	return output.size() + droppedLines;
}

std::string FlashingService::getLastError()
{
	std::lock_guard<std::mutex> guard(mutex);
	return lastError;
}

FlashingService::FlashResult FlashingService::getLastResult()
{
	std::lock_guard<std::mutex> guard(mutex);
	return lastResult;
}

void FlashingService::waitForCompletion()
{
	/* Worth dropping the lock for: the worker needs it to publish its result,
	   and joining while holding it would wait on a thread that cannot finish. */
	if (worker.joinable())
		worker.join();
}

std::string FlashingService::stateToString(State state)
{
	switch (state)
	{
		case State::Idle:
			return "idle";
		case State::Running:
			return "running";
		case State::Success:
			return "success";
		case State::Failed:
			return "failed";
		case State::Aborted:
			return "aborted";
	}

	return "idle";
}

void FlashingService::appendLine(const std::string& line)
{
	{
		std::lock_guard<std::mutex> guard(mutex);

		output.push_back(line);

		/* The panel shows the tail, so the oldest line is the one to give up. */
		while (output.size() > maximumOutputLines)
		{
			output.pop_front();
			droppedLines++;
		}
	}

	outputChanged.store(true, std::memory_order_relaxed);
}

void FlashingService::run(const std::string& commandLine, int32_t timeoutSeconds, const std::shared_ptr<std::promise<FlashResult>>& promise, const std::function<void()>& onUpdate)
{
	FlashResult result;
	result.state = State::Failed;

	const auto startedAt = std::chrono::steady_clock::now();

	if (!startProcess(commandLine))
	{
		if (logger != nullptr)
			logger->error("Flash command could not be started: {}", commandLine);

		{
			std::lock_guard<std::mutex> guard(mutex);
			state = State::Failed;
			lastError = "Flash command could not be started";
			lastResult = result;
		}

		promise->set_value(result);
		return;
	}

	if (logger != nullptr)
		logger->info("Flash started: {}", commandLine);

	std::string pending;
	int32_t exitCode = -1;
	bool timedOut = false;

	/* Counted so that a grandchild holding the writing end of the pipe open does
	   not keep the run alive after the shell itself has gone. */
	int32_t emptyPollsAfterExit = 0;

	while (true)
	{
		if (abortRequested.load(std::memory_order_relaxed))
			break;

		ProcessState processState{};
		std::vector<std::string> lines;

		readAvailable(pending, processState, lines);

		for (const std::string& line : lines)
			appendLine(line);

		if (processState.producedOutput)
		{
			std::lock_guard<std::mutex> guard(mutex);
			lastOutputTime = std::chrono::steady_clock::now();
		}

		if (outputChanged.exchange(false, std::memory_order_relaxed) && onUpdate)
			onUpdate();

		if (processState.processExited && processState.exitCode != -1)
			exitCode = processState.exitCode;

		/* The pipe closing is the reliable end of the output. */
		if (processState.pipeClosed)
			break;

		if (processState.processExited)
		{
			if (!processState.producedOutput && ++emptyPollsAfterExit >= exitDrainPolls)
				break;
		}
		else
			emptyPollsAfterExit = 0;

		if (timeoutSeconds > 0)
		{
			std::lock_guard<std::mutex> guard(mutex);

			const double idleSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastOutputTime).count();

			if (idleSeconds >= static_cast<double>(timeoutSeconds))
			{
				timedOut = true;

				if (logger != nullptr)
					logger->warn("Flash timed out: no output for {} s", timeoutSeconds);

				break;
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMilliseconds));
	}

	const bool wasAborted = abortRequested.load(std::memory_order_relaxed);

	if (wasAborted || timedOut)
		terminateProcess();

	/* A last line that never received its newline is still output. */
	if (!pending.empty())
	{
		if (pending.back() == '\r')
			pending.pop_back();

		appendLine(pending);
		pending.clear();
	}

	const int32_t processExitCode = closeProcess();

	/* The pipe can close a moment before the child is reaped, so a run whose
	   code was not read in time would be reported as a failure. The wait inside
	   closeProcess() is what closes that gap; a code seen earlier always wins. */
	if (exitCode == -1)
		exitCode = processExitCode;

	if (outputChanged.exchange(false, std::memory_order_relaxed) && onUpdate)
		onUpdate();

	result.exitCode = exitCode;
	result.timedOut = timedOut;
	result.durationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();

	if (wasAborted)
	{
		result.state = State::Aborted;
		result.exitCode = -1;
	}
	else if (timedOut)
		result.state = State::Failed;
	else if (exitCode == 0)
		result.state = State::Success;
	else
		result.state = State::Failed;

	{
		std::lock_guard<std::mutex> guard(mutex);

		state = result.state;
		lastResult = result;

		if (result.state == State::Aborted)
			lastError = "Flash aborted by user.";
		else if (timedOut)
			lastError = "Flash timed out: no output for " + std::to_string(timeoutSeconds) + " s";
		else if (result.state == State::Failed)
			lastError = "Flash command exited with code " + std::to_string(result.exitCode);
		else
			lastError.clear();
	}

	if (logger != nullptr)
		logger->info("Flash finished: {} after {:.2f} s", stateToString(result.state), result.durationSeconds);

	promise->set_value(result);
}

#ifdef _WIN32

bool FlashingService::startProcess(const std::string& commandLine)
{
	SECURITY_ATTRIBUTES attributes{};
	attributes.nLength = sizeof(attributes);
	attributes.bInheritHandle = TRUE;
	attributes.lpSecurityDescriptor = nullptr;

	HANDLE readPipe = nullptr;
	HANDLE writePipe = nullptr;

	if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
		return false;

	/* The child needs the write end; the read end must not be inherited, or the
	   pipe would never report the last writer as gone. */
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	startup.hStdOutput = writePipe;
	startup.hStdError = writePipe;
	startup.hStdInput = nullptr;

	/* cmd.exe /c turns the user's template into something runnable, and is what
	   lets a template chain several steps the way a terminal would. */
	const std::string fullCommandLine = "cmd.exe /c " + commandLine;

	std::vector<char> commandBuffer(fullCommandLine.begin(), fullCommandLine.end());
	commandBuffer.push_back('\0');

	PROCESS_INFORMATION processInfo{};

	const BOOL created = CreateProcessA(nullptr, commandBuffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo);

	/* The parent's copy of the write end has to go, otherwise the read end never
	   sees the end of the output even after the child is gone. */
	CloseHandle(writePipe);

	if (!created)
	{
		CloseHandle(readPipe);
		return false;
	}

	CloseHandle(processInfo.hThread);

	pipeHandle = readPipe;
	processHandle = processInfo.hProcess;

	return true;
}

void FlashingService::readAvailable(std::string& pending, ProcessState& processState, std::vector<std::string>& lines)
{
	HANDLE readPipe = static_cast<HANDLE>(pipeHandle);
	HANDLE process = static_cast<HANDLE>(processHandle);

	for (;;)
	{
		DWORD available = 0;

		if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr))
		{
			/* The write end is gone and nothing is buffered, so no more output
			   can arrive. */
			processState.pipeClosed = true;
			break;
		}

		if (available == 0)
			break;

		char buffer[4096];
		const DWORD toRead = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
		DWORD got = 0;

		if (!ReadFile(readPipe, buffer, toRead, &got, nullptr) || got == 0)
		{
			processState.pipeClosed = true;
			break;
		}

		pending.append(buffer, got);
		processState.producedOutput = true;
	}

	extractLines(pending, lines);

	DWORD code = 0;

	if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE)
	{
		processState.processExited = true;
		processState.exitCode = static_cast<int32_t>(code);
	}
}

void FlashingService::terminateProcess()
{
	if (processHandle != nullptr)
		TerminateProcess(static_cast<HANDLE>(processHandle), 1);
}

int32_t FlashingService::closeProcess()
{
	if (pipeHandle != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(pipeHandle));
		pipeHandle = nullptr;
	}

	int32_t exitCode = -1;

	if (processHandle != nullptr)
	{
		HANDLE process = static_cast<HANDLE>(processHandle);

		/* Waiting before the handle is closed is what makes the run really over
		   rather than about to be over, and it is also what makes the exit code
		   readable: the pipe can close a moment before the process is reaped,
		   and the code would otherwise be lost. */
		WaitForSingleObject(process, terminationGraceMilliseconds);

		DWORD code = 0;

		if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE)
			exitCode = static_cast<int32_t>(code);

		CloseHandle(process);
		processHandle = nullptr;
	}

	return exitCode;
}

#else

bool FlashingService::startProcess(const std::string& commandLine)
{
	int descriptors[2];

	if (::pipe(descriptors) != 0)
		return false;

	const pid_t pid = ::fork();

	if (pid < 0)
	{
		::close(descriptors[0]);
		::close(descriptors[1]);
		return false;
	}

	if (pid == 0)
	{
		/* The child puts itself in a process group of its own, so that stopping
		   the flash reaches whatever the shell started and not only the shell. */
		::setpgid(0, 0);

		::dup2(descriptors[1], STDOUT_FILENO);
		::dup2(descriptors[1], STDERR_FILENO);
		::close(descriptors[0]);
		::close(descriptors[1]);

		::execl("/bin/sh", "sh", "-c", commandLine.c_str(), static_cast<char*>(nullptr));

		/* Reached only when the shell itself could not be started. */
		::_exit(127);
	}

	::close(descriptors[1]);

	/* The same thread reads the pipe and watches for an abort, so the read may
	   not block. */
	const int flags = ::fcntl(descriptors[0], F_GETFL, 0);

	if (flags >= 0)
		::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK);

	pipeDescriptor = descriptors[0];
	childPid = static_cast<int64_t>(pid);

	return true;
}

void FlashingService::readAvailable(std::string& pending, ProcessState& processState, std::vector<std::string>& lines)
{
	char buffer[4096];

	for (;;)
	{
		const ssize_t got = ::read(pipeDescriptor, buffer, sizeof(buffer));

		if (got > 0)
		{
			pending.append(buffer, static_cast<size_t>(got));
			processState.producedOutput = true;
			continue;
		}

		if (got == 0)
			processState.pipeClosed = true;
		else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			processState.pipeClosed = true;

		break;
	}

	extractLines(pending, lines);

	if (childPid > 0)
	{
		int status = 0;
		const pid_t waited = ::waitpid(static_cast<pid_t>(childPid), &status, WNOHANG);

		if (waited == static_cast<pid_t>(childPid))
		{
			processState.processExited = true;

			if (WIFEXITED(status))
				processState.exitCode = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				processState.exitCode = 128 + WTERMSIG(status);

			/* Reaped, so nothing may wait on it again. */
			childPid = -1;
		}
	}
	else
	{
		/* Already reaped by an earlier look, so the code it had is remembered by
		   the caller and must not be overwritten with a guess. */
		processState.processExited = true;
		processState.exitCode = -1;
	}
}

void FlashingService::terminateProcess()
{
	if (childPid > 0)
		::kill(-static_cast<pid_t>(childPid), SIGTERM);
}

int32_t FlashingService::closeProcess()
{
	if (pipeDescriptor >= 0)
	{
		::close(pipeDescriptor);
		pipeDescriptor = -1;
	}

	if (childPid > 0)
	{
		int status = 0;
		::kill(-static_cast<pid_t>(childPid), SIGKILL);
		::waitpid(static_cast<pid_t>(childPid), &status, 0);
		childPid = -1;

		/* Reaching here means the child was still alive when the loop ended,
		   which is the timed out and the aborted case, and neither has a code
		   worth reporting. */
		return -1;
	}

	return -1;
}

#endif
