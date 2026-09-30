#include <string>

#include "McpTools.hpp"

namespace mcp
{
	namespace
	{
		Json addPlot(McpContext& context, const Json& arguments)
		{
			if (context.plotHandler == nullptr)
				reportUnavailable("plots", "add_plot");

			const std::string name = requireString(arguments, "name");
			const std::optional<std::string> typeName = optionalString(arguments, "type");
			const std::string groupName = resolveGroupName(context, optionalString(arguments, "group").value_or(""));

			/* PlotHandler::addPlot replaces a plot that already carries the name,
			   which would throw away its series and the data it collected, so an
			   existing plot is reused instead. */
			const bool existed = context.plotHandler->checkIfPlotExists(name);
			auto plot = existed ? context.plotHandler->getPlot(name) : context.plotHandler->addPlot(name);

			if (typeName.has_value())
				plot->setType(plotTypeFromString(*typeName));

			context.plotGroupHandler->getGroup(groupName)->addPlot(plot);

			return Json::object({{"plot", name},
								 {"group", groupName},
								 {"type", plotTypeToString(plot->getType())},
								 {"reused", existed}});
		}

		Json renamePlot(McpContext& context, const Json& arguments)
		{
			if (context.plotHandler == nullptr)
				reportUnavailable("plots", "rename_plot");

			const std::string name = requireString(arguments, "name");
			const std::string newName = requireString(arguments, "new_name");

			requirePlot(context, name);

			if (context.plotHandler->checkIfPlotExists(newName))
				throw ToolError("Plot '" + newName + "' already exists.");

			context.plotHandler->renamePlot(name, newName);
			context.plotGroupHandler->renamePlotInAllGroups(name, newName);

			return Json::object({{"name", name}, {"new_name", newName}});
		}

		Json deletePlot(McpContext& context, const Json& arguments)
		{
			if (context.plotHandler == nullptr || context.plotGroupHandler == nullptr)
				reportUnavailable("plots", "delete_plot");

			const std::string name = requireString(arguments, "name");
			const std::string groupName = resolveGroupName(context, optionalString(arguments, "group").value_or(""));

			requirePlot(context, name);

			context.plotGroupHandler->getGroup(groupName)->removePlot(name);

			/* The plot is only dropped from the handler once no group refers to it,
			   so deleting it from one group cannot damage another. */
			bool stillUsed = false;

			for (const auto& [otherGroupName, otherGroup] : *context.plotGroupHandler)
			{
				for (const auto& [plotName, entry] : *otherGroup)
				{
					if (plotName == name)
						stillUsed = true;
				}
			}

			if (!stillUsed)
				context.plotHandler->removePlot(name);

			if (requestAcquisitionRestart(context))
				return Json::object({{"deleted", name}, {"group", groupName}, {"removed_from_handler", !stillUsed}, {"acquisition_restarted", true}});

			return Json::object({{"deleted", name}, {"group", groupName}, {"removed_from_handler", !stillUsed}});
		}

		Json addVariableToPlot(McpContext& context, const Json& arguments)
		{
			if (context.plotHandler == nullptr)
				reportUnavailable("plots", "add_variable_to_plot");

			const std::string plotName = requireString(arguments, "plot");
			const std::string groupName = resolveGroupName(context, optionalString(arguments, "group").value_or(""));

			const bool batch = has(arguments, "variables");

			if (!batch && !has(arguments, "variable"))
				throw ToolError("Pass 'variable' with a single name, or 'variables' with an array of names.");

			const std::vector<std::string> names = batch ? stringArray(arguments.at("variables"), "variables") : stringArray(arguments.at("variable"), "variable");

			auto group = context.plotGroupHandler->getGroup(groupName);
			auto plot = context.plotHandler->checkIfPlotExists(plotName) ? context.plotHandler->getPlot(plotName) : context.plotHandler->addPlot(plotName);

			group->addPlot(plot);

			Json results = Json::array();
			bool anyAdded = false;

			for (const std::string& name : names)
			{
				std::string status = "added";

				if (!context.variableHandler->contains(name))
					status = "not_found";
				else if (plot->getSeriesMap().contains(name))
					status = "already_in_plot";
				else
				{
					plot->addSeries(context.variableHandler->getVariable(name).get());
					anyAdded = true;
				}

				results.push_back(Json::object({{"name", name}, {"status", status}}));
			}

			const bool restarted = anyAdded && requestAcquisitionRestart(context);

			return Json::object({{"plot", plotName}, {"group", groupName}, {"results", results}, {"acquisition_restarted", restarted}});
		}
	}  // namespace

	void PlotTools::registerAll(ToolRegistry& registry)
	{
		registry.add({.name = "add_plot",
					  .description = "Add a new plot to a group. 'type': 'curve' (default), 'bar', 'table', or 'xy'.",
					  .inputSchema = objectSchema(Json::object({{"group", stringProperty("Group name (omit for the active group)")},
																{"name", stringProperty("Plot name")},
																{"type", enumProperty("'curve', 'bar', 'table', or 'xy'", {"curve", "bar", "table", "xy"})}}),
												  {"name"}),
					  .handler = addPlot});

		registry.add({.name = "rename_plot",
					  .description = "Rename a plot within a group.",
					  .inputSchema = objectSchema(Json::object({{"group", stringProperty("Group name (omit for the active group)")},
																{"name", stringProperty("Current plot name")},
																{"new_name", stringProperty("New plot name")}}),
												  {"name", "new_name"}),
					  .handler = renamePlot});

		registry.add({.name = "delete_plot",
					  .description = "Delete a plot from a group.",
					  .inputSchema = objectSchema(Json::object({{"group", stringProperty("Group name (omit for the active group)")},
																{"name", stringProperty("Plot name")}}),
												  {"name"}),
					  .handler = deletePlot});

		registry.add({.name = "add_variable_to_plot",
					  .description = "Add variable(s) to a plot. Single: 'variable' (name string). Batch: 'variables' (JSON array of name strings) -> JSON array of {name, status: 'added'|'already_in_plot'|'not_found'}.",
					  .inputSchema = objectSchema(Json::object({{"group", stringProperty("Group name (omit for the active group)")},
																{"plot", stringProperty("Plot name")},
																{"variable", stringProperty("Variable name (single mode)")},
																{"variables", arrayProperty("JSON array of variable names (batch mode)", stringProperty("Variable name"))}}),
												  {"plot"}),
					  .handler = addVariableToPlot});
	}
}  // namespace mcp
