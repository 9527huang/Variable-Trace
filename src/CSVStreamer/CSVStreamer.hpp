#ifndef _CSV_STREAMER_HPP
#define _CSV_STREAMER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "spdlog/spdlog.h"

class CSVStreamer
{
   public:
	enum class WriteMode
	{
		Asynchronous,
		Synchronous
	};

	enum class FailureReason
	{
		None,
		QueueOverload,
		OpenFailed,
		WriteFailed
	};

	struct Options
	{
		WriteMode mode = WriteMode::Asynchronous;
		size_t queueCapacity = 1024;
		std::chrono::microseconds simulatedWriteDelay{};
		bool simulateOpenFailure = false;
		size_t failWriteAfterBatches = std::numeric_limits<size_t>::max();
	};

	struct Metrics
	{
		uint64_t enqueuedSamples = 0;
		uint64_t writtenSamples = 0;
		uint64_t rejectedSamples = 0;
		uint64_t failedSamples = 0;
		uint64_t formatNs = 0;
		uint64_t writeNs = 0;
		uint64_t maxWriterLagNs = 0;
		uint64_t ioErrors = 0;
		size_t queueDepth = 0;
		size_t queueHighWaterMark = 0;
		size_t queueCapacity = 0;
		uint64_t failureAtUnixMs = 0;
		FailureReason failureReason = FailureReason::None;
		std::string errorMessage;
		std::string filePath;
		bool active = false;
		bool overloaded = false;
		WriteMode mode = WriteMode::Asynchronous;
	};

	CSVStreamer(spdlog::logger* logger, const std::string& logPrefix = "logfile");
	CSVStreamer(spdlog::logger* logger, const std::string& logPrefix, Options options);
	~CSVStreamer();

	CSVStreamer(const CSVStreamer&) = delete;
	CSVStreamer& operator=(const CSVStreamer&) = delete;

	bool prepareFile(const std::string& directory);
	void createHeader(const std::vector<std::string>& headerNames);
	bool writeLine(double time, std::unordered_map<std::string, double>& valuesMap);
	bool writeLine(double time, const std::vector<double>& values);
	void finishLogging();
	Metrics getMetrics() const;
	static const char* failureReasonName(FailureReason reason);

   private:
	enum class ItemKind
	{
		Header,
		Sample
	};

	struct QueueItem
	{
		ItemKind kind = ItemKind::Sample;
		uint64_t generation = 0;
		double time = 0.0;
		std::vector<double> values;
		std::vector<std::string> headerNames;
		std::chrono::steady_clock::time_point enqueuedAt;
	};

	std::string makeRunFilePath(const std::string& directory, uint32_t attempt = 0) const;
	bool enqueue(QueueItem&& item, bool sample);
	void writerLoop();
	std::string formatHeader(const std::vector<std::string>& names) const;
	std::string formatSample(double time, const std::vector<double>& values) const;
	void writeSynchronously(const std::string& text, bool sample);
	void resetMetrics();
	void setFailureLocked(FailureReason reason, const std::string& message);

	spdlog::logger* logger;
	std::string logPrefix;
	std::string filePath;
	std::string logDirectory;
	Options options;
	std::ofstream csvFile;

	mutable std::mutex queueMutex;
	std::condition_variable queueReady;
	std::condition_variable queueDrained;
	std::deque<QueueItem> queue;
	std::thread writerThread;
	std::vector<std::string> headerNames;
	uint64_t headerGeneration = 0;
	bool accepting = false;
	bool fileOpen = false;
	bool closeRequested = false;
	bool writerBusy = false;
	size_t writerBatchItems = 0;
	bool shutdown = false;
	bool overloaded = false;
	bool overloadReported = false;
	bool writeFailed = false;
	size_t writeBatchCount = 0;
	FailureReason failureReason = FailureReason::None;
	uint64_t failureAtUnixMs = 0;
	std::string failureMessage;

	std::atomic<uint64_t> metricsEnqueuedSamples{0};
	std::atomic<uint64_t> metricsWrittenSamples{0};
	std::atomic<uint64_t> metricsRejectedSamples{0};
	std::atomic<uint64_t> metricsFailedSamples{0};
	std::atomic<uint64_t> metricsFormatNs{0};
	std::atomic<uint64_t> metricsWriteNs{0};
	std::atomic<uint64_t> metricsMaxWriterLagNs{0};
	std::atomic<uint64_t> metricsIoErrors{0};
	size_t metricsQueueHighWaterMark = 0;
};

#endif
