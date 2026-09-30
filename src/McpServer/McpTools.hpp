#ifndef _MCPTOOLS_HPP
#define _MCPTOOLS_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "McpContext.hpp"
#include "McpToolRegistry.hpp"
#include "McpTypes.hpp"
#include "Plot.hpp"
#include "PlotGroupHandler.hpp"
#include "PlotHandler.hpp"
#include "Variable.hpp"
#include "VariableHandler.hpp"
#include "ViewerDataHandler.hpp"

namespace mcp
{
	/*
	 * Shared lookups and name mappings.
	 *
	 * Every mapping is defined in exactly one direction and back, so an
	 * unrecognised value produces the list of accepted spellings instead of a
	 * silent fallback that would make a wrong call look successful.
	 */

	std::shared_ptr<Variable> requireVariable(McpContext& context, const std::string& name);
	std::shared_ptr<Plot> requirePlot(McpContext& context, const std::string& name);

	/* An empty group name means the active group, which is what most tools want
	   and what the descriptions imply by not asking for one. */
	std::shared_ptr<PlotGroup> resolveGroup(McpContext& context, const std::string& groupName);
	std::string resolveGroupName(McpContext& context, const std::string& groupName);

	Variable::Type variableTypeFromString(const std::string& name);
	std::string variableTypeToString(Variable::Type type);

	Plot::Type plotTypeFromString(const std::string& name);
	std::string plotTypeToString(Plot::Type type);

	Variable::HighLevelType interpretationFromString(const std::string& name);
	std::string interpretationToString(Variable::HighLevelType type);

	/* The 'enum_labels' array of {"label", "value"} objects, in both directions.
	   A label may not be empty or repeated, because a value is turned back into
	   a name by looking the label list up. */
	std::vector<Variable::EnumLabel> enumLabelsFromJson(const Json& value, const std::string& field);
	Json enumLabelsToJson(const std::vector<Variable::EnumLabel>& labels);

	/* Both throw with the message the tool descriptions promise, so a client that
	   calls out of order is told which step it skipped. */
	void requireAcquisitionRunning(McpContext& context);
	void requireApiWritesEnabled(McpContext& context);

	/* True when the variable belongs to a plot of the active group, which is the
	   condition for the acquisition loop to read it. */
	bool isVariableSampled(McpContext& context, const std::string& name);

	/* Asks a running acquisition to stop, rebuild its read list and start again.
	   Returns false when acquisition is not running, in which case the next start
	   already picks the change up. */
	bool requestAcquisitionRestart(McpContext& context);

	/* Index of every kept sample when a buffer is longer than maxPoints. */
	std::vector<size_t> decimateIndices(size_t count, size_t maxPoints);

	/* Text used by the read tools when a variable is not being sampled. */
	std::string notSampledHint(const std::string& name);

	/* Every tool that needs a subsystem this build does not have reports through
	   this helper, so the wording stays in one place. */
	[[noreturn]] void reportUnavailable(const std::string& feature, const std::string& toolName);

	class ProjectTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class GroupTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class PlotTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class VariableTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class RecorderTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class AcquisitionTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class DataTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};

	class FlashingTools
	{
	   public:
		static void registerAll(ToolRegistry& registry);
	};
}  // namespace mcp

#endif
