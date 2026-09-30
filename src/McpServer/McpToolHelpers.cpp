#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "McpTools.hpp"

namespace mcp
{
	namespace
	{
		const std::map<std::string, Variable::Type> variableTypes{{"u8", Variable::Type::U8},
																  {"i8", Variable::Type::I8},
																  {"u16", Variable::Type::U16},
																  {"i16", Variable::Type::I16},
																  {"u32", Variable::Type::U32},
																  {"i32", Variable::Type::I32},
																  {"f32", Variable::Type::F32}};

		const std::map<std::string, Plot::Type> plotTypes{{"curve", Plot::Type::CURVE},
														  {"bar", Plot::Type::BAR},
														  {"table", Plot::Type::TABLE},
														  {"xy", Plot::Type::XY}};

		const std::map<std::string, Variable::HighLevelType> interpretations{{"none", Variable::HighLevelType::NONE},
																			{"signed_frac", Variable::HighLevelType::SIGNEDFRAC},
																			{"unsigned_frac", Variable::HighLevelType::UNSIGNEDFRAC},
																			{"enum", Variable::HighLevelType::ENUM},
																			{"custom_enum", Variable::HighLevelType::CUSTOM_ENUM}};

		std::string joinNames(const std::vector<std::string>& names)
		{
			std::string result;

			for (size_t index = 0; index < names.size(); index++)
			{
				if (index > 0)
					result += ", ";

				result += "'" + names[index] + "'";
			}

			return result;
		}

		template <typename T>
		std::vector<std::string> keysOf(const std::map<std::string, T>& mapping)
		{
			std::vector<std::string> keys;
			keys.reserve(mapping.size());

			for (const auto& [key, value] : mapping)
				keys.push_back(key);

			return keys;
		}
	}  // namespace

	std::shared_ptr<Variable> requireVariable(McpContext& context, const std::string& name)
	{
		if (context.variableHandler == nullptr)
			throw ToolError("No variable table is available.");

		if (!context.variableHandler->contains(name))
			throw ToolError("Variable '" + name + "' does not exist. Use add_variable to create it.");

		return context.variableHandler->getVariable(name);
	}

	std::shared_ptr<Plot> requirePlot(McpContext& context, const std::string& name)
	{
		if (context.plotHandler == nullptr)
			throw ToolError("No plot handler is available.");

		if (!context.plotHandler->checkIfPlotExists(name))
			throw ToolError("Plot '" + name + "' does not exist. Use add_plot to create it.");

		return context.plotHandler->getPlot(name);
	}

	std::string resolveGroupName(McpContext& context, const std::string& groupName)
	{
		if (context.plotGroupHandler == nullptr)
			throw ToolError("No group handler is available.");

		if (groupName.empty())
			return context.plotGroupHandler->getActiveGroupName();

		if (!context.plotGroupHandler->checkIfGroupExists(groupName))
			throw ToolError("Group '" + groupName + "' does not exist. Use list_groups to see the available groups.");

		return groupName;
	}

	std::shared_ptr<PlotGroup> resolveGroup(McpContext& context, const std::string& groupName)
	{
		return context.plotGroupHandler->getGroup(resolveGroupName(context, groupName));
	}

	Variable::Type variableTypeFromString(const std::string& name)
	{
		auto entry = variableTypes.find(name);

		if (entry == variableTypes.end())
			throw ToolError("Unknown variable type '" + name + "'. Use one of " + joinNames(keysOf(variableTypes)) + ".");

		return entry->second;
	}

	std::string variableTypeToString(Variable::Type type)
	{
		for (const auto& [name, value] : variableTypes)
		{
			if (value == type)
				return name;
		}

		return "unknown";
	}

	Plot::Type plotTypeFromString(const std::string& name)
	{
		auto entry = plotTypes.find(name);

		if (entry == plotTypes.end())
			throw ToolError("Unknown plot type '" + name + "'. Use one of " + joinNames(keysOf(plotTypes)) + ".");

		return entry->second;
	}

	std::string plotTypeToString(Plot::Type type)
	{
		for (const auto& [name, value] : plotTypes)
		{
			if (value == type)
				return name;
		}

		return "unknown";
	}

	Variable::HighLevelType interpretationFromString(const std::string& name)
	{
		auto entry = interpretations.find(name);

		if (entry == interpretations.end())
			throw ToolError("Unknown interpretation '" + name + "'. Use one of " + joinNames(keysOf(interpretations)) + ".");

		return entry->second;
	}

	std::string interpretationToString(Variable::HighLevelType type)
	{
		for (const auto& [name, value] : interpretations)
		{
			if (value == type)
				return name;
		}

		return "none";
	}

	std::vector<Variable::EnumLabel> enumLabelsFromJson(const Json& value, const std::string& field)
	{
		if (!value.is_array())
			throw ToolError("Field '" + field + "' must be a JSON array of {\"label\": string, \"value\": number}.");

		std::vector<Variable::EnumLabel> labels;
		labels.reserve(value.size());

		for (const Json& element : value)
		{
			if (!element.is_object())
				throw ToolError("Every entry of '" + field + "' must be an object with 'label' and 'value'.");

			const std::string label = requireString(element, "label");

			/* An empty label is indistinguishable from "this value has no
			   name", which is what the reverse lookup answers with when it finds
			   nothing. */
			if (label.empty())
				throw ToolError("Label cannot be empty!");

			if (!element.contains("value") || !element.at("value").is_number())
				throw ToolError("Value must be a valid integer!");

			const double number = element.at("value").get<double>();

			if (number != std::floor(number) || number < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
				number > static_cast<double>(std::numeric_limits<int64_t>::max()))
				throw ToolError("Value must be a valid integer!");

			/* The reverse lookup turns a value back into a name, so two entries
			   with the same label would make that answer ambiguous. */
			for (const Variable::EnumLabel& existing : labels)
			{
				if (existing.label == label)
					throw ToolError("Label already exists!");
			}

			labels.push_back({label, static_cast<int64_t>(number)});
		}

		return labels;
	}

	Json enumLabelsToJson(const std::vector<Variable::EnumLabel>& labels)
	{
		Json result = Json::array();

		for (const Variable::EnumLabel& entry : labels)
			result.push_back(Json::object({{"label", entry.label}, {"value", entry.value}}));

		return result;
	}

	void requireAcquisitionRunning(McpContext& context)
	{
		if (context.viewerDataHandler == nullptr)
			throw ToolError("Acquisition is not running. Call start_acquisition first.");

		if (context.viewerDataHandler->getStateImmediate() != DataHandlerBase::State::RUN)
			throw ToolError("Acquisition is not running. Call start_acquisition first.");
	}

	void requireApiWritesEnabled(McpContext& context)
	{
		if (!context.getApiWritesEnabled || !context.getApiWritesEnabled())
			throw ToolError("Writing to the target through the API is disabled. Enable it in the application preferences first.");
	}

	bool isVariableSampled(McpContext& context, const std::string& name)
	{
		if (context.plotGroupHandler == nullptr || context.variableHandler == nullptr)
			return false;

		if (!context.variableHandler->contains(name))
			return false;

		return context.variableHandler->getVariable(name)->getIsCurrentlySampled();
	}

	bool requestAcquisitionRestart(McpContext& context)
	{
		if (context.viewerDataHandler == nullptr)
			return false;

		if (context.viewerDataHandler->getStateImmediate() != DataHandlerBase::State::RUN)
			return false;

		context.viewerDataHandler->requestRestart();

		return true;
	}

	std::vector<size_t> decimateIndices(size_t count, size_t maxPoints)
	{
		std::vector<size_t> indices;

		if (count == 0)
			return indices;

		if (maxPoints == 0 || maxPoints >= count)
		{
			indices.resize(count);

			for (size_t index = 0; index < count; index++)
				indices[index] = index;

			return indices;
		}

		/* Even spacing keeps the shape of the curve. The last sample is always
		   included, because the newest value is usually the interesting one. */
		indices.reserve(maxPoints);

		for (size_t index = 0; index < maxPoints; index++)
		{
			const size_t position = static_cast<size_t>((static_cast<double>(index) * static_cast<double>(count - 1)) / static_cast<double>(maxPoints - 1));

			if (indices.empty() || indices.back() != position)
				indices.push_back(position);
		}

		return indices;
	}

	std::string notSampledHint(const std::string& name)
	{
		return "Variable '" + name + "' is not added to a plot in the active group, so it is not being sampled (data might be stale). Use add_variable_to_plot to start sampling it.";
	}

	void reportUnavailable(const std::string& feature, const std::string& toolName)
	{
		throw ToolError("'" + toolName + "' is not available in this build: " + feature +
						" has not been implemented yet. Use get_instance_info to see which subsystems this build supports.");
	}
}  // namespace mcp
