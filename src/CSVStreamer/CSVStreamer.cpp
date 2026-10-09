#include "CSVStreamer.hpp"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

bool CSVStreamer::Buffer::appendLine(std::string& line)
{
	if (isFull())
		return false;

	buffer[index] = line;
	index++;
	return true;
}

bool CSVStreamer::Buffer::isFull() const
{
	return (index + 1) >= buffer.size();
}

CSVStreamer::CSVStreamer(spdlog::logger* logger, const std::string& logPrefix) : logger(logger), logPrefix(logPrefix)
{
	currentBuffer = &buffer1;
	buffer1.nextBuffer = &buffer2;
	buffer2.nextBuffer = &buffer1;
}

CSVStreamer::~CSVStreamer()
{
	finishLogging();
}

/* A file name for the run that is starting: the prefix these logs carry in this
   build, and the moment the run began, written so that a directory listing
   sorts in the order the runs happened. No character a file system refuses. */
std::string CSVStreamer::makeRunFilePath(const std::string& directory, uint32_t attempt) const
{
	const std::time_t now = std::time(nullptr);
	std::tm local{};

#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	char stamp[32]{};
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

	const std::string suffix = attempt == 0 ? "" : "-" + std::to_string(attempt);

	return directory + "/" + logPrefix + "_" + stamp + suffix + ".csv";
}

bool CSVStreamer::prepareFile(const std::string& directory)
{
	/* A file that is already open is the one this run is being written to, and
	   it is left alone. The call comes once when a run starts and again on
	   every restart inside it, and the lines of one run belong in one file.
	   Opening a second time would cut off what has been written so far, and
	   because a stream that is open refuses to be opened again, the attempt
	   would instead leave it in a failed state that drops every line after it
	   without a word. */
	if (csvFile.is_open())
	{
		if (directory == logDirectory)
			return true;

		/* The log was pointed at another directory while the run was going.
		   What has been collected is written out and the file closed, so that
		   the part of the run before the change is kept where it was asked
		   for, and the rest continues in the new one. */
		finishLogging();
	}

	/* A run gets a file of its own, named for the moment it began: what a
	   reader collects from one run is then not cut short by the next one
	   starting, and the directory keeps the history of the runs rather than the
	   last of them. A run that begins in the second another one did is told
	   apart from it by a number rather than by cutting it off. */
	logDirectory = directory;
	filePath = makeRunFilePath(directory);

	/* Asked without a throwing overload: this runs on the acquisition thread,
	   where an exception has nowhere to go. A path that cannot be looked at is
	   taken as one that is not taken, and the file is opened over it. */
	std::error_code existsError;

	for (uint32_t attempt = 2; std::filesystem::exists(filePath, existsError); attempt++)
		filePath = makeRunFilePath(directory, attempt);

	csvFile.open(filePath, std::ios::out);
	if (!csvFile.is_open())
	{
		logger->error("Failed to open file: {}", filePath);
		return false;
	}

	/* A new file has no columns named in it yet. */
	headerWritten = false;

	return true;
}

void CSVStreamer::createHeader(const std::vector<std::string>& headerNames)
{
	/* Written once for each set of columns. A restart in the middle of a run
	   asks for it again, and the columns are usually still the same ones:
	   another header line there is a line a reader of the file has to step
	   over. When a plot has gained or lost a variable the columns differ, and
	   then the line is what marks where they changed. */
	if (headerWritten && headerNames == this->headerNames)
		return;

	this->headerNames = headerNames;
	headerWritten = true;

	std::string header = "time,";
	for (const auto& value : headerNames)
	{
		header += value + ",";
	}
	header.back() = '\n';
	currentBuffer->appendLine(header);
}

void CSVStreamer::writeLine(double time, std::unordered_map<std::string, double>& valuesMap)
{
	std::string line = std::to_string(time) + ",";
	for (const auto& name : headerNames)
	{
		line += std::to_string(valuesMap[name]) + ",";
	}
	line.back() = '\n';

	currentBuffer->appendLine(line);

	if (currentBuffer->isFull())
	{
		exchangeBuffers();

		if (saveTask.valid() && saveTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			logger->error("Buffer overrun in CSVStreamer object!");

		saveTask = std::async(std::launch::async, &CSVStreamer::writeFile, this);
	}
}

void CSVStreamer::exchangeBuffers()
{
	processingBuffer = currentBuffer;
	currentBuffer = currentBuffer->nextBuffer;
	currentBuffer->index = 0;
}

void CSVStreamer::writeFile()
{
	if (!csvFile.is_open())
	{
		logger->error("CSV file is not open!");
		return;
	}

	for (size_t i = 0; i < processingBuffer->index; i++)
	{
		csvFile << processingBuffer->buffer[i];
	}
}

void CSVStreamer::finishLogging()
{
	if (csvFile.is_open())
	{
		exchangeBuffers();
		writeFile();
		csvFile.close();
	}
}