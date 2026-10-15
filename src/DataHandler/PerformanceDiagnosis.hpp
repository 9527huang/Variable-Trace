#pragma once

#include <algorithm>
#include <cstdint>

enum class PerformanceBottleneck
{
	WarmingUp,
	None,
	ProbeLatency,
	AddressFragmentation,
	RenderContention,
	CsvBacklog,
	DeadlineLimited
};

struct PerformanceGuiMetrics
{
	uint64_t measuredFrames = 0;
	double frameBuildP95Us = 0.0;
};

struct PerformanceDiagnosisInput
{
	uint32_t requestedFrequencyHz = 0;
	uint32_t sampledVariables = 0;
	uint32_t plannedProbeCallsPerSample = 0;
	uint64_t completedSamples = 0;
	uint64_t deadlineMisses = 0;
	uint64_t skippedSamplePeriods = 0;
	uint64_t elapsedNs = 0;
	uint64_t probeReadNs = 0;
	uint64_t sharedBufferLockWaitNs = 0;
	uint64_t csvQueueDepth = 0;
	uint64_t csvQueueCapacity = 0;
	bool csvActive = false;
	bool csvFailed = false;
	PerformanceGuiMetrics gui;
};

struct PerformanceDiagnosis
{
	PerformanceBottleneck bottleneck = PerformanceBottleneck::WarmingUp;
	uint32_t requestedSamplingHz = 0;
	double actualSamplingHz = 0.0;
	double requestAttainmentRatio = 0.0;
	double deadlineMissRatio = 0.0;
	double probeReadRatio = 0.0;
	double sharedBufferWaitRatio = 0.0;
	double csvQueueFillRatio = 0.0;

	bool active() const
	{
		return bottleneck != PerformanceBottleneck::WarmingUp && bottleneck != PerformanceBottleneck::None;
	}
};

inline const char* performanceBottleneckCode(PerformanceBottleneck bottleneck)
{
	switch (bottleneck)
	{
		case PerformanceBottleneck::ProbeLatency:
			return "probe_latency";
		case PerformanceBottleneck::AddressFragmentation:
			return "address_fragmentation";
		case PerformanceBottleneck::RenderContention:
			return "render_contention";
		case PerformanceBottleneck::CsvBacklog:
			return "csv_backlog";
		case PerformanceBottleneck::DeadlineLimited:
			return "deadline_limited";
		case PerformanceBottleneck::None:
			return "none";
		default:
			return "warming_up";
	}
}

inline PerformanceDiagnosis diagnosePerformance(const PerformanceDiagnosisInput& input)
{
	constexpr uint64_t minimumSamples = 100;
	constexpr uint64_t minimumElapsedNs = 1'000'000'000;
	constexpr double minimumAttainment = 0.90;
	constexpr double deadlinePressureThreshold = 0.01;
	constexpr double probeDominanceThreshold = 0.50;
	constexpr double sharedBufferWaitThreshold = 0.05;
	constexpr double csvQueueFillThreshold = 0.75;
	constexpr double frameBuildP95LimitUs = 33'300.0;

	PerformanceDiagnosis result;
	result.requestedSamplingHz = input.requestedFrequencyHz;
	const double elapsedSeconds = static_cast<double>(input.elapsedNs) / 1'000'000'000.0;
	if (elapsedSeconds > 0.0)
		result.actualSamplingHz = static_cast<double>(input.completedSamples) / elapsedSeconds;
	if (input.requestedFrequencyHz > 0)
		result.requestAttainmentRatio = result.actualSamplingHz / static_cast<double>(input.requestedFrequencyHz);
	if (input.completedSamples > 0)
		result.deadlineMissRatio = static_cast<double>(std::max(input.deadlineMisses, input.skippedSamplePeriods)) / static_cast<double>(input.completedSamples);
	if (input.elapsedNs > 0)
	{
		result.probeReadRatio = static_cast<double>(input.probeReadNs) / static_cast<double>(input.elapsedNs);
		result.sharedBufferWaitRatio = static_cast<double>(input.sharedBufferLockWaitNs) / static_cast<double>(input.elapsedNs);
	}
	if (input.csvQueueCapacity > 0)
		result.csvQueueFillRatio = static_cast<double>(input.csvQueueDepth) / static_cast<double>(input.csvQueueCapacity);

	if (input.csvFailed || (input.csvActive && result.csvQueueFillRatio >= csvQueueFillThreshold))
	{
		result.bottleneck = PerformanceBottleneck::CsvBacklog;
		return result;
	}

	if (input.completedSamples < minimumSamples || input.elapsedNs < minimumElapsedNs)
		return result;

	if ((input.gui.measuredFrames >= 30 && input.gui.frameBuildP95Us >= frameBuildP95LimitUs) ||
		result.sharedBufferWaitRatio >= sharedBufferWaitThreshold)
	{
		result.bottleneck = PerformanceBottleneck::RenderContention;
		return result;
	}

	const bool underperforming = input.requestedFrequencyHz > 0 && result.requestAttainmentRatio < minimumAttainment;
	const bool deadlinePressure = result.deadlineMissRatio >= deadlinePressureThreshold;
	if (!underperforming && !deadlinePressure)
	{
		result.bottleneck = PerformanceBottleneck::None;
		return result;
	}

	const bool fragmented = input.sampledVariables >= 4 && input.plannedProbeCallsPerSample > 2 &&
		input.plannedProbeCallsPerSample * 2 >= input.sampledVariables;
	if (fragmented)
		result.bottleneck = PerformanceBottleneck::AddressFragmentation;
	else if (result.probeReadRatio >= probeDominanceThreshold)
		result.bottleneck = PerformanceBottleneck::ProbeLatency;
	else
		result.bottleneck = PerformanceBottleneck::DeadlineLimited;
	return result;
}
