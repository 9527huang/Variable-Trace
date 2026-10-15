#include <gtest/gtest.h>

#include "PerformanceDiagnosis.hpp"

namespace
{
	PerformanceDiagnosisInput healthyInput()
	{
		PerformanceDiagnosisInput input;
		input.requestedFrequencyHz = 100;
		input.sampledVariables = 8;
		input.plannedProbeCallsPerSample = 1;
		input.completedSamples = 100;
		input.elapsedNs = 1'000'000'000;
		input.probeReadNs = 800'000'000;
		input.csvQueueCapacity = 1024;
		return input;
	}
}

TEST(PerformanceDiagnosisTest, WaitsForAStableMeasurementWindow)
{
	auto input = healthyInput();
	input.completedSamples = 99;
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::WarmingUp);
}

TEST(PerformanceDiagnosisTest, DoesNotReportDominantProbeTimeWhenTheRequestedRateIsMet)
{
	const PerformanceDiagnosis diagnosis = diagnosePerformance(healthyInput());
	EXPECT_EQ(diagnosis.bottleneck, PerformanceBottleneck::None);
	EXPECT_FALSE(diagnosis.active());
	EXPECT_DOUBLE_EQ(diagnosis.requestAttainmentRatio, 1.0);
}

TEST(PerformanceDiagnosisTest, CsvFailureAndHighQueueFillTakePriority)
{
	auto input = healthyInput();
	input.completedSamples = 1;
	input.csvFailed = true;
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::CsvBacklog);

	input.csvFailed = false;
	input.csvActive = true;
	input.csvQueueDepth = 768;
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::CsvBacklog);
}

TEST(PerformanceDiagnosisTest, DetectsRenderContentionFromFrameOrBufferEvidence)
{
	auto input = healthyInput();
	input.gui = {30, 33'300.0};
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::RenderContention);

	input.gui = {};
	input.sharedBufferLockWaitNs = 50'000'000;
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::RenderContention);
}

TEST(PerformanceDiagnosisTest, AddressFragmentationWinsWhenTooManyTransactionsLimitRate)
{
	auto input = healthyInput();
	input.completedSamples = 100;
	input.elapsedNs = 2'000'000'000;
	input.plannedProbeCallsPerSample = 8;
	const PerformanceDiagnosis diagnosis = diagnosePerformance(input);
	EXPECT_EQ(diagnosis.bottleneck, PerformanceBottleneck::AddressFragmentation);
	EXPECT_DOUBLE_EQ(diagnosis.requestAttainmentRatio, 0.5);
}

TEST(PerformanceDiagnosisTest, DetectsProbeLatencyWhenOneReadDominatesAnUnderperformingRun)
{
	auto input = healthyInput();
	input.completedSamples = 100;
	input.elapsedNs = 2'000'000'000;
	input.probeReadNs = 1'600'000'000;
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::ProbeLatency);
}

TEST(PerformanceDiagnosisTest, UsesDeadlineLimitedWhenNoConsumerOrProbeDominates)
{
	auto input = healthyInput();
	input.completedSamples = 100;
	input.elapsedNs = 1'250'000'000;
	input.probeReadNs = 100'000'000;
	input.deadlineMisses = 2;
	EXPECT_EQ(diagnosePerformance(input).bottleneck, PerformanceBottleneck::DeadlineLimited);
}
