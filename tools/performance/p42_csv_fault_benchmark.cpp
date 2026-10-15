#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "CSVStreamer.hpp"
#include "spdlog/sinks/null_sink.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Result
	{
		std::string scenario;
		double submitMs = 0.0;
		double finishMs = 0.0;
		CSVStreamer::Metrics metrics;
	};

	Result runSlowDisk(const std::filesystem::path& root, spdlog::logger* logger)
	{
		CSVStreamer::Options options;
		options.queueCapacity = 256;
		options.simulatedWriteDelay = std::chrono::milliseconds(50);
		CSVStreamer streamer(logger, "slow-disk", options);
		if (!streamer.prepareFile(root.string()))
			throw std::runtime_error("slow disk file open failed");

		const auto submitBegan = Clock::now();
		for (uint32_t index = 0; index < 128; ++index)
			if (!streamer.writeLine(index, {static_cast<double>(index)}))
				throw std::runtime_error("slow disk queue unexpectedly overloaded");
		const auto submittedAt = Clock::now();
		streamer.finishLogging();
		const auto finishedAt = Clock::now();
		return {"slow_disk_50ms", std::chrono::duration<double, std::milli>(submittedAt - submitBegan).count(),
			std::chrono::duration<double, std::milli>(finishedAt - submittedAt).count(), streamer.getMetrics()};
	}

	Result runOverload(const std::filesystem::path& root, spdlog::logger* logger)
	{
		CSVStreamer::Options options;
		options.queueCapacity = 2;
		options.simulatedWriteDelay = std::chrono::milliseconds(50);
		CSVStreamer streamer(logger, "overload", options);
		if (!streamer.prepareFile(root.string()))
			throw std::runtime_error("overload file open failed");

		const auto submitBegan = Clock::now();
		for (uint32_t index = 0; index < 1000; ++index)
			if (!streamer.writeLine(index, {static_cast<double>(index)}))
				break;
		const auto submittedAt = Clock::now();
		streamer.finishLogging();
		const auto finishedAt = Clock::now();
		return {"queue_overload", std::chrono::duration<double, std::milli>(submittedAt - submitBegan).count(),
			std::chrono::duration<double, std::milli>(finishedAt - submittedAt).count(), streamer.getMetrics()};
	}

	Result runWriteFailure(const std::filesystem::path& root, spdlog::logger* logger)
	{
		CSVStreamer::Options options;
		options.queueCapacity = 256;
		options.simulatedWriteDelay = std::chrono::milliseconds(20);
		options.failWriteAfterBatches = 0;
		CSVStreamer streamer(logger, "write-failure", options);
		if (!streamer.prepareFile(root.string()))
			throw std::runtime_error("write failure file open failed");

		const auto submitBegan = Clock::now();
		for (uint32_t index = 0; index < 100; ++index)
			if (!streamer.writeLine(index, {static_cast<double>(index)}))
				throw std::runtime_error("write failure input was rejected before injection");
		const auto submittedAt = Clock::now();
		streamer.finishLogging();
		const auto finishedAt = Clock::now();
		return {"write_failure", std::chrono::duration<double, std::milli>(submittedAt - submitBegan).count(),
			std::chrono::duration<double, std::milli>(finishedAt - submittedAt).count(), streamer.getMetrics()};
	}

	Result runOpenFailure(const std::filesystem::path& root, spdlog::logger* logger)
	{
		CSVStreamer::Options options;
		options.simulateOpenFailure = true;
		CSVStreamer streamer(logger, "open-failure", options);
		const auto began = Clock::now();
		streamer.prepareFile(root.string());
		const auto ended = Clock::now();
		return {"open_failure", std::chrono::duration<double, std::milli>(ended - began).count(), 0.0, streamer.getMetrics()};
	}

	void writeResult(std::ofstream& output, const Result& result, bool last)
	{
		output << "    {\"scenario\": \"" << result.scenario << "\""
			   << ", \"submit_ms\": " << result.submitMs
			   << ", \"finish_ms\": " << result.finishMs
			   << ", \"failure_reason\": \"" << CSVStreamer::failureReasonName(result.metrics.failureReason) << "\""
			   << ", \"enqueued\": " << result.metrics.enqueuedSamples
			   << ", \"written\": " << result.metrics.writtenSamples
			   << ", \"failed\": " << result.metrics.failedSamples
			   << ", \"rejected\": " << result.metrics.rejectedSamples
			   << ", \"io_errors\": " << result.metrics.ioErrors
			   << ", \"queue_capacity\": " << result.metrics.queueCapacity
			   << ", \"queue_high_water_mark\": " << result.metrics.queueHighWaterMark
			   << ", \"queue_depth_after_finish\": " << result.metrics.queueDepth
			   << ", \"failure_at_unix_ms\": " << result.metrics.failureAtUnixMs << "}" << (last ? "\n" : ",\n");
	}
}

int main(int argc, char** argv)
{
	std::string outputPath = "analysis/variable-view-p4.2-csv-faults.json";
	for (int index = 1; index + 1 < argc; ++index)
		if (std::string(argv[index]) == "--output")
			outputPath = argv[++index];

	const auto logger = std::make_shared<spdlog::logger>(
		"p42-csv-benchmark", std::make_shared<spdlog::sinks::null_sink_mt>());
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "mcuviewer-p42-csv-benchmark";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);

	const Result results[] = {runSlowDisk(root, logger.get()), runOverload(root, logger.get()),
		runWriteFailure(root, logger.get()), runOpenFailure(root, logger.get())};
	std::ofstream output(outputPath);
	output << std::fixed << std::setprecision(3) << "{\n  \"benchmark\": \"p4.2_csv_faults\",\n  \"results\": [\n";
	for (size_t index = 0; index < std::size(results); ++index)
		writeResult(output, results[index], index + 1 == std::size(results));
	output << "  ]\n}\n";
	std::filesystem::remove_all(root, error);
	std::cout << "Wrote " << outputPath << '\n';
	return 0;
}
