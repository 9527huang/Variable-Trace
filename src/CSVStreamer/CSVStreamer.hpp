#ifndef _CSV_STREAMER_HPP
#define _CSV_STREAMER_HPP

#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <unordered_map>

#include "spdlog/spdlog.h"

class CSVStreamer
{
   public:
	struct Buffer
	{
		bool appendLine(std::string& line);
		bool isFull() const;

		std::array<std::string, 1000> buffer;
		size_t index;
		Buffer* nextBuffer;
	};

	CSVStreamer(spdlog::logger* logger, const std::string& logPrefix = "logfile");
	~CSVStreamer();

	/// @brief Creates a file of its own for the run that is starting, in the given
	/// directory, and keeps it for the rest of that run
	/// @param directory directory string
	/// @return
	bool prepareFile(const std::string& directory);

	/// @brief create csv file header from given argument, first column - time - is added internally
	/// @param headerNames table headers
	void createHeader(const std::vector<std::string>& headerNames);

	/// @brief writes single line to internal buffer
	/// @param time
	/// @param valuesMap
	void writeLine(double time, std::unordered_map<std::string, double>& valuesMap);

	/// @brief exchanges the buffer that is being processed with the one that's being written to
	void exchangeBuffers();

	/// @brief writes the processing buffer to the opened csv file
	void writeFile();

	/// @brief writes the rest of the buffer to the file and closes it
	void finishLogging();

   private:
	/* The name of a run's file is built from this and the moment the run began,
	   so that a directory keeps the runs that were logged into it rather than
	   the last one of them. The second argument tells a run that began in the
	   same second as one already written apart from it. */
	std::string makeRunFilePath(const std::string& directory, uint32_t attempt = 0) const;

	spdlog::logger* logger;
	std::future<void> saveTask{};
	std::string logPrefix;
	std::string filePath;
	std::string logDirectory;
	std::ofstream csvFile;
	Buffer buffer1{};
	Buffer buffer2{};
	Buffer* currentBuffer;
	Buffer* processingBuffer;
	std::vector<std::string> headerNames;

	/* Whether the header of the open file has been written. Cleared when a file
	   is opened, so that the columns of a run are named once at its start, and
	   again inside it only when they change. */
	bool headerWritten = false;
};

#endif