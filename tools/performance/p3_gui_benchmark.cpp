#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "DisplayDecimator.hpp"
#include "imgui.h"
#include "implot.h"

namespace
{
	struct Result
	{
		std::string scenario;
		std::string path;
		uint32_t width = 0;
		uint32_t plots = 0;
		uint32_t series = 0;
		uint32_t inputPoints = 0;
		uint64_t displayPointsPerFrame = 0;
		double p50Us = 0.0;
		double p95Us = 0.0;
		double p99Us = 0.0;
	};

	double percentileUs(std::vector<uint64_t> values, double percentile)
	{
		std::sort(values.begin(), values.end());
		const std::size_t index = static_cast<std::size_t>(percentile * static_cast<double>(values.size() - 1));
		return static_cast<double>(values[index]) / 1000.0;
	}

	Result runScenario(const std::string& scenario, uint32_t width, uint32_t plots, uint32_t seriesCount,
		bool useP3Policy, uint32_t pointCount = 10000, uint32_t visiblePoints = 10000,
		uint32_t warmupFrames = 30, uint32_t measuredFrames = 240)
	{
		constexpr float height = 1080.0f;

		std::vector<double> x(pointCount);
		std::iota(x.begin(), x.end(), 0.0);
		std::vector<std::vector<double>> y(seriesCount, std::vector<double>(pointCount));
		std::vector<DisplayPoints> displayCache(static_cast<std::size_t>(plots) * seriesCount);
		for (uint32_t series = 0; series < seriesCount; ++series)
		{
			for (uint32_t point = 0; point < pointCount; ++point)
				y[series][point] = std::sin(static_cast<double>(point) * 0.007 + series * 0.31);
			y[series][pointCount / 2] += 20.0 + series;
		}

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(width), height);
		io.DeltaTime = 1.0f / 60.0f;
		std::vector<uint64_t> durations;
		durations.reserve(measuredFrames);
		uint64_t submitted = 0;
		const double visibleMin = static_cast<double>(pointCount - std::min(pointCount, visiblePoints));

		for (uint32_t frame = 0; frame < warmupFrames + measuredFrames; ++frame)
		{
			const auto started = std::chrono::steady_clock::now();
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
			ImGui::Begin("P3 GUI benchmark", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

			const float plotHeight = (height - 30.0f) / static_cast<float>(plots);
			uint64_t frameSubmitted = 0;
			for (uint32_t plot = 0; plot < plots; ++plot)
			{
				const std::string plotName = "Plot##" + std::to_string(plot);
				if (ImPlot::BeginPlot(plotName.c_str(), ImVec2(-1, plotHeight), ImPlotFlags_NoLegend))
				{
					ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
					ImPlot::SetupAxisLimits(ImAxis_X1, visibleMin, pointCount - 1.0, ImPlotCond_Always);
					ImPlot::SetupAxisLimits(ImAxis_Y1, -2.0, 45.0, ImPlotCond_Always);
					const uint32_t pixelColumns = std::max(1u, width - 40u);
					const bool decimated = useP3Policy && shouldDecimateDisplaySeries(pointCount, pixelColumns);

					for (uint32_t series = 0; series < seriesCount; ++series)
					{
						const std::string label = "Series##" + std::to_string(series);
						if (decimated)
						{
							DisplayPoints& points = displayCache[static_cast<std::size_t>(plot) * seriesCount + series];
							decimateDisplaySeries(x.data(), y[series].data(), pointCount, 0,
								visibleMin, pointCount - 1.0, pixelColumns, points);
							ImPlot::PlotLine(label.c_str(), points.x.data(), points.y.data(), static_cast<int>(points.x.size()));
							frameSubmitted += points.x.size();
						}
						else
						{
							ImPlot::PlotLine(label.c_str(), x.data(), y[series].data(), pointCount);
							frameSubmitted += pointCount;
						}
					}
					ImPlot::EndPlot();
				}
			}
			ImGui::End();
			ImGui::Render();
			const uint64_t duration = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count());

			if (frame >= warmupFrames)
			{
				durations.push_back(duration);
				submitted = frameSubmitted;
			}
		}

		return {.scenario = scenario,
				.path = useP3Policy ? "p3_policy" : "raw",
				.width = width,
				.plots = plots,
				.series = seriesCount,
				.inputPoints = pointCount,
				.displayPointsPerFrame = submitted,
				.p50Us = percentileUs(durations, 0.50),
				.p95Us = percentileUs(durations, 0.95),
				.p99Us = percentileUs(durations, 0.99)};
	}

	void writeResults(std::ostream& output, const std::vector<Result>& results)
	{
		output << std::fixed << std::setprecision(3) << "{\n  \"benchmark\": \"implot_cpu_no_gpu\",\n  \"results\": [\n";
		for (std::size_t index = 0; index < results.size(); ++index)
		{
			const Result& result = results[index];
			output << "    {\"scenario\": \"" << result.scenario << "\", \"path\": \"" << result.path
				   << "\", \"logical_width\": " << result.width << ", \"plots\": " << result.plots
				   << ", \"series_per_plot\": " << result.series << ", \"input_points_per_series\": " << result.inputPoints
				   << ", \"display_points_per_frame\": " << result.displayPointsPerFrame
				   << ", \"frame_p50_us\": " << result.p50Us << ", \"frame_p95_us\": " << result.p95Us
				   << ", \"frame_p99_us\": " << result.p99Us << "}" << (index + 1 == results.size() ? "\n" : ",\n");
		}
		output << "  ]\n}\n";
	}
}

int main(int argc, char** argv)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->AddFontDefault();
	unsigned char* pixels = nullptr;
	int textureWidth = 0;
	int textureHeight = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &textureWidth, &textureHeight);
	io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(1)));

	std::vector<Result> results;
	for (uint32_t width : {1920u, 3840u})
	{
		for (const auto [plots, series] : {std::pair{1u, 8u}, std::pair{4u, 20u}})
		{
			const std::string scenario = std::to_string(plots) + "x" + std::to_string(series);
			results.push_back(runScenario(scenario, width, plots, series, false));
			results.push_back(runScenario(scenario, width, plots, series, true));
		}
	}

	constexpr uint32_t maximumSeries = 20;
	constexpr uint64_t memoryBudgetBytes = 512ull * 1024ull * 1024ull;
	const uint32_t maximumPoints = static_cast<uint32_t>(memoryBudgetBytes /
		(2ull * sizeof(double) * (maximumSeries + 1ull)));
	results.push_back(runScenario("max_history_1x20", 1920, 1, maximumSeries, false, maximumPoints, 10000, 3, 10));
	results.push_back(runScenario("max_history_1x20", 1920, 1, maximumSeries, true, maximumPoints, 10000, 3, 10));

	writeResults(std::cout, results);
	if (argc == 3 && std::string(argv[1]) == "--output")
	{
		std::ofstream file(argv[2]);
		if (!file)
			return 2;
		writeResults(file, results);
	}

	ImPlot::DestroyContext();
	ImGui::DestroyContext();
	return 0;
}
