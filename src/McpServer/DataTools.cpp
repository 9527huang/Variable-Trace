#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "McpTools.hpp"

namespace mcp
{
	namespace
	{
		constexpr int64_t defaultCurvePoints = 200;
		constexpr int64_t maxCurvePoints = 10000;

		struct SampleBuffer
		{
			std::vector<double> times;
			std::vector<double> values;
		};

		/*
		 * Copies the ring buffer of the series that carries a variable into
		 * chronological order.
		 *
		 * A ScrollingBuffer stores its newest value at offset - 1 and its oldest at
		 * offset once it has wrapped, so the position of the n-th oldest sample is
		 * (n + offset) % count. The application mutex is taken while copying, which
		 * is what the plotting code does for the same reason.
		 */
		bool collectSamples(McpContext& context, const std::string& name, SampleBuffer& samples)
		{
			if (context.plotHandler == nullptr || context.mtx == nullptr)
				return false;

			uint32_t bestCount = 0;
			std::shared_ptr<Plot> bestPlot;
			std::shared_ptr<Plot::Series> bestSeries;

			/* A variable can be shown in several plots, and the one holding the most
			   samples is the one worth reporting. */
			for (const std::shared_ptr<Plot>& plot : *context.plotHandler)
			{
				for (const auto& [seriesName, series] : plot->getSeriesMap())
				{
					if (series->var == nullptr || series->var->getName() != name)
						continue;

					const uint32_t count = series->buffer->getSize();

					if (count > bestCount)
					{
						bestCount = count;
						bestPlot = plot;
						bestSeries = series;
					}
				}
			}

			if (bestCount == 0)
				return false;

			ScrollingBuffer<double>* timeAxis = bestPlot->getXAxisSeries();

			std::lock_guard<std::mutex> lock(*context.mtx);
			timeAxis->copyData();
			bestSeries->buffer->copyData();

			/* Both buffers are appended in the same step, so their offsets match and
			   only the smaller count matters. */
			const uint32_t count = std::min(bestSeries->buffer->getSize(), timeAxis->getSize());
			const uint32_t offset = bestSeries->buffer->getOffset();
			const double* values = bestSeries->buffer->getFirstElementCopy();
			const double* times = timeAxis->getFirstElementCopy();

			if (count == 0)
				return false;

			samples.times.reserve(count);
			samples.values.reserve(count);

			for (uint32_t index = 0; index < count; index++)
			{
				const uint32_t position = (index + offset) % count;
				samples.times.push_back(times[position]);
				samples.values.push_back(values[position]);
			}

			return true;
		}

		/* Timestamps grow monotonically, so the window start is a binary search. */
		void trimToLastSeconds(SampleBuffer& samples, double lastSeconds)
		{
			if (samples.times.empty() || lastSeconds <= 0.0)
				return;

			const double threshold = samples.times.back() - lastSeconds;
			const auto first = std::lower_bound(samples.times.begin(), samples.times.end(), threshold);
			const size_t kept = static_cast<size_t>(std::distance(samples.times.begin(), first));

			samples.times.erase(samples.times.begin(), samples.times.begin() + static_cast<std::ptrdiff_t>(kept));
			samples.values.erase(samples.values.begin(), samples.values.begin() + static_cast<std::ptrdiff_t>(kept));
		}

		struct Statistics
		{
			double min = 0.0;
			double max = 0.0;
			double mean = 0.0;
			double stddev = 0.0;
		};

		/* Population standard deviation, the same definition the statistics window
		   of the application uses. */
		Statistics computeStatistics(const std::vector<double>& values)
		{
			Statistics result;
			result.min = values.front();
			result.max = values.front();

			double sum = 0.0;

			for (const double value : values)
			{
				result.min = std::min(result.min, value);
				result.max = std::max(result.max, value);
				sum += value;
			}

			result.mean = sum / static_cast<double>(values.size());

			double variance = 0.0;

			for (const double value : values)
				variance += (value - result.mean) * (value - result.mean);

			result.stddev = std::sqrt(variance / static_cast<double>(values.size()));

			return result;
		}

		Json getVariableValue(McpContext& context, const Json& arguments)
		{
			if (context.variableHandler == nullptr)
				reportUnavailable("the variable table", "get_variable_value");

			const bool batch = has(arguments, "variables");

			if (!batch && !has(arguments, "name"))
				throw ToolError("Pass 'name' with a single variable name, or 'variables' with an array of names.");

			if (batch)
			{
				Json results = Json::array();

				for (const std::string& name : stringArray(arguments.at("variables"), "variables"))
				{
					/* A missing variable is reported inside the entry rather than as a
					   failure, so one bad name does not hide the rest of the batch. */
					if (!context.variableHandler->contains(name))
					{
						results.push_back(Json::object({{"n", name}, {"v", 0.0}, {"ok", false}}));
						continue;
					}

					const std::shared_ptr<Variable> variable = context.variableHandler->getVariable(name);
					const bool sampled = isVariableSampled(context, name);

					results.push_back(Json::object({{"n", name}, {"v", variable->getValue()}, {"ok", sampled}}));
				}

				return results;
			}

			const std::string name = requireString(arguments, "name");
			const std::shared_ptr<Variable> variable = requireVariable(context, name);
			const bool sampled = isVariableSampled(context, name);

			Json result = Json::object({{"name", name},
										{"value", variable->getValue()},
										{"is_found", variable->getIsFound()},
										{"is_sampled", sampled}});

			if (!sampled)
				result["hint"] = notSampledHint(name);

			return result;
		}

		Json getVariableStats(McpContext& context, const Json& arguments)
		{
			if (context.variableHandler == nullptr || context.plotHandler == nullptr)
				reportUnavailable("the sample buffers", "get_variable_stats");

			requireAcquisitionRunning(context);

			const bool batch = has(arguments, "variables");

			if (!batch && !has(arguments, "name"))
				throw ToolError("Pass 'name' with a single variable name, or 'variables' with an array of names.");

			const std::optional<double> lastSeconds = optionalNumber(arguments, "last_s");

			if (lastSeconds.has_value() && *lastSeconds <= 0.0)
				throw ToolError("Field 'last_s' must be greater than zero.");

			Json results = Json::array();

			auto measure = [&](const std::string& name, Json& target)
			{
				if (!context.variableHandler->contains(name))
				{
					if (!batch)
						throw ToolError("Variable '" + name + "' does not exist. Use add_variable to create it.");

					results.push_back(Json::object({{"n", name}, {"cur", 0.0}, {"min", 0.0}, {"max", 0.0}, {"d", 0.0}, {"mean", 0.0}, {"std", 0.0}, {"ok", false}}));
					return;
				}

				const std::shared_ptr<Variable> variable = context.variableHandler->getVariable(name);
				const bool sampled = isVariableSampled(context, name);

				SampleBuffer samples;

				if (collectSamples(context, name, samples) && lastSeconds.has_value())
					trimToLastSeconds(samples, *lastSeconds);

				if (samples.values.empty())
				{
					if (!batch)
					{
						const std::string reason = sampled ? std::string("No samples collected yet.") : ("No samples collected yet. " + notSampledHint(name));
						throw ToolError(reason);
					}

					results.push_back(Json::object({{"n", name}, {"cur", 0.0}, {"min", 0.0}, {"max", 0.0}, {"d", 0.0}, {"mean", 0.0}, {"std", 0.0}, {"ok", false}}));
					return;
				}

				const Statistics statistics = computeStatistics(samples.values);

				if (batch)
				{
					results.push_back(Json::object({{"n", name},
													{"cur", variable->getValue()},
													{"min", statistics.min},
													{"max", statistics.max},
													{"d", statistics.max - statistics.min},
													{"mean", statistics.mean},
													{"std", statistics.stddev},
													{"ok", sampled}}));
					return;
				}

				target = Json::object({{"name", name},
									   {"current", variable->getValue()},
									   {"min", statistics.min},
									   {"max", statistics.max},
									   {"delta", statistics.max - statistics.min},
									   {"mean", statistics.mean},
									   {"std_dev", statistics.stddev},
									   {"is_found", variable->getIsFound()},
									   {"is_sampled", sampled},
									   {"time_start_s", samples.times.front()},
									   {"time_end_s", samples.times.back()},
									   {"samples", samples.values.size()}});
			};

			if (!batch)
			{
				Json single;
				measure(requireString(arguments, "name"), single);

				if (!isVariableSampled(context, single.at("name").get<std::string>()))
					single["hint"] = notSampledHint(single.at("name").get<std::string>());

				return single;
			}

			for (const std::string& name : stringArray(arguments.at("variables"), "variables"))
			{
				Json unused;
				measure(name, unused);
			}

			return results;
		}

		Json setVariableValue(McpContext& context, const Json& arguments)
		{
			const std::string name = requireString(arguments, "name");

			if (!has(arguments, "value"))
				throw ToolError("Field 'value' is required.");

			const double value = *optionalNumber(arguments, "value");

			requireAcquisitionRunning(context);
			requireApiWritesEnabled(context);

			const std::shared_ptr<Variable> variable = requireVariable(context, name);

			if (!variable->getIsFound())
				throw ToolError("Variable '" + name + "' has no address because it was not found in the ELF file. Call refresh_variable_addresses first.");

			/* The write goes through the data handler, which applies the per
			   variable range the user configured with configure_variable. The
			   outer permission was checked above; this is the inner guard, and it
			   sits in the handler so the plot table obeys the same limits. */
			if (!context.viewerDataHandler->writeVariable(*variable, value))
			{
				const std::string reason = context.viewerDataHandler->getLastWriteError();

				if (!reason.empty())
					throw ToolError(reason + ". Use configure_variable to change the limits.");

				const std::string probeReason = context.viewerDataHandler->getLastReaderError();
				throw ToolError("The write to '" + name + "' failed." + (probeReason.empty() ? "" : " " + probeReason));
			}

			return Json::object({{"name", name}, {"value", value}, {"written", true}});
		}

		Json getCurveData(McpContext& context, const Json& arguments)
		{
			if (context.variableHandler == nullptr || context.plotHandler == nullptr)
				reportUnavailable("the sample buffers", "get_curve_data");

			const std::string name = requireString(arguments, "name");

			requireVariable(context, name);

			int64_t maxPoints = defaultCurvePoints;

			if (has(arguments, "max_points"))
			{
				maxPoints = *optionalInteger(arguments, "max_points");

				if (maxPoints < 1 || maxPoints > maxCurvePoints)
					throw ToolError("Field 'max_points' must be between 1 and 10000.");
			}

			const std::optional<double> lastSeconds = optionalNumber(arguments, "last_s");

			if (lastSeconds.has_value() && *lastSeconds <= 0.0)
				throw ToolError("Field 'last_s' must be greater than zero.");

			SampleBuffer samples;

			if (collectSamples(context, name, samples) && lastSeconds.has_value())
				trimToLastSeconds(samples, *lastSeconds);

			if (samples.values.empty())
			{
				if (!isVariableSampled(context, name))
					throw ToolError("No samples collected yet. " + notSampledHint(name));

				throw ToolError("No samples collected yet.");
			}

			const std::vector<size_t> indices = decimateIndices(samples.values.size(), static_cast<size_t>(maxPoints));

			Json times = Json::array();
			Json values = Json::array();

			for (const size_t index : indices)
			{
				times.push_back(samples.times[index]);
				values.push_back(samples.values[index]);
			}

			return Json::object({{"name", name},
								 {"total", samples.values.size()},
								 {"returned", indices.size()},
								 {"time_start_s", samples.times.front()},
								 {"time_end_s", samples.times.back()},
								 {"times", times},
								 {"values", values}});
		}
	}  // namespace

	void DataTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "get_variable_value",
					  .description = "Get current sampled value(s). Single: 'name' -> {name, value, is_found, is_sampled}. "
									 "Batch: 'variables' as JSON array of names -> [{n, v, ok}, ...]. is_sampled=false means the "
									 "value is stale, see 'hint'.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name (single mode)")},
																{"variables", arrayProperty("JSON array of names (batch mode)", stringProperty("Variable name"))}})),
					  .handler = getVariableValue});

		registry.add({.name = "get_variable_stats",
					  .description = "Compute statistics over the sample buffer. Requires acquisition data. Single: 'name' -> "
									 "{name, current, min, max, delta, mean, std_dev, is_found, is_sampled, time_start_s, time_end_s}. "
									 "Batch: 'variables' as JSON array of names -> [{n, cur, min, max, d, mean, std, ok}, ...]. "
									 "'last_s': limit to last N seconds. is_sampled=false means stats are 0, see 'hint'.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name (single mode)")},
																{"variables", arrayProperty("JSON array of names (batch mode)", stringProperty("Variable name"))},
																{"last_s", numberProperty("Limit to last N seconds (omit = full buffer)")}})),
					  .handler = getVariableStats});

		registry.add({.name = "set_variable_value",
					  .description = "Write a value to a variable on the target MCU. Requires active acquisition. Rejected if value is outside configured write limits. Returns {name, value, written}.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name")},
																{"value", numberProperty("Value to write")}}),
												  {"name", "value"}),
					  .handler = setVariableValue});

		registry.add({.name = "get_curve_data",
					  .description = "Return sampled values from a variable's ring-buffer in chronological order. 'max_points': max points after decimation (1 - 10000, default 200). 'last_s': limit to last N seconds. Returns {name, total, returned, time_start_s, time_end_s, times:[...], values:[...]}. Use get_variable_stats when only range/mean is needed.",
					  .inputSchema = objectSchema(Json::object({{"name", stringProperty("Variable name")},
																{"max_points", integerProperty("Max points to return (1 - 10000, default 200)")},
																{"last_s", numberProperty("Return only the last N seconds of data")}}),
												  {"name"}),
					  .handler = getCurveData});
	}
}  // namespace mcp
