#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "McpTools.hpp"
#include "PerformanceDebugProbe.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

namespace
{
	uint32_t expectedProbeValue(uint32_t address, uint32_t size, uint64_t sequence)
	{
		uint32_t value = 0;
		for (uint32_t index = 0; index < size; index++)
			value |= static_cast<uint32_t>((address + index + sequence) & 0xff) << (index * 8);
		return value;
	}

	class ViewerDataHandlerPerformanceTest : public ::testing::Test
	{
	   protected:
		void SetUp() override
		{
			logger = std::make_shared<spdlog::logger>("viewer-performance-test", std::make_shared<spdlog::sinks::null_sink_mt>());
		}

		void TearDown() override
		{
			applicationDone = true;
			viewer.reset();
		}

		void startLayout(const std::vector<std::pair<uint32_t, Variable::Type>>& layout,
					 uint32_t frequencyHz,
					 std::chrono::microseconds perCall = {},
					 bool mergeContiguousReads = true,
					 uint32_t maxBlockSize = std::numeric_limits<uint32_t>::max(),
					 IDebugProbe::Probe probeType = IDebugProbe::Probe::Jlink,
					 bool shouldLog = false,
					 const std::string& logDirectory = {})
		{
			plotGroupHandler.addGroup("baseline");
			plotGroupHandler.setActiveGroup("baseline");
			plot = plotHandler.addPlot("baseline");

			for (uint32_t index = 0; index < layout.size(); index++)
			{
				const std::string name = "value" + std::to_string(index);
				variableHandler.addNewVariable(name);
				auto variable = variableHandler.getVariable(name);
				variable->setType(layout[index].second);
				variable->setAddress(layout[index].first);
				plot->addSeries(variable.get());
			}

			plotGroupHandler.getActiveGroup()->addPlot(plot);
			viewer = std::make_unique<ViewerDataHandler>(&plotGroupHandler, &variableHandler, &plotHandler, &tracePlotHandler, applicationDone, &bufferMutex, logger.get());

			auto settings = viewer->getSettings();
			settings.sampleFrequencyHz = frequencyHz;
			settings.mergeContiguousReads = mergeContiguousReads;
			settings.shouldLog = shouldLog;
			settings.logFilePath = logDirectory;
			viewer->setSettings(settings);
			auto probeSettings = viewer->getProbeSettings();
			probeSettings.debugProbe = probeType;
			viewer->setProbeSettings(probeSettings);

			probe = std::make_shared<PerformanceDebugProbe>();
			probe->timing.fixedPerCall = perCall;
			probe->maxBlockSize = maxBlockSize;
			viewer->setDebugProbe(probe);
			viewer->setState(DataHandlerBase::State::RUN);
		}

		void start(uint32_t variables,
				   uint32_t frequencyHz,
				   std::chrono::microseconds perCall = {},
				   uint32_t baseAddress = 0x20001000,
				   uint32_t stride = sizeof(uint32_t),
				   bool mergeContiguousReads = true,
				   uint32_t maxBlockSize = std::numeric_limits<uint32_t>::max())
		{
			std::vector<std::pair<uint32_t, Variable::Type>> layout;
			for (uint32_t index = 0; index < variables; index++)
				layout.push_back({baseAddress + index * stride, Variable::Type::U32});

			startLayout(layout, frequencyHz, perCall, mergeContiguousReads, maxBlockSize);
		}

		bool waitUntil(const std::function<bool()>& predicate)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

			while (std::chrono::steady_clock::now() < deadline)
			{
				if (predicate())
					return true;

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			return false;
		}

		void stopAndWait()
		{
			viewer->setState(DataHandlerBase::State::STOP);
			ASSERT_TRUE(waitUntil([this] { return probe->detachments.load() > 0; }));
		}

		void verifySchedulerBaseline(uint32_t variables)
		{
			start(variables, 100);
			ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 201; }));
			const auto metrics = viewer->getPerformanceMetrics();
			stopAndWait();
			const double elapsedSeconds = static_cast<double>(metrics.elapsedNs) / 1'000'000'000.0;
			const double actualHz = static_cast<double>(metrics.completedSamples) / elapsedSeconds;
			const double sleepRatio = static_cast<double>(metrics.schedulerSleepNs) / metrics.elapsedNs;

			RecordProperty("variables", variables);
			RecordProperty("completed_samples", metrics.completedSamples);
			RecordProperty("actual_hz", std::to_string(actualHz));
			RecordProperty("scheduler_sleep_ratio", std::to_string(sleepRatio));
			RecordProperty("skipped_sample_periods", metrics.skippedSamplePeriods);
			const double completed = static_cast<double>(metrics.completedSamples);
			RecordProperty("average_model_update_us", std::to_string(static_cast<double>(metrics.modelUpdateNs) / completed / 1000.0));
			RecordProperty("average_model_lock_wait_us", std::to_string(static_cast<double>(metrics.modelLockWaitNs) / completed / 1000.0));
			RecordProperty("average_shared_buffer_lock_wait_us", std::to_string(static_cast<double>(metrics.sharedBufferLockWaitNs) / completed / 1000.0));
			RecordProperty("average_probe_read_us", std::to_string(static_cast<double>(metrics.probeReadNs) / completed / 1000.0));
			const PerformanceDiagnosis diagnosis = viewer->getPerformanceDiagnosis();
			RecordProperty("diagnosis", performanceBottleneckCode(diagnosis.bottleneck));
			RecordProperty("request_attainment_ratio", std::to_string(diagnosis.requestAttainmentRatio));
			RecordProperty("deadline_miss_ratio", std::to_string(diagnosis.deadlineMissRatio));
			RecordProperty("probe_read_ratio", std::to_string(diagnosis.probeReadRatio));

			EXPECT_GE(actualHz, 95.0);
			EXPECT_LE(actualHz, 105.0);
			EXPECT_GT(sleepRatio, 0.70);
			EXPECT_EQ(metrics.deadlineMisses, metrics.skippedSamplePeriods);
			EXPECT_EQ(diagnosis.bottleneck, PerformanceBottleneck::None);
		}

		std::shared_ptr<spdlog::logger> logger;
		std::atomic<bool> applicationDone{false};
		std::mutex bufferMutex;
		PlotGroupHandler plotGroupHandler;
		VariableHandler variableHandler;
		PlotHandler plotHandler;
		PlotHandler tracePlotHandler;
		std::shared_ptr<Plot> plot;
		std::shared_ptr<PerformanceDebugProbe> probe;
		std::unique_ptr<ViewerDataHandler> viewer;
	};

	TEST_F(ViewerDataHandlerPerformanceTest, ContiguousRamUsesOneBlockTransactionPerSample)
	{
		constexpr uint32_t variables = 8;
		start(variables, 1000, std::chrono::microseconds(50));
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();

		const auto metrics = viewer->getPerformanceMetrics();
		ASSERT_GT(metrics.completedSamples, 0u);
		EXPECT_EQ(metrics.sampledVariables, variables);
		EXPECT_EQ(metrics.requestedSamples, metrics.completedSamples);
		EXPECT_EQ(metrics.failedSamples, 0u);
		EXPECT_TRUE(metrics.mergedReadsEnabled);
		EXPECT_EQ(metrics.plannedProbeCallsPerSample, 1u);
		EXPECT_EQ(metrics.mergedReadSpans, 1u);
		EXPECT_EQ(metrics.probeCalls, metrics.completedSamples);
		EXPECT_EQ(metrics.successfulProbeCalls, metrics.probeCalls);
		EXPECT_EQ(metrics.bytesRead, metrics.completedSamples * variables * sizeof(uint32_t));
		EXPECT_EQ(probe->memoryCalls.load(), 0u);
		EXPECT_EQ(probe->blockCalls.load(), metrics.probeCalls);
		EXPECT_EQ(probe->bytesRead.load(), metrics.bytesRead);
		const uint64_t lastSequence = metrics.completedSamples - 1;
		EXPECT_EQ(static_cast<uint32_t>(variableHandler.getVariable("value0")->transformToDouble()), expectedProbeValue(0x20001000, 4, lastSequence));
		EXPECT_EQ(static_cast<uint32_t>(variableHandler.getVariable("value7")->transformToDouble()), expectedProbeValue(0x2000101c, 4, lastSequence));
		EXPECT_GT(metrics.probeReadNs, 0u);
		EXPECT_GT(metrics.modelUpdateNs, 0u);
		EXPECT_GT(metrics.elapsedNs, 0u);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, OriginalPerVariablePathRemainsAvailable)
	{
		constexpr uint32_t variables = 8;
		start(variables, 1000, {}, 0x20001000, sizeof(uint32_t), false);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();

		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_FALSE(metrics.mergedReadsEnabled);
		EXPECT_EQ(metrics.plannedProbeCallsPerSample, variables);
		EXPECT_EQ(metrics.mergedReadSpans, 0u);
		EXPECT_EQ(metrics.probeCalls, metrics.completedSamples * variables);
		EXPECT_EQ(probe->memoryCalls.load(), metrics.probeCalls);
		EXPECT_EQ(probe->blockCalls.load(), 0u);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, SparseRamAndPeripheralAddressesKeepExactReads)
	{
		startLayout({{0x20001000, Variable::Type::U32},
					 {0x20001008, Variable::Type::U32},
					 {0x40000000, Variable::Type::U32},
					 {0x40000004, Variable::Type::U32}},
					1000);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();

		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_EQ(metrics.plannedProbeCallsPerSample, 4u);
		EXPECT_EQ(metrics.mergedReadSpans, 0u);
		EXPECT_EQ(metrics.probeCalls, metrics.completedSamples * 4);
		EXPECT_EQ(probe->memoryCalls.load(), metrics.probeCalls);
		EXPECT_EQ(probe->blockCalls.load(), 0u);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, OverlappingRamValuesShareOnePhysicalRead)
	{
		startLayout({{0x20001000, Variable::Type::U32}, {0x20001002, Variable::Type::U16}}, 1000);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();

		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_EQ(metrics.probeCalls, metrics.completedSamples);
		EXPECT_EQ(metrics.bytesRead, metrics.completedSamples * sizeof(uint32_t));
		EXPECT_EQ(probe->blockCalls.load(), metrics.completedSamples);
		const uint64_t lastSequence = metrics.completedSamples - 1;
		EXPECT_EQ(static_cast<uint32_t>(variableHandler.getVariable("value0")->transformToDouble()), expectedProbeValue(0x20001000, 4, lastSequence));
		EXPECT_EQ(static_cast<uint32_t>(variableHandler.getVariable("value1")->transformToDouble()), expectedProbeValue(0x20001002, 2, lastSequence));
	}

	TEST_F(ViewerDataHandlerPerformanceTest, FailedMergedReadInvalidatesTheSampleAndStops)
	{
		start(8, 1000, {}, 0x20001000, sizeof(uint32_t), true, 4);
		ASSERT_TRUE(waitUntil([this] { return probe->detachments.load() > 0; }));

		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_EQ(metrics.requestedSamples, 1u);
		EXPECT_EQ(metrics.completedSamples, 0u);
		EXPECT_EQ(metrics.failedSamples, 1u);
		EXPECT_EQ(metrics.probeCalls, 1u);
		EXPECT_EQ(metrics.successfulProbeCalls, 0u);
		EXPECT_EQ(metrics.bytesRead, 0u);
		EXPECT_EQ(probe->blockCalls.load(), 1u);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, MixedWidthsAndUnalignedOffsetsDecodeWithoutChangingTypes)
	{
		startLayout({{0x20001001, Variable::Type::U8},
					 {0x20001002, Variable::Type::I16},
					 {0x20001004, Variable::Type::F32},
					 {0x20001008, Variable::Type::U32}}, 1000);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();
		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_EQ(metrics.probeCalls, metrics.completedSamples);
		EXPECT_EQ(metrics.bytesRead, metrics.completedSamples * 11);
		const uint64_t sequence = metrics.completedSamples - 1;
		EXPECT_EQ(variableHandler.getVariable("value0")->transformToDouble(), expectedProbeValue(0x20001001, 1, sequence));
		EXPECT_EQ(variableHandler.getVariable("value1")->transformToDouble(), static_cast<int16_t>(expectedProbeValue(0x20001002, 2, sequence)));
		EXPECT_EQ(variableHandler.getVariable("value2")->transformToDouble(), std::bit_cast<float>(expectedProbeValue(0x20001004, 4, sequence)));
		EXPECT_EQ(variableHandler.getVariable("value3")->transformToDouble(), expectedProbeValue(0x20001008, 4, sequence));
	}

	TEST_F(ViewerDataHandlerPerformanceTest, SerialKeepsItsOriginalSweepPath)
	{
		start(8, 1000);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();
		const uint64_t blocks = probe->blockCalls.load();
		const uint64_t memory = probe->memoryCalls.load();
		auto probeSettings = viewer->getProbeSettings();
		probeSettings.debugProbe = IDebugProbe::Probe::Serial;
		viewer->setProbeSettings(probeSettings);
		const uint64_t previousRun = viewer->getPerformanceMetrics().runId;
		viewer->setState(DataHandlerBase::State::RUN);
		ASSERT_TRUE(waitUntil([this, previousRun] { const auto m = viewer->getPerformanceMetrics(); return m.runId > previousRun && m.completedSamples >= 20; }));
		const uint64_t previousDetachments = probe->detachments.load();
		viewer->setState(DataHandlerBase::State::STOP);
		ASSERT_TRUE(waitUntil([this, previousDetachments] { return probe->detachments.load() > previousDetachments; }));
		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_EQ(metrics.plannedProbeCallsPerSample, 8u);
		EXPECT_EQ(metrics.mergedReadSpans, 0u);
		EXPECT_EQ(probe->blockCalls.load(), blocks);
		EXPECT_EQ(probe->memoryCalls.load() - memory, metrics.completedSamples * 8);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, UnalignedStlinkSpansKeepOriginalReads)
	{
		startLayout({{0x20001001, Variable::Type::U8}, {0x20001002, Variable::Type::U16}},
					1000, {}, true, std::numeric_limits<uint32_t>::max(), IDebugProbe::Probe::Stlink);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();
		const auto metrics = viewer->getPerformanceMetrics();
		EXPECT_EQ(metrics.plannedProbeCallsPerSample, 2u);
		EXPECT_EQ(metrics.mergedReadSpans, 0u);
		EXPECT_EQ(probe->blockCalls.load(), 0u);
		EXPECT_EQ(probe->memoryCalls.load(), metrics.completedSamples * 2);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, ANewRunResetsCountersAndAdvancesTheRunId)
	{
		start(2, 500);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 10; }));
		stopAndWait();

		const auto first = viewer->getPerformanceMetrics();
		viewer->setState(DataHandlerBase::State::RUN);
		ASSERT_TRUE(waitUntil([this, first]
							  { const auto current = viewer->getPerformanceMetrics(); return current.runId > first.runId && current.completedSamples >= 3; }));
		stopAndWait();

		const auto second = viewer->getPerformanceMetrics();
		EXPECT_EQ(second.runId, first.runId + 1);
		EXPECT_LT(second.completedSamples, first.completedSamples);
		/* Stop may arrive after one probe read and before that sample is committed.
		   The new run must not inherit old calls, but one interrupted in-flight
		   call belongs to this run and is valid. */
		EXPECT_GE(second.probeCalls, second.completedSamples);
		EXPECT_LE(second.probeCalls, second.completedSamples + 1);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, AcquisitionStateCarriesTheMetricsWithoutAddingATool)
	{
		start(1, 500);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 3; }));

		mcp::ToolRegistry registry;
		mcp::AcquisitionTools::registerAll(registry);
		const mcp::Tool* tool = registry.find("get_acquisition_state");
		ASSERT_NE(tool, nullptr);

		McpContext context;
		context.viewerDataHandler = viewer.get();
		const mcp::Json result = tool->handler(context, mcp::Json::object());

		EXPECT_EQ(result.at("state"), "RUN");
		ASSERT_TRUE(result.contains("metrics"));
		EXPECT_EQ(result.at("metrics").at("sampled_variables"), 1);
		EXPECT_TRUE(result.at("metrics").at("merged_reads_enabled").get<bool>());
		EXPECT_EQ(result.at("metrics").at("planned_probe_calls_per_sample"), 1);
		EXPECT_GE(result.at("metrics").at("completed_samples").get<uint64_t>(), 3u);
		EXPECT_GT(result.at("metrics").at("actual_sampling_hz").get<double>(), 0.0);
		EXPECT_TRUE(result.at("metrics").contains("scheduler_sleep_ns"));
		EXPECT_TRUE(result.at("metrics").contains("skipped_sample_periods"));
		EXPECT_TRUE(result.at("metrics").contains("csv_failed_samples"));
		EXPECT_TRUE(result.at("metrics").contains("csv_queue_capacity"));
		EXPECT_TRUE(result.at("metrics").at("csv_failure_reason").is_string());
		EXPECT_TRUE(result.at("metrics").at("csv_failure_at_unix_ms").is_number_unsigned());
		EXPECT_TRUE(result.at("metrics").at("csv_error").is_string());
		EXPECT_TRUE(result.at("metrics").at("csv_file_path").is_string());
		ASSERT_TRUE(result.at("metrics").contains("diagnosis"));
		const auto& diagnosis = result.at("metrics").at("diagnosis");
		EXPECT_EQ(diagnosis.at("code"), "warming_up");
		EXPECT_FALSE(diagnosis.at("active").get<bool>());
		EXPECT_EQ(diagnosis.at("requested_sampling_hz"), 500);
		EXPECT_TRUE(diagnosis.at("request_attainment_ratio").is_number());

		context.getGuiPerformanceMetrics = []
		{
			return mcp::Json::object({{"measured_frames", 30}, {"frame_build_p95_us", 40000.0}});
		};
		ASSERT_TRUE(waitUntil([this]
						  { const auto current = viewer->getPerformanceMetrics(); return current.completedSamples >= 500 && current.elapsedNs >= 1'000'000'000; }));
		const mcp::Json renderLimited = tool->handler(context, mcp::Json::object());
		EXPECT_EQ(renderLimited.at("metrics").at("diagnosis").at("code"), "render_contention");
		EXPECT_TRUE(renderLimited.at("metrics").at("diagnosis").at("active").get<bool>());
	}

	TEST_F(ViewerDataHandlerPerformanceTest, AbsoluteDeadlineBaselineWithOneVariable)
	{
		verifySchedulerBaseline(1);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, AbsoluteDeadlineBaselineWithEightVariables)
	{
		verifySchedulerBaseline(8);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, AbsoluteDeadlineBaselineWithTwentyVariables)
	{
		verifySchedulerBaseline(20);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, SlowSamplesSkipExpiredPeriodsWithoutCatchUpBursts)
	{
		start(8, 1000, std::chrono::milliseconds(5));
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 20; }));
		stopAndWait();

		const auto metrics = viewer->getPerformanceMetrics();
		const double elapsedSeconds = static_cast<double>(metrics.elapsedNs) / 1'000'000'000.0;
		RecordProperty("completed_samples", metrics.completedSamples);
		RecordProperty("skipped_sample_periods", metrics.skippedSamplePeriods);
		RecordProperty("actual_hz", std::to_string(static_cast<double>(metrics.completedSamples) / elapsedSeconds));
		EXPECT_GT(metrics.skippedSamplePeriods, metrics.completedSamples * 2);
		EXPECT_EQ(metrics.deadlineMisses, metrics.skippedSamplePeriods);
		EXPECT_EQ(metrics.probeCalls, metrics.completedSamples);
		EXPECT_EQ(metrics.requestedSamples, metrics.completedSamples);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, TenThousandImportedVariablesDoNotEnterTheEightVariableHotPath)
	{
		constexpr uint32_t importedVariables = 10000;
		constexpr uint32_t sampledVariables = 8;
		for (uint32_t index = 0; index < importedVariables - sampledVariables; ++index)
			variableHandler.addVariable(std::make_shared<Variable>("unused" + std::to_string(index)));

		start(sampledVariables, 1000);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 200; }));
		stopAndWait();
		const auto metrics = viewer->getPerformanceMetrics();

		const double averageModelUpdateUs = static_cast<double>(metrics.modelUpdateNs) / metrics.completedSamples / 1000.0;
		RecordProperty("imported_variables", importedVariables);
		RecordProperty("sampled_variables", sampledVariables);
		RecordProperty("average_model_update_us", std::to_string(averageModelUpdateUs));

		EXPECT_EQ(metrics.sampledVariables, sampledVariables);
		EXPECT_LT(averageModelUpdateUs, 100.0);
		EXPECT_EQ(metrics.failedSamples, 0u);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, CsvFormattingRunsOutsideTheModelCommitBaseline)
	{
		const auto logDirectory = std::filesystem::temp_directory_path() /
			("mcuviewer-p23-csv-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ASSERT_TRUE(std::filesystem::create_directories(logDirectory));

		std::vector<std::pair<uint32_t, Variable::Type>> layout;
		for (uint32_t index = 0; index < 20; ++index)
			layout.push_back({0x20001000 + index * sizeof(uint32_t), Variable::Type::U32});
		startLayout(layout, 1000, {}, true, std::numeric_limits<uint32_t>::max(), IDebugProbe::Probe::Jlink, true, logDirectory.string());
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 200; }));
		stopAndWait();

		/* The writer is a thread of its own: `stopAndWait` returns when the probe
		   is detached, which is not the same moment as when the last row has
		   reached the file. Reading the counters between those two moments is a
		   race, and it was the race this test was losing - twice in three runs on
		   a busy machine. Wait for the drain, then read. */
		ASSERT_TRUE(waitUntil([this]
		                      {
			                      const auto now = viewer->getPerformanceMetrics();
			                      return now.csvEnqueuedSamples == 0 || now.csvWrittenSamples >= now.csvEnqueuedSamples;
		                      }));

		const auto metrics = viewer->getPerformanceMetrics();

		const double completed = static_cast<double>(metrics.completedSamples);
		RecordProperty("average_model_update_us", std::to_string(static_cast<double>(metrics.modelUpdateNs) / completed / 1000.0));
		RecordProperty("average_csv_submit_us", std::to_string(static_cast<double>(metrics.csvSubmitNs) / completed / 1000.0));
		RecordProperty("average_csv_background_format_us", std::to_string(static_cast<double>(metrics.csvFormatNs) / completed / 1000.0));
		RecordProperty("csv_rejected_samples", metrics.csvRejectedSamples);

		EXPECT_GT(metrics.csvSubmitNs, 0u);
		EXPECT_GT(metrics.csvFormatNs, 0u);

		/* Every completed sample either entered the queue or was refused on the
		   spot. Refusing is designed behaviour: the queue is bounded and the
		   sampling loop must never block on a disk, so a machine under load is
		   allowed to reject. What is not allowed is a sample disappearing without
		   a counter, which is what this identity catches.
		   Asserting "no rejections at 1 kHz" was asserting more than the design
		   promises, on a machine whose disk it does not control. */
		EXPECT_EQ(metrics.csvEnqueuedSamples + metrics.csvRejectedSamples, metrics.completedSamples);

		/* And everything that was accepted reached the file. */
		EXPECT_EQ(metrics.csvWrittenSamples, metrics.csvEnqueuedSamples);
		EXPECT_EQ(metrics.failedSamples, 0u);

		std::error_code cleanupError;
		std::filesystem::remove_all(logDirectory, cleanupError);
		EXPECT_FALSE(cleanupError);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, LowFrequencySleepKeepsStopResponsive)
	{
		start(1, 1);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 1; }));

		const auto stopBegan = std::chrono::steady_clock::now();
		stopAndWait();
		const auto stopDuration = std::chrono::steady_clock::now() - stopBegan;
		RecordProperty("stop_response_ms", std::chrono::duration_cast<std::chrono::microseconds>(stopDuration).count() / 1000);

		EXPECT_LT(stopDuration, std::chrono::milliseconds(250));
		EXPECT_GT(viewer->getPerformanceMetrics().schedulerSleepNs, 0u);
	}

	TEST_F(ViewerDataHandlerPerformanceTest, FrequencyChangeReanchorsTheAbsoluteSchedule)
	{
		start(1, 10);
		ASSERT_TRUE(waitUntil([this] { return viewer->getPerformanceMetrics().completedSamples >= 3; }));
		const uint64_t before = viewer->getPerformanceMetrics().completedSamples;

		auto settings = viewer->getSettings();
		settings.sampleFrequencyHz = 100;
		viewer->setSettings(settings);
		viewer->requestRestart();
		const auto changedAt = std::chrono::steady_clock::now();
		ASSERT_TRUE(waitUntil([this, before] { return viewer->getPerformanceMetrics().completedSamples >= before + 15; }));
		const auto changeDuration = std::chrono::steady_clock::now() - changedAt;
		stopAndWait();

		EXPECT_LT(changeDuration, std::chrono::milliseconds(400));
	}
}
