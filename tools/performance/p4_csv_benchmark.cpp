#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "CSVStreamer.hpp"
#include "spdlog/sinks/null_sink.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Result
	{
		uint32_t variables = 0;
		std::string mode;
		uint64_t samples = 0;
		double submitP50Us = 0.0;
		double submitP95Us = 0.0;
		double submitP99Us = 0.0;
		double submitTotalMs = 0.0;
		double endToEndMs = 0.0;
		CSVStreamer::Metrics metrics;
	};

	double percentileUs(std::vector<uint64_t> values, double percentile)
	{
		std::sort(values.begin(), values.end());
		const size_t index = static_cast<size_t>(percentile * static_cast<double>(values.size() - 1));
		return static_cast<double>(values[index]) / 1000.0;
	}

	Result runScenario(const std::filesystem::path& root, spdlog::logger* logger,
		uint32_t variables, CSVStreamer::WriteMode mode, uint64_t sampleCount)
	{
		const std::string modeName = mode == CSVStreamer::WriteMode::Asynchronous ? "async" : "sync_fallback";
		const std::filesystem::path directory = root / (modeName + "-" + std::to_string(variables));
		std::filesystem::create_directories(directory);

		CSVStreamer::Options options;
		options.mode = mode;
		options.queueCapacity = static_cast<size_t>(sampleCount + 1);
		CSVStreamer streamer(logger, "p4", options);
		if (!streamer.prepareFile(directory.string()))
			throw std::runtime_error("could not create benchmark CSV file");

		std::vector<std::string> headers;
		std::vector<double> values(variables);
		for (uint32_t index = 0; index < variables; ++index)
			headers.push_back("v" + std::to_string(index));
		streamer.createHeader(headers);

		std::vector<uint64_t> submitDurations;
		submitDurations.reserve(static_cast<size_t>(sampleCount));
		const auto totalBegan = Clock::now();
		for (uint64_t sample = 0; sample < sampleCount; ++sample)
		{
			for (uint32_t index = 0; index < variables; ++index)
				values[index] = static_cast<double>(sample + index) * 0.125;
			const auto submitBegan = Clock::now();
			if (!streamer.writeLine(static_cast<double>(sample) * 0.001, values))
				throw std::runtime_error("benchmark queue unexpectedly overloaded");
			submitDurations.push_back(static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - submitBegan).count()));
		}
		const auto submittedAt = Clock::now();
		streamer.finishLogging();
		const auto finishedAt = Clock::now();

		Result result;
		result.variables = variables;
		result.mode = modeName;
		result.samples = sampleCount;
		result.submitP50Us = percentileUs(submitDurations, 0.50);
		result.submitP95Us = percentileUs(submitDurations, 0.95);
		result.submitP99Us = percentileUs(submitDurations, 0.99);
		result.submitTotalMs = std::chrono::duration<double, std::milli>(submittedAt - totalBegan).count();
		result.endToEndMs = std::chrono::duration<double, std::milli>(finishedAt - totalBegan).count();
		result.metrics = streamer.getMetrics();
		return result;
	}

	void writeResults(const std::string& outputPath, const std::vector<Result>& results)
	{
		std::ofstream output(outputPath);
		output << std::fixed << std::setprecision(3)
			   << "{\n  \"benchmark\": \"p4_csv_binary_queue\",\n  \"results\": [\n";
		for (size_t index = 0; index < results.size(); ++index)
		{
			const Result& result = results[index];
			output << "    {\"variables\": " << result.variables
				   << ", \"mode\": \"" << result.mode << "\""
				   << ", \"samples\": " << result.samples
				   << ", \"submit_p50_us\": " << result.submitP50Us
				   << ", \"submit_p95_us\": " << result.submitP95Us
				   << ", \"submit_p99_us\": " << result.submitP99Us
				   << ", \"submit_total_ms\": " << result.submitTotalMs
				   << ", \"end_to_end_ms\": " << result.endToEndMs
				   << ", \"background_format_ms\": " << static_cast<double>(result.metrics.formatNs) / 1000000.0
				   << ", \"file_write_ms\": " << static_cast<double>(result.metrics.writeNs) / 1000000.0
				   << ", \"max_writer_lag_ms\": " << static_cast<double>(result.metrics.maxWriterLagNs) / 1000000.0
				   << ", \"queue_high_water_mark\": " << result.metrics.queueHighWaterMark
				   << ", \"enqueued\": " << result.metrics.enqueuedSamples
				   << ", \"written\": " << result.metrics.writtenSamples
				   << ", \"rejected\": " << result.metrics.rejectedSamples << "}";
			if (index + 1 != results.size())
				output << ',';
			output << '\n';
		}
		output << "  ]\n}\n";
	}
}

int main(int argc, char** argv)
{
	std::string outputPath = "analysis/variable-view-p4-csv-cpu.json";
	for (int index = 1; index + 1 < argc; ++index)
		if (std::string(argv[index]) == "--output")
			outputPath = argv[++index];

	const auto logger = std::make_shared<spdlog::logger>(
		"p4-csv-benchmark", std::make_shared<spdlog::sinks::null_sink_mt>());
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "mcuviewer-p4-csv-benchmark";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);

	constexpr uint64_t sampleCount = 20000;
	std::vector<Result> results;
	for (uint32_t variables : {1u, 8u, 20u})
	{
		results.push_back(runScenario(root, logger.get(), variables, CSVStreamer::WriteMode::Synchronous, sampleCount));
		results.push_back(runScenario(root, logger.get(), variables, CSVStreamer::WriteMode::Asynchronous, sampleCount));
	}

	writeResults(outputPath, results);
	std::filesystem::remove_all(root, error);
	std::cout << "Wrote " << outputPath << '\n';
	return 0;
}
