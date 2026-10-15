#include "CSVStreamer.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>

namespace
{
	uint64_t nanosecondsBetween(std::chrono::steady_clock::time_point begin,
		std::chrono::steady_clock::time_point end)
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
	}

	void keepMaximum(std::atomic<uint64_t>& target, uint64_t candidate)
	{
		uint64_t previous = target.load(std::memory_order_relaxed);
		while (previous < candidate && !target.compare_exchange_weak(previous, candidate, std::memory_order_relaxed))
		{
		}
	}

	uint64_t unixTimeMilliseconds()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());
	}
}

CSVStreamer::CSVStreamer(spdlog::logger* logger, const std::string& logPrefix)
	: CSVStreamer(logger, logPrefix, Options{})
{
}

CSVStreamer::CSVStreamer(spdlog::logger* logger, const std::string& logPrefix, Options options)
	: logger(logger), logPrefix(logPrefix), options(options)
{
	this->options.queueCapacity = std::max<size_t>(1, this->options.queueCapacity);
	if (this->options.mode == WriteMode::Asynchronous)
		writerThread = std::thread(&CSVStreamer::writerLoop, this);
}

CSVStreamer::~CSVStreamer()
{
	finishLogging();
	if (options.mode == WriteMode::Asynchronous)
	{
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			shutdown = true;
		}
		queueReady.notify_one();
		if (writerThread.joinable())
			writerThread.join();
	}
}

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
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (fileOpen && accepting && directory == logDirectory)
			return true;
	}

	finishLogging();
	resetMetrics();
	logDirectory = directory;
	std::string nextFilePath = makeRunFilePath(directory);
	std::error_code existsError;
	for (uint32_t attempt = 2; std::filesystem::exists(nextFilePath, existsError); ++attempt)
		nextFilePath = makeRunFilePath(directory, attempt);
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		filePath = nextFilePath;
	}

	csvFile.clear();
	if (!options.simulateOpenFailure)
		csvFile.open(nextFilePath, std::ios::out);
	if (!csvFile.is_open())
	{
		metricsIoErrors.fetch_add(1, std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			setFailureLocked(FailureReason::OpenFailed, "CSV file could not be opened: " + nextFilePath);
		}
		logger->error("Failed to open file: {}", nextFilePath);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		fileOpen = true;
		headerNames.clear();
		headerGeneration = 0;
		accepting = true;
		closeRequested = false;
		overloaded = false;
		overloadReported = false;
		writeFailed = false;
		writeBatchCount = 0;
	}
	return true;
}

void CSVStreamer::createHeader(const std::vector<std::string>& names)
{
	uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (!accepting || (headerGeneration != 0 && names == headerNames))
			return;
		headerNames = names;
		generation = ++headerGeneration;
	}

	if (options.mode == WriteMode::Synchronous)
	{
		writeSynchronously(formatHeader(names), false);
		return;
	}

	QueueItem item;
	item.kind = ItemKind::Header;
	item.generation = generation;
	item.headerNames = names;
	item.enqueuedAt = std::chrono::steady_clock::now();
	enqueue(std::move(item), false);
}

bool CSVStreamer::writeLine(double time, std::unordered_map<std::string, double>& valuesMap)
{
	std::vector<std::string> names;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (!accepting)
		{
			metricsRejectedSamples.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		names = headerNames;
	}
	std::vector<double> values;
	values.reserve(names.size());
	for (const auto& name : names)
		values.push_back(valuesMap[name]);
	return writeLine(time, values);
}

bool CSVStreamer::writeLine(double time, const std::vector<double>& values)
{
	if (options.mode == WriteMode::Synchronous)
	{
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			if (!accepting)
			{
				metricsRejectedSamples.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
		}
		metricsEnqueuedSamples.fetch_add(1, std::memory_order_relaxed);
		const auto formatBegan = std::chrono::steady_clock::now();
		const std::string line = formatSample(time, values);
		metricsFormatNs.fetch_add(nanosecondsBetween(formatBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
		writeSynchronously(line, true);
		return csvFile.good();
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (!accepting)
		{
			metricsRejectedSamples.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	QueueItem item;
	item.kind = ItemKind::Sample;
	item.time = time;
	item.values = values;
	item.enqueuedAt = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		item.generation = headerGeneration;
	}
	return enqueue(std::move(item), true);
}

bool CSVStreamer::enqueue(QueueItem&& item, bool sample)
{
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (!accepting)
		{
			if (sample)
				metricsRejectedSamples.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (queue.size() >= options.queueCapacity)
		{
			accepting = false;
			closeRequested = true;
			overloaded = true;
			setFailureLocked(FailureReason::QueueOverload, "CSV queue reached its capacity; logging stopped.");
			if (sample)
				metricsRejectedSamples.fetch_add(1, std::memory_order_relaxed);
			queueReady.notify_one();
			return false;
		}
		queue.push_back(std::move(item));
		metricsQueueHighWaterMark = std::max(metricsQueueHighWaterMark, queue.size());
		if (sample)
			metricsEnqueuedSamples.fetch_add(1, std::memory_order_relaxed);
	}
	queueReady.notify_one();
	return true;
}

std::string CSVStreamer::formatHeader(const std::vector<std::string>& names) const
{
	std::string line = "time,";
	for (const auto& name : names)
		line += name + ",";
	line.back() = '\n';
	return line;
}

std::string CSVStreamer::formatSample(double time, const std::vector<double>& values) const
{
	std::string line = std::to_string(time) + ",";
	for (double value : values)
		line += std::to_string(value) + ",";
	line.back() = '\n';
	return line;
}

void CSVStreamer::writeSynchronously(const std::string& text, bool sample)
{
	const auto writeBegan = std::chrono::steady_clock::now();
	if (options.simulatedWriteDelay.count() > 0)
		std::this_thread::sleep_for(options.simulatedWriteDelay);
	if (writeBatchCount++ >= options.failWriteAfterBatches)
		csvFile.setstate(std::ios::badbit);
	else
		csvFile.write(text.data(), static_cast<std::streamsize>(text.size()));
	metricsWriteNs.fetch_add(nanosecondsBetween(writeBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
	if (!csvFile)
	{
		metricsIoErrors.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(queueMutex);
		accepting = false;
		setFailureLocked(FailureReason::WriteFailed, "CSV file write failed: " + filePath);
		if (sample)
			metricsFailedSamples.fetch_add(1, std::memory_order_relaxed);
	}
	else if (sample)
		metricsWrittenSamples.fetch_add(1, std::memory_order_relaxed);
}

void CSVStreamer::writerLoop()
{
	constexpr size_t maximumBatchItems = 64;
	std::vector<QueueItem> batch;
	batch.reserve(maximumBatchItems);
	for (;;)
	{
		bool reportOverload = false;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueReady.wait(lock, [this] { return shutdown || !queue.empty() || closeRequested; });
			if (shutdown && queue.empty())
				break;
			while (!queue.empty() && batch.size() < maximumBatchItems)
			{
				batch.push_back(std::move(queue.front()));
				queue.pop_front();
			}
			writerBusy = !batch.empty();
			writerBatchItems = batch.size();
			if (overloaded && !overloadReported)
			{
				overloadReported = true;
				reportOverload = true;
			}
		}
		if (reportOverload)
			logger->error("CSV logging stopped: the bounded queue reached {} entries", options.queueCapacity);

		if (!batch.empty())
		{
			std::string text;
			uint64_t samples = 0;
			const auto formatBegan = std::chrono::steady_clock::now();
			for (const QueueItem& item : batch)
			{
				if (item.kind == ItemKind::Header)
					text += formatHeader(item.headerNames);
				else
				{
					text += formatSample(item.time, item.values);
					++samples;
				}
			}
			metricsFormatNs.fetch_add(nanosecondsBetween(formatBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
			if (writeFailed)
				metricsFailedSamples.fetch_add(samples, std::memory_order_relaxed);
			else
			{
				const auto writeBegan = std::chrono::steady_clock::now();
				if (options.simulatedWriteDelay.count() > 0)
					std::this_thread::sleep_for(options.simulatedWriteDelay);
				if (writeBatchCount++ >= options.failWriteAfterBatches)
					csvFile.setstate(std::ios::badbit);
				else
					csvFile.write(text.data(), static_cast<std::streamsize>(text.size()));
				metricsWriteNs.fetch_add(nanosecondsBetween(writeBegan, std::chrono::steady_clock::now()), std::memory_order_relaxed);
				if (!csvFile)
				{
					writeFailed = true;
					metricsIoErrors.fetch_add(1, std::memory_order_relaxed);
					metricsFailedSamples.fetch_add(samples, std::memory_order_relaxed);
					logger->error("CSV logging stopped: writing failed for {}", filePath);
					std::lock_guard<std::mutex> lock(queueMutex);
					accepting = false;
					closeRequested = true;
					setFailureLocked(FailureReason::WriteFailed, "CSV file write failed: " + filePath);
				}
				else
					metricsWrittenSamples.fetch_add(samples, std::memory_order_relaxed);
			}

			const auto writtenAt = std::chrono::steady_clock::now();
			for (const QueueItem& item : batch)
				keepMaximum(metricsMaxWriterLagNs, nanosecondsBetween(item.enqueuedAt, writtenAt));
			batch.clear();
		}

		{
			std::lock_guard<std::mutex> lock(queueMutex);
			writerBusy = false;
			writerBatchItems = 0;
			if (queue.empty() && closeRequested)
			{
				csvFile.close();
				if (!csvFile && !writeFailed)
				{
					writeFailed = true;
					metricsIoErrors.fetch_add(1, std::memory_order_relaxed);
					setFailureLocked(FailureReason::WriteFailed, "CSV file close failed: " + filePath);
					logger->error("CSV logging stopped: closing failed for {}", filePath);
				}
				fileOpen = false;
				closeRequested = false;
				queueDrained.notify_all();
			}
			else if (queue.empty())
				queueDrained.notify_all();
		}
	}
}

void CSVStreamer::finishLogging()
{
	if (options.mode == WriteMode::Synchronous)
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		accepting = false;
		if (fileOpen)
		{
			csvFile.close();
			if (!csvFile && failureReason != FailureReason::WriteFailed)
			{
				metricsIoErrors.fetch_add(1, std::memory_order_relaxed);
				setFailureLocked(FailureReason::WriteFailed, "CSV file close failed: " + filePath);
				logger->error("CSV logging stopped: closing failed for {}", filePath);
			}
			fileOpen = false;
		}
		return;
	}

	std::unique_lock<std::mutex> lock(queueMutex);
	accepting = false;
	if (!fileOpen && queue.empty() && !writerBusy)
		return;
	closeRequested = true;
	queueReady.notify_one();
	queueDrained.wait(lock, [this] { return !fileOpen && queue.empty() && !writerBusy; });
}

CSVStreamer::Metrics CSVStreamer::getMetrics() const
{
	Metrics result;
	result.enqueuedSamples = metricsEnqueuedSamples.load(std::memory_order_relaxed);
	result.writtenSamples = metricsWrittenSamples.load(std::memory_order_relaxed);
	result.rejectedSamples = metricsRejectedSamples.load(std::memory_order_relaxed);
	result.failedSamples = metricsFailedSamples.load(std::memory_order_relaxed);
	result.formatNs = metricsFormatNs.load(std::memory_order_relaxed);
	result.writeNs = metricsWriteNs.load(std::memory_order_relaxed);
	result.maxWriterLagNs = metricsMaxWriterLagNs.load(std::memory_order_relaxed);
	result.ioErrors = metricsIoErrors.load(std::memory_order_relaxed);
	result.mode = options.mode;
	result.queueCapacity = options.queueCapacity;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		result.queueDepth = queue.size() + writerBatchItems;
		result.queueHighWaterMark = metricsQueueHighWaterMark;
		result.active = accepting;
		result.overloaded = overloaded;
		result.failureReason = failureReason;
		result.failureAtUnixMs = failureAtUnixMs;
		result.errorMessage = failureMessage;
		result.filePath = filePath;
	}
	return result;
}

void CSVStreamer::resetMetrics()
{
	metricsEnqueuedSamples.store(0, std::memory_order_relaxed);
	metricsWrittenSamples.store(0, std::memory_order_relaxed);
	metricsRejectedSamples.store(0, std::memory_order_relaxed);
	metricsFailedSamples.store(0, std::memory_order_relaxed);
	metricsFormatNs.store(0, std::memory_order_relaxed);
	metricsWriteNs.store(0, std::memory_order_relaxed);
	metricsMaxWriterLagNs.store(0, std::memory_order_relaxed);
	metricsIoErrors.store(0, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(queueMutex);
	metricsQueueHighWaterMark = 0;
	failureReason = FailureReason::None;
	failureAtUnixMs = 0;
	failureMessage.clear();
	overloaded = false;
	overloadReported = false;
	writeFailed = false;
	writeBatchCount = 0;
}

void CSVStreamer::setFailureLocked(FailureReason reason, const std::string& message)
{
	failureReason = reason;
	failureAtUnixMs = unixTimeMilliseconds();
	failureMessage = message;
}

const char* CSVStreamer::failureReasonName(FailureReason reason)
{
	switch (reason)
	{
		case FailureReason::QueueOverload:
			return "queue_overload";
		case FailureReason::OpenFailed:
			return "open_failed";
		case FailureReason::WriteFailed:
			return "write_failed";
		default:
			return "none";
	}
}
